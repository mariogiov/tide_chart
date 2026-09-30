// test.cpp -- checks for the tide math, parsing and time handling.
// `make test` builds and runs it; any failure prints and exits nonzero.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "canvas.h"
#include "plan.h"
#include "tide_core.h"

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } \
  } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

int main() {
  // --- calendar ---------------------------------------------------------
  CHECK(epoch_from_utc(1970, 1, 1, 0, 0) == 0);
  CHECK(epoch_from_utc(2000, 3, 1, 0, 0) == 951868800);        // leap-year edge
  CHECK(epoch_from_utc(2026, 9, 29, 21, 30) == 1790717400);
  CHECK(epoch_from_utc(2038, 1, 19, 3, 14) == 2147483640);     // past 32-bit time_t

  // --- CSV parsing ------------------------------------------------------
  const char *csv =
      "Date Time, Prediction, Type\r\n"
      "2026-09-29 10:52,-0.301,L\r\n"
      "\r\n"
      "2026-09-29 17:05, 3.912,H\r\n"
      "garbage line\n"
      "2026-09-30 02:40,1.4,L";                                   // no final newline
  Extreme ex[10];
  size_t n = parse_noaa_csv(csv, ex, 10);
  CHECK(n == 3);
  CHECK(ex[0].t == epoch_from_utc(2026, 9, 29, 10, 52));
  NEAR(ex[0].h, -0.301, 1e-6);
  CHECK(ex[0].type == 'L' && ex[1].type == 'H' && ex[2].type == 'L');
  NEAR(ex[1].h, 3.912, 1e-6);
  CHECK(parse_noaa_csv(csv, ex, 2) == 2);                         // respects max
  CHECK(parse_noaa_csv("", ex, 10) == 0);
  // Out of order input comes back sorted.
  n = parse_noaa_csv("2026-09-29 17:05,3.9,H\n2026-09-29 10:52,-0.3,L\n", ex, 10);
  CHECK(n == 2 && ex[0].type == 'L' && ex[1].type == 'H');

  // --- interpolation ----------------------------------------------------
  Extreme e[3] = {{0, 0.0f, 'L'}, {6 * 3600, 4.0f, 'H'}, {12 * 3600, 1.0f, 'L'}};
  float h;
  CHECK(height_at(e, 3, 0, &h)); NEAR(h, 0.0, 1e-6);
  CHECK(height_at(e, 3, 6 * 3600, &h)); NEAR(h, 4.0, 1e-6);
  CHECK(height_at(e, 3, 3 * 3600, &h)); NEAR(h, 2.0, 1e-5);        // midpoint
  CHECK(height_at(e, 3, 9 * 3600, &h)); NEAR(h, 2.5, 1e-5);
  CHECK(height_at(e, 3, 1 * 3600, &h)); NEAR(h, 4.0 * (1 - cos(M_PI / 6)) / 2, 1e-5);
  CHECK(!height_at(e, 3, -1, &h));                                 // unbracketed
  CHECK(!height_at(e, 3, 12 * 3600 + 1, &h));

  // --- summarize --------------------------------------------------------
  Extreme s[6] = {{0, 5, 'H'}, {6 * 3600, 0, 'L'}, {12 * 3600, 4, 'H'},
                  {18 * 3600, 1, 'L'}, {24 * 3600, 5, 'H'}, {30 * 3600, 0, 'L'}};
  // Between extremes: falling toward the low at 6h.
  Summary r = summarize(s, 6, 3 * 3600, 3 * 3600, 3 * 3600 + 4200);
  CHECK(r.has_current && !r.turning && r.rising == 0);
  CHECK(r.prev == &s[0] && r.n_next == 3 && r.next[0] == &s[1]);
  // A low inside the display window: turning, NOW slot, no direction.
  r = summarize(s, 6, 5 * 3600 + 1800, 5 * 3600 + 1800, 5 * 3600 + 1800 + 4200);
  CHECK(r.turning == &s[1] && r.rising == -1 && r.prev == &s[0] && r.next[0] == &s[2]);
  // Rising after the low has passed.
  r = summarize(s, 6, 7 * 3600, 7 * 3600, 7 * 3600 + 4200);
  CHECK(r.rising == 1 && r.prev == &s[1]);

  // --- local time across DST ------------------------------------------
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
  struct tm lt;
  time_t t = (time_t)epoch_from_utc(2026, 9, 29, 21, 30);          // PDT, UTC-7
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 14 && lt.tm_min == 30);
  t = (time_t)epoch_from_utc(2026, 12, 1, 21, 30);                 // PST, UTC-8
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 13);
  t = (time_t)epoch_from_utc(2027, 3, 14, 10, 0);                  // 3 AM PDT, just after spring-forward
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 3);

  // --- canvas -----------------------------------------------------------
  Canvas c(8, 4);
  CHECK(c.ok() && c.bytes() == 8);
  c.set(0, 0, BLACK);
  c.set(5, 1, LIGHT);
  CHECK(c.get(0, 0) == BLACK && c.get(1, 0) == WHITE && c.get(5, 1) == LIGHT);
  CHECK(c.data()[0] == 0x3F);            // leftmost pixel in the high bits
  CHECK(c.data()[3] == 0xEF);            // row 1, pixels 4-7: W L W W
  uint8_t bits[4];
  c.to_1bit(bits);
  CHECK((bits[0] & 0x80) == 0);          // black stays black
  CHECK((bits[0] & 0x40) != 0);          // white stays white

  // --- UTC fields round trip ------------------------------------------
  {
    int Y, M, D, hh, mm;
    utc_fields(epoch_from_utc(2026, 9, 29, 21, 30), &Y, &M, &D, &hh, &mm);
    CHECK(Y == 2026 && M == 9 && D == 29 && hh == 21 && mm == 30);
    utc_fields(epoch_from_utc(2028, 2, 29, 0, 0), &Y, &M, &D, &hh, &mm);
    CHECK(Y == 2028 && M == 2 && D == 29 && hh == 0);
    utc_fields(epoch_from_utc(2026, 12, 31, 23, 59), &Y, &M, &D, &hh, &mm);
    CHECK(Y == 2026 && M == 12 && D == 31 && hh == 23 && mm == 59);
  }

  // --- series parsing (Monterey water levels / 6-min predictions) --------
  {
    const char *wl =
        "Date Time, Water Level, Sigma, O or I (for verified), F, R, L, Quality\n"
        "2026-09-29 14:00,3.281,0.023,0,0,0,0,p\n"
        "2026-09-29 14:06,,,,,,,\n"                     // hole in the data
        "2026-09-29 14:12,3.402,0.020,0,0,0,0,p\n";
    epoch_t t[8];
    float v[8];
    size_t k = parse_noaa_series(wl, t, v, 8);
    CHECK(k == 2);
    CHECK(t[1] == epoch_from_utc(2026, 9, 29, 14, 12));
    NEAR(v[0], 3.281, 1e-6);
    CHECK(parse_noaa_series("Error: No data was found.", t, v, 8) == 0);
    CHECK(parse_noaa_series("Date Time, Prediction\n2026-09-29 14:00,3.1\n", t, v, 8) == 1);
  }

  // --- anomalies, offset, trace ---------------------------------------
  {
    const epoch_t t0 = epoch_from_utc(2026, 9, 29, 12, 0);
    epoch_t ot[80], pt[80];
    float ov[80], pv[80];
    size_t no = 0, np = 0;
    for (int i = 0; i < 61; i++) {                 // 6 hours of 6-min data
      pt[np] = t0 + i * 360; pv[np++] = 2.0f + 0.01f * i;
      if (i >= 20 && i < 27) continue;             // 42-minute hole in obs
      ot[no] = t0 + i * 360; ov[no++] = 2.0f + 0.01f * i + 0.3f;
    }
    Anomaly an[80];
    size_t na = match_anomalies(ot, ov, no, pt, pv, np, an, 80);
    CHECK(na == no);
    NEAR(an[0].a, 0.3, 1e-5);
    const epoch_t newest = t0 + 60 * 360;
    float off;
    CHECK(weather_offset(an, na, newest + 600, 3 * 3600, &off)); NEAR(off, 0.3, 1e-5);
    CHECK(!weather_offset(an, na, newest + 4 * 3600, 3 * 3600, &off));   // stale
    CHECK(!weather_offset(an, 3, t0 + 1000, 3600, &off));                // too few
    an[na - 2].a = 9.0f;                                                 // one spike
    CHECK(weather_offset(an, na, newest, 3600, &off)); NEAR(off, 0.3, 1e-5);
    an[na - 2].a = 0.3f;

    Extreme ex[3] = {{t0 - 3600, 1.0f, 'L'}, {t0 + 5 * 3600, 4.0f, 'H'}, {t0 + 11 * 3600, 0.5f, 'L'}};
    epoch_t tt[100];
    float th[100];
    size_t nt = measured_trace(an, na, ex, 3, t0, tt, th, 100);
    int gaps = 0;
    for (size_t i = 0; i < nt; i++) gaps += isnan(th[i]) ? 1 : 0;
    CHECK(gaps == 1);                              // the 42-minute hole
    float base;
    height_at(ex, 3, tt[0], &base);
    NEAR(th[0] - base, 0.3, 1e-5);
    CHECK(measured_trace(an, 3, ex, 3, t0, tt, th, 100) == 0);           // too short
  }

  // --- wake planning ----------------------------------------------------
  {
    CHECK(!is_quiet_hour(3, 5, 5));                 // equal = never quiet
    CHECK(is_quiet_hour(23, 23, 4) && is_quiet_hour(3, 23, 4));
    CHECK(!is_quiet_hour(4, 23, 4) && !is_quiet_hour(22, 23, 4));
    CHECK(is_quiet_hour(1, 1, 5) && !is_quiet_hour(5, 1, 5));
    CHECK(wifi_backoff_s(0) == 0 && wifi_backoff_s(1) == 3600 && wifi_backoff_s(3) == 4 * 3600);
    CHECK(wifi_backoff_s(10) == 12 * 3600);

    Settings cfg = {true, 3, 0, 0, 24 * 3600, 7 * 86400, 3 * 3600};
    const epoch_t now = epoch_from_utc(2026, 9, 29, 20, 0);
    WakeState s = {};
    s.now = now; s.clock_ok = true; s.local_hour = 13;
    s.preds_fetched = now - 3600; s.preds_end = now + 20 * 86400;
    s.offset_attempted = now - 3600;
    WakePlan p = plan_wake(s, cfg);
    CHECK(!p.fetch_preds && !p.fetch_offset && !p.wifi());        // nothing due yet
    s.offset_attempted = now - 3 * 3600 + 120;                   // 2 min early: still due
    CHECK(plan_wake(s, cfg).fetch_offset);
    s.offset_attempted = now - 3600;
    s.preds_fetched = now - 25 * 3600;                           // a day old
    CHECK(plan_wake(s, cfg).fetch_preds);
    s.preds_attempted = now - 3600;                              // ...but just failed
    CHECK(!plan_wake(s, cfg).fetch_preds);
    s.preds_attempted = now - 3 * 3600;                          // retry after 3 h
    CHECK(plan_wake(s, cfg).fetch_preds);
    s.preds_attempted = 0;
    s.preds_fetched = now - 3600; s.preds_end = now + 3 * 86400; // cache running out
    CHECK(plan_wake(s, cfg).fetch_preds);
    s.preds_end = now + 20 * 86400;
    s.clock_ok = false;                                          // after a reset
    p = plan_wake(s, cfg);
    CHECK(p.fetch_preds && p.fetch_offset);
    s.clock_ok = true;
    cfg.quiet_start = 23; cfg.quiet_end = 4; s.local_hour = 2; s.offset_attempted = now - 5 * 3600;
    CHECK(!plan_wake(s, cfg).fetch_offset);                      // quiet hours on
    cfg.offset_enabled = false; s.local_hour = 13;
    CHECK(!plan_wake(s, cfg).fetch_offset);                      // feature off
    cfg.offset_enabled = true;
    s.preds_fetched = 0; s.wifi_failures = 2; s.wifi_attempted = now - 3600;
    CHECK(!plan_wake(s, cfg).wifi());                            // backing off (2 h)
    s.wifi_attempted = now - 2 * 3600;
    CHECK(plan_wake(s, cfg).wifi());

    CHECK(next_sleep_s(epoch_from_utc(2026, 9, 29, 20, 0), true) == 3603);
    CHECK(next_sleep_s(epoch_from_utc(2026, 9, 29, 20, 30), true) == 1803);
    CHECK(next_sleep_s(epoch_from_utc(2026, 9, 29, 20, 57), true) == 180 + 3600 + 3);
    CHECK(next_sleep_s(12345, false) == 3600);

    CHECK(lipo_percent(4250) == 100 && lipo_percent(4200) == 100);
    CHECK(lipo_percent(3800) == 45 && lipo_percent(3300) == 0);
    CHECK(lipo_percent(3850) > 45 && lipo_percent(3850) < 62);
  }

  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
