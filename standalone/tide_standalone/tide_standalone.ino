// tide_standalone.ino -- a tide chart that computes itself.
//
// Every hour the board wakes from deep sleep and:
//   1. checks the battery; nearly empty -> "charge me" screen, stop
//   2. asks plan.h whether anything needs downloading:
//        NOAA predictions for Santa Cruz, about a month   (daily)
//        Monterey's live gauge, for the offset and dots   (every 3 h)
//      and if so, turns WiFi on, syncs the clock, downloads, turns it off
//   3. draws the chart from what it has and refreshes the panel
//   4. sleeps until just after the next top of the hour
//
// What it remembers between wakes (the predictions, the Monterey readings,
// when it last fetched) lives in RTC memory, which survives deep sleep but
// not a reset or power loss -- after one, it just downloads again.
//
// Before flashing: copy secrets.h.example to secrets.h and fill in WiFi.
// Settings are in config.h. Serial monitor at 115200 shows what it's doing.

#include <esp_sleep.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"
#include "battery.h"
#include "canvas.h"
#include "chart.h"
#include "net.h"
#include "panel.h"
#include "plan.h"
#include "tide_core.h"

// ---- memory that survives deep sleep ------------------------------------

const uint32_t CACHE_MAGIC = 0x71DE0003;   // bump when Cache's layout changes
const int MAX_EX = 160;                   // ~31 days of highs and lows, plus slack
const int MAX_AN = 90;                    // 6.5 h of 6-minute readings, plus slack

struct Cache {
  uint32_t magic;
  epoch_t  preds_fetched;                // last success
  epoch_t  preds_attempted;              // last try
  epoch_t  offset_attempted;             // last Monterey try
  epoch_t  wifi_attempted;
  int32_t  wifi_failures;                 // consecutive WiFi / clock failures
  bool     cutoff;                        // showing "charge me", waiting
  uint16_t n_ex, n_an;
  Extreme  ex[MAX_EX];
  Anomaly  an[MAX_AN];
};
RTC_DATA_ATTR Cache cache;

// Anything before 2025 means the clock hasn't been set since power-on.
const epoch_t CLOCK_VALID = 1735689600;   // 2025-01-01

// ---- helpers -------------------------------------------------------------

static void sleep_for(uint32_t seconds) {
  Serial.printf("sleeping %lu s\n", (unsigned long)seconds);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

static epoch_t now_utc() { return (epoch_t)time(nullptr); }

// "YYYYMMDD" or "YYYYMMDD%20HH:MM" in UTC, for NOAA's begin/end_date.
static String noaa_date(epoch_t t, bool with_time) {
  int y, m, d, hh, mm;
  utc_fields(t, &y, &m, &d, &hh, &mm);
  char buf[24];
  if (with_time) snprintf(buf, sizeof(buf), "%04d%02d%02d%%20%02d:%02d", y, m, d, hh, mm);
  else snprintf(buf, sizeof(buf), "%04d%02d%02d", y, m, d);
  return String(buf);
}

static String noaa_url(const char *station, const char *product, const char *extra,
                       const String &begin, const String &end) {
  return String("https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?station=") +
         station + "&product=" + product + extra + "&begin_date=" + begin +
         "&end_date=" + end +
         "&datum=MLLW&units=english&time_zone=gmt&format=csv&application=tide_display";
}

// Frame buffers are allocated only after WiFi is off, when memory is free.
static void show(Canvas &canvas) {
#if PANEL_GRAY4
  panel_show(canvas.data(), true);
#else
  uint8_t *bits = (uint8_t *)malloc(canvas.width() * canvas.height() / 8);
  if (!bits) { Serial.println("no memory for 1-bit frame"); return; }
  canvas.to_1bit(bits);
  panel_show(bits, false);
  free(bits);
#endif
}

// ---- downloads -----------------------------------------------------------

static bool fetch_predictions(epoch_t now) {
  const String url = noaa_url(STATION_ID, "predictions", "&interval=hilo",
                              noaa_date(now - 86400, false),
                              noaa_date(now + PREDS_DAYS * 86400LL, false));
  String body;
  if (!http_get(url, body)) return false;
  static Extreme fresh[MAX_EX];
  const size_t n = parse_noaa_csv(body.c_str(), fresh, MAX_EX);
  Serial.printf("predictions: %u highs and lows\n", (unsigned)n);
  if (n < 4) {
    Serial.println(body.substring(0, 200));   // NOAA's error text, if any
    return false;
  }
  memcpy(cache.ex, fresh, n * sizeof(Extreme));
  cache.n_ex = (uint16_t)n;
  cache.preds_fetched = now;
  return true;
}

static bool fetch_offset(epoch_t now) {
  const String begin = noaa_date(now - 6 * 3600 - 1800, true);
  const String end = noaa_date(now, true);
  String obs_body, pred_body;
  if (!http_get(noaa_url(OFFSET_STATION, "water_level", "", begin, end), obs_body)) return false;
  if (!http_get(noaa_url(OFFSET_STATION, "predictions", "&interval=6", begin, end), pred_body))
    return false;

  static epoch_t ot[MAX_AN], pt[MAX_AN];
  static float ov[MAX_AN], pv[MAX_AN];
  const size_t no = parse_noaa_series(obs_body.c_str(), ot, ov, MAX_AN);
  const size_t np = parse_noaa_series(pred_body.c_str(), pt, pv, MAX_AN);
  static Anomaly fresh[MAX_AN];
  const size_t na = match_anomalies(ot, ov, no, pt, pv, np, fresh, MAX_AN);
  Serial.printf("monterey: %u measured, %u predicted, %u matched\n",
                (unsigned)no, (unsigned)np, (unsigned)na);
  if (na < (size_t)OFFSET_MIN_POINTS) {
    if (no == 0) Serial.println(obs_body.substring(0, 200));
    return false;
  }
  memcpy(cache.an, fresh, na * sizeof(Anomaly));
  cache.n_an = (uint16_t)na;
  return true;
}

// ---- the wake --------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  setenv("TZ", TIMEZONE, 1);      // environment doesn't survive deep sleep
  tzset();

  if (cache.magic != CACHE_MAGIC) {
    Serial.println("fresh start: cache cleared");
    memset(&cache, 0, sizeof(cache));
    cache.magic = CACHE_MAGIC;
  }

  // 1. Battery. The cutoff keeps a LiPo from being run flat, which ruins it
  //    (and is how the last one swelled).
  const int mv = battery_mv();
  const int pct = mv > 0 ? lipo_percent(mv) : -1;
  if (mv > 0) Serial.printf("battery: %d mV (~%d%%)\n", mv, pct);
  if (mv > 0 && cache.cutoff) {
    if (mv < BATTERY_RESUME_MV) sleep_for(CUTOFF_CHECK_H * 3600);
    Serial.println("battery recovered: resuming");
    cache.cutoff = false;
  } else if (mv > 0 && mv < BATTERY_CUTOFF_MV) {
    Serial.println("battery empty: charge-me screen, then idling");
    Canvas canvas(800, 480);
    if (canvas.ok()) {
      draw_charge_me(canvas, STATION_NAME, PANEL_GRAY4);
      show(canvas);
    }
    cache.cutoff = true;
    sleep_for(CUTOFF_CHECK_H * 3600);
  }

  // 2. Downloads, if anything's due.
  epoch_t now = now_utc();
  bool clock_ok = now > CLOCK_VALID;
  struct tm lt;
  time_t tt = (time_t)now;
  localtime_r(&tt, &lt);

  WakeState st;
  st.now = now;
  st.clock_ok = clock_ok;
  st.local_hour = lt.tm_hour;
  st.preds_fetched = cache.preds_fetched;
  st.preds_attempted = cache.preds_attempted;
  st.preds_end = cache.n_ex ? cache.ex[cache.n_ex - 1].t : 0;
  st.offset_attempted = cache.offset_attempted;
  st.wifi_attempted = cache.wifi_attempted;
  st.wifi_failures = cache.wifi_failures;

  const Settings cfg = {OFFSET_ENABLED != 0, OFFSET_EVERY_H, QUIET_START, QUIET_END,
                        PREDS_EVERY_H * 3600LL, 7 * 86400LL, 3 * 3600LL};
  const WakePlan plan = plan_wake(st, cfg);
  Serial.printf("plan: predictions %s, monterey %s\n", plan.fetch_preds ? "yes" : "no",
                plan.fetch_offset ? "yes" : "no");

  if (plan.wifi()) {
    cache.wifi_attempted = now;
    const bool connected = wifi_connect();
    if (connected && ntp_sync()) {
      now = now_utc();
      clock_ok = true;
    }
    // Downloads need WiFi and a clock (NOAA is asked for dates). If either
    // is missing, back off (see plan.h); the cache covers weeks meanwhile.
    const bool can_fetch = connected && clock_ok;
    cache.wifi_failures = can_fetch ? 0 : cache.wifi_failures + 1;
    if (can_fetch && plan.fetch_preds) {
      cache.preds_attempted = now;
      if (!fetch_predictions(now)) Serial.println("predictions: failed, retry in 3 h");
    }
    if (can_fetch && plan.fetch_offset) {
      cache.offset_attempted = now;
      if (!fetch_offset(now)) Serial.println("monterey: failed, retry next interval");
    }
    wifi_off();
  }

  // 3. Draw. The canvas (96 KB) is allocated now that WiFi's memory is free.
  now = now_utc();
  Canvas canvas(800, 480);
  if (!canvas.ok()) {
    Serial.println("no memory for the canvas");
    sleep_for(next_sleep_s(now, clock_ok));
  }

  float h;
  if (!clock_ok || !height_at(cache.ex, cache.n_ex, now, &h)) {
    draw_message(canvas, "Waiting for tide data",
                 clock_ok ? "No predictions for right now yet." : "Couldn't reach WiFi yet.",
                 "It will keep trying on its own.", PANEL_GRAY4);
  } else {
    ChartInput in;
    in.extremes = cache.ex;
    in.n = cache.n_ex;
    in.station_name = STATION_NAME;
    in.now = now / 60 * 60;
    in.window_lo = in.now;
    in.window_hi = in.now + 70 * 60;          // until the next redraw, and a bit
    in.gray4 = PANEL_GRAY4;
    in.battery_pct = pct;
    in.battery_warn_pct = BATTERY_WARN_PCT;

    static epoch_t trace_t[MAX_AN * 2];
    static float trace_h[MAX_AN * 2];
    float off;
    if (OFFSET_ENABLED &&
        weather_offset(cache.an, cache.n_an, now, OFFSET_MAX_AGE_H * 3600LL, &off)) {
      in.offset_ft = off;
      in.trace_n = measured_trace(cache.an, cache.n_an, cache.ex, cache.n_ex,
                                  in.now - 6 * 3600, trace_t, trace_h, MAX_AN * 2);
      in.trace_t = trace_t;
      in.trace_h = trace_h;
      Serial.printf("offset: %+.2f ft, %u trace points\n", off, (unsigned)in.trace_n);
    }
    draw_chart(canvas, in);
  }
  show(canvas);

  // 4. Sleep until just after the next top of the hour.
  sleep_for(next_sleep_s(now_utc(), clock_ok));
}

void loop() {}   // never reached: every wake ends in deep sleep
