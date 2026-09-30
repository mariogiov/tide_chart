// sim_main.cpp -- run the real sketch through days of simulated wakes.
//
// The sketch, and everything it includes, is compiled unmodified; only the
// Arduino, ESP32 and Waveshare functions underneath are stand-ins (stubs/).
// NOAA is faked with synthetic tides, the clock is simulated, and deep
// sleep jumps the clock forward. Frames the panel would show are saved as
// BMPs. Scenarios:
//     ./sim                  3 normal days
//     ./sim wifi-outage      WiFi down for 30 hours from hour 20
//     ./sim noaa-errors      Monterey gauge returns no data for a day
//     ./sim offline-start    no WiFi for the first 5 hours after flashing
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "Arduino.h"
#include "WiFi.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "EPD.h"
#include "tide_core.h"

void setup();

int64_t sim_clock = 0;           // what the device's clock says
uint32_t sim_millis = 0;
SerialStub Serial;
WiFiStub WiFi;

static int64_t true_time;        // the real (simulated) time
static bool clock_set = false;   // has NTP ever set the device clock
static bool wifi_up = true;
static bool gauge_up = true;
static uint64_t sleep_us = 0;
static int wakes = 0, wifi_joins = 0, frames = 0;
static std::vector<std::string> events;

struct Sleep {};

time_t sim_time(time_t *out) {
  time_t t = (time_t)sim_clock;
  if (out) *out = t;
  return t;
}
int WiFiStub::status() { return wifi_up ? (wifi_joins++, WL_CONNECTED) : WL_DISCONNECTED; }
void configTzTime(const char *tz, const char *, const char *) { setenv("TZ", tz, 1); tzset(); }
int sntp_get_sync_status() {
  if (!wifi_up) return SNTP_SYNC_STATUS_RESET;
  sim_clock = true_time;
  clock_set = true;
  return SNTP_SYNC_STATUS_COMPLETED;
}
void esp_sleep_enable_timer_wakeup(uint64_t us) { sleep_us = us; }
void esp_deep_sleep_start() { throw Sleep(); }

// ---- fake NOAA -------------------------------------------------------------

// A semidiurnal tide with a diurnal inequality, anchored at 2026-09-29.
static double true_tide(int64_t t, double scale) {
  const double hrs = (double)(t - 1790640000) / 3600.0;
  return scale * (2.6 + 1.8 * cos(2 * M_PI * hrs / 12.42) + 0.7 * cos(2 * M_PI * hrs / 24.84 + 1.0));
}

static std::string param(const std::string &url, const std::string &key) {
  size_t p = url.find(key + "=");
  if (p == std::string::npos) return "";
  p += key.size() + 1;
  size_t e = url.find('&', p);
  return url.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static int64_t parse_date(const std::string &s) {       // YYYYMMDD[%20HH:MM]
  int Y = atoi(s.substr(0, 4).c_str()), M = atoi(s.substr(4, 2).c_str()), D = atoi(s.substr(6, 2).c_str());
  int h = 0, m = 0;
  if (s.size() >= 16) { h = atoi(s.substr(11, 2).c_str()); m = atoi(s.substr(14, 2).c_str()); }
  return epoch_from_utc(Y, M, D, h, m);
}

static std::string stamp(int64_t t) {
  int Y, M, D, h, m;
  utc_fields(t, &Y, &M, &D, &h, &m);
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", Y, M, D, h, m);
  return b;
}

String sim_http(const String &u, int *code) {
  const std::string url = u.c_str();
  const std::string product = param(url, "product"), interval = param(url, "interval");
  const std::string station = param(url, "station");
  int64_t begin = parse_date(param(url, "begin_date")), end = parse_date(param(url, "end_date"));
  if (param(url, "end_date").size() == 8) end += 86400 - 60;   // whole days: through 23:59
  *code = 200;
  events.push_back("GET " + station + " " + product + (interval.empty() ? "" : "/" + interval));
  std::string out;
  if (product == "predictions" && interval == "hilo") {
    out = "Date Time, Prediction, Type\n";
    // Find turning points of the true tide by sampling each minute.
    double prev = true_tide(begin - 60, 1), cur = true_tide(begin, 1);
    for (int64_t t = begin; t <= end; t += 60) {
      const double next = true_tide(t + 60, 1);
      if ((cur > prev && cur >= next) || (cur < prev && cur <= next)) {
        char b[64];
        snprintf(b, sizeof(b), "%s,%.3f,%c\n", stamp(t).c_str(), cur, cur > prev ? 'H' : 'L');
        out += b;
      }
      prev = cur;
      cur = next;
    }
  } else if (product == "water_level") {
    if (!gauge_up) return String("Error: No data was found. This product may not be offered at this station at the requested time.");
    out = "Date Time, Water Level, Sigma, O or I (for verified), F, R, L, Quality\n";
    for (int64_t t = (begin + 359) / 360 * 360; t <= end && t <= true_time - 360; t += 360) {
      if ((t / 360) % 23 == 0) { out += stamp(t) + ",,,,,,,\n"; continue; }   // occasional hole
      char b[96];
      const double surge = 0.35 + 0.1 * sin((double)t / 20000.0);
      snprintf(b, sizeof(b), "%s,%.3f,0.020,0,0,0,0,p\n", stamp(t).c_str(), true_tide(t, 1.05) + surge);
      out += b;
    }
  } else if (product == "predictions") {
    out = "Date Time, Prediction\n";
    for (int64_t t = (begin + 359) / 360 * 360; t <= end; t += 360) {
      char b[64];
      snprintf(b, sizeof(b), "%s,%.3f\n", stamp(t).c_str(), true_tide(t, 1.05));
      out += b;
    }
  } else {
    *code = 400;
  }
  return String(out);
}

// ---- fake panel --------------------------------------------------------------

static const char *scenario = "normal";
static bool save_frames = true;

static void save_bmp(const uint8_t *frame, bool gray4) {
  frames++;
  if (!save_frames) return;
  char path[128];
  snprintf(path, sizeof(path), "out_%s/frame_%03d.bmp", scenario, frames);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  const int W = 800, H = 480, row = W * 3;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hdr[o + i] = (v >> (8 * i)) & 255; };
  put32(2, 54 + row * H); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
  hdr[26] = 1; hdr[28] = 24; put32(34, row * H);
  fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> line(row);
  static const uint8_t ramp[4] = {0, 85, 170, 255};
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      uint8_t g;
      if (gray4) g = ramp[(frame[(y * W + x) / 4] >> (6 - 2 * (x & 3))) & 3];
      else g = (frame[(y * W + x) / 8] & (0x80 >> (x & 7))) ? 255 : 0;
      line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = g;
    }
    fwrite(line.data(), 1, row, f);
  }
  fclose(f);
}
void DEV_Module_Init() {}
void EPD_7IN5_V2_Init() {}
void EPD_7IN5_V2_Init_4Gray() {}
void EPD_7IN5_V2_Display(UBYTE *b) { save_bmp(b, false); }
void EPD_7IN5_V2_Display_4Gray(UBYTE *b) { save_bmp(b, true); }
void EPD_7IN5_V2_Sleep() {}

// ---- the run -------------------------------------------------------------------

int main(int argc, char **argv) {
  if (argc > 1) scenario = argv[1];
  char dir[64];
  snprintf(dir, sizeof(dir), "mkdir -p out_%s", scenario);
  if (system(dir) != 0) return 1;

  true_time = epoch_from_utc(2026, 9, 29, 21, 17);       // flashed at 2:17 PM PDT
  sim_clock = 0;                                          // power-on: clock unset
  const double DRIFT = 1.003;                             // RTC runs 0.3% slow
  const int HOURS = 72;

  int wifi_per_day[4] = {0};
  for (wakes = 1; true_time < epoch_from_utc(2026, 9, 29, 21, 17) + HOURS * 3600; wakes++) {
    const double hrs = (double)(true_time - epoch_from_utc(2026, 9, 29, 21, 17)) / 3600.0;
    if (!strcmp(scenario, "wifi-outage")) wifi_up = !(hrs >= 20 && hrs < 50);
    if (!strcmp(scenario, "noaa-errors")) gauge_up = !(hrs >= 10 && hrs < 34);
    if (!strcmp(scenario, "offline-start")) wifi_up = hrs >= 5;
    const int joins_before = wifi_joins;
    events.clear();
    printf("\n=== wake %d, true %s UTC (device clock %s) ===\n", wakes, stamp(true_time).c_str(),
           clock_set ? "set" : "unset");
    try {
      setup();
      printf("!! setup returned without sleeping\n");
      return 1;
    } catch (Sleep &) {
    }
    if (wifi_joins > joins_before) wifi_per_day[(int)(hrs / 24)]++;
    for (auto &e : events) printf("   [%s]\n", e.c_str());
    const int64_t slept = (int64_t)(sleep_us / 1000000);
    const int64_t real = (int64_t)(slept * DRIFT) + 4;    // +4 s awake time
    true_time += real;
    // The device clock is kept by the same slow RTC that timed the sleep,
    // so it thinks only `slept` passed: it falls behind until NTP fixes it.
    sim_clock += slept + 4;
    if (wakes > 200) { printf("too many wakes\n"); return 1; }
  }
  printf("\nSUMMARY %s: %d wakes, %d frames, WiFi wakes per day: %d %d %d\n", scenario, wakes - 1,
         frames, wifi_per_day[0], wifi_per_day[1], wifi_per_day[2]);
  return 0;
}
