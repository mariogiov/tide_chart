// plan.h -- what each wake-up does, and how long to sleep after.
//
// Pure arithmetic on times and settings: no WiFi, no clock reads, no
// hardware. The sketch fills in a WakeState from what it remembers across
// sleeps, asks plan_wake() what to do, and asks next_sleep_s() when to wake
// again. host/test.cpp checks every rule here on a laptop.
#pragma once
#include "tide_core.h"

// Everything the plan depends on. Times are UTC epoch seconds; 0 = never.
struct WakeState {
  epoch_t now;
  bool    clock_ok;          // false after a power-on or reset, until NTP
  int     local_hour;        // 0-23, only meaningful when clock_ok

  epoch_t preds_fetched;     // last SUCCESSFUL predictions download
  epoch_t preds_attempted;   // last try, success or not
  epoch_t preds_end;         // time of the last extreme we have
  epoch_t offset_attempted;  // last Monterey try, success or not

  epoch_t wifi_attempted;    // last time WiFi was tried
  int     wifi_failures;     // consecutive times WiFi or the clock failed
};

struct Settings {
  bool    offset_enabled;    // fetch Monterey's gauge for the offset/dots
  int     offset_every_h;    // hours between Monterey fetches
  int     quiet_start;       // no offset fetches from this hour...
  int     quiet_end;         // ...until this one. Equal = never quiet.
  epoch_t preds_every_s;     // refresh predictions this often
  epoch_t preds_min_ahead_s; // ...or sooner, if the cache runs this short
  epoch_t preds_retry_s;     // after a failed download, wait this long
};

struct WakePlan {
  bool fetch_preds;
  bool fetch_offset;
  bool wifi() const { return fetch_preds || fetch_offset; }
  // The clock is synced whenever WiFi is up anyway.
};

// Quiet hours can wrap past midnight (23 -> 4). Start is inclusive, end
// exclusive, so 23 -> 4 skips 11 PM through 3 AM and fetches at 4 AM.
// Equal values mean no quiet hours at all.
inline bool is_quiet_hour(int hour, int start, int end) {
  if (start == end) return false;
  if (start < end) return hour >= start && hour < end;
  return hour >= start || hour < end;
}

// After WiFi (or the time server) fails, wait 1 h, then 2, 4, 8, capped at
// 12, before trying again -- so a router that's down for a week costs a few
// attempts a day, not 24 twenty-second timeouts. A download that fails
// with WiFi working (NOAA hiccup, gauge offline) doesn't count: it just
// retries on its own schedule.
inline epoch_t wifi_backoff_s(int failures) {
  if (failures <= 0) return 0;
  epoch_t h = 1;
  for (int i = 1; i < failures && h < 12; i++) h *= 2;
  return (h > 12 ? 12 : h) * 3600;
}

inline WakePlan plan_wake(const WakeState &s, const Settings &cfg) {
  WakePlan p = {false, false};
  // Differences between two readings of the same clock are valid even
  // before NTP (it counts up from 1970 through deep sleep), so backoff
  // works with no real time too.
  const bool backing_off = s.wifi_failures > 0 &&
                           s.now - s.wifi_attempted < wifi_backoff_s(s.wifi_failures);

  // Predictions (and with them, the clock): needed if we've never had them,
  // the clock is unknown, they're a day old, or the cache is running out.
  const bool preds_due = !s.clock_ok || s.preds_fetched == 0 ||
                         s.now - s.preds_fetched >= cfg.preds_every_s ||
                         s.preds_end - s.now < cfg.preds_min_ahead_s;
  const bool preds_retry_ok = s.preds_attempted == 0 || !s.clock_ok ||
                              s.now - s.preds_attempted >= cfg.preds_retry_s - 300;
  p.fetch_preds = preds_due && preds_retry_ok;

  // Monterey: every few hours outside quiet hours, counted from the last
  // try -- a gauge that's offline gets asked again next interval, not every
  // hour. Five minutes of slack so a slightly early wake still counts.
  if (cfg.offset_enabled && s.clock_ok &&
      !is_quiet_hour(s.local_hour, cfg.quiet_start, cfg.quiet_end)) {
    p.fetch_offset = s.offset_attempted == 0 ||
                     s.now - s.offset_attempted >= cfg.offset_every_h * 3600 - 300;
  }
  // No clock at all: fetch everything once we're on WiFi anyway.
  if (!s.clock_ok && cfg.offset_enabled) p.fetch_offset = true;

  if (backing_off) p.fetch_preds = p.fetch_offset = false;
  return p;
}

// Sleep until just after the next top of the hour, so each redraw lands on
// the hour. If that's under 5 minutes away (we woke late, or the draw ran
// long), skip to the one after instead of waking twice in quick succession.
// Without a trustworthy clock, just sleep an hour.
inline uint32_t next_sleep_s(epoch_t now, bool clock_ok) {
  if (!clock_ok) return 3600;
  epoch_t into = now % 3600;
  if (into < 0) into += 3600;
  epoch_t wait = 3600 - into;
  if (wait < 300) wait += 3600;
  return (uint32_t)(wait + 3);    // a few seconds past, not before, the hour
}

// Rough state of charge for a single-cell LiPo from its resting voltage.
// Only approximate -- the curve varies by cell, age, temperature and load
// -- but good enough for "getting low". Returns 0-100.
inline int lipo_percent(int mv) {
  static const int table[][2] = {
      {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62}, {3800, 45},
      {3750, 35},  {3700, 22}, {3650, 12}, {3600, 6},  {3500, 2}, {3400, 0}};
  const int n = sizeof(table) / sizeof(table[0]);
  if (mv >= table[0][0]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv >= table[i][0]) {
      const int v0 = table[i][0], v1 = table[i - 1][0];
      const int p0 = table[i][1], p1 = table[i - 1][1];
      return p0 + (p1 - p0) * (mv - v0) / (v1 - v0);
    }
  }
  return 0;
}
