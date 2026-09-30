#include "tide_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Days since 1970-01-01 for a proleptic Gregorian date. Howard Hinnant's
// days_from_civil: exact for any year, no tables, no locale.
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

epoch_t epoch_from_utc(int year, int month, int day, int hour, int minute) {
  return days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 +
         hour * 3600 + minute * 60;
}

// Civil date from days since 1970 -- the inverse of days_from_civil.
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

void utc_fields(epoch_t t, int *year, int *month, int *day, int *hour, int *minute) {
  int64_t days = t / 86400, secs = t % 86400;
  if (secs < 0) { secs += 86400; days--; }
  civil_from_days(days, year, month, day);
  *hour = (int)(secs / 3600);
  *minute = (int)(secs % 3600 / 60);
}

// One CSV line of NOAA data: "2026-09-29 10:52,-0.301,..." Reads the time
// and the first value; `rest` is left pointing just past that value's comma
// (or at `end`). Strict about the date so headers and junk are rejected;
// an EMPTY value (a hole in real-time data) is also a rejection.
static bool parse_time_value(const char *s, const char *end, epoch_t *t,
                             double *v, const char **rest) {
  while (s < end && (*s == ' ' || *s == '\t')) s++;
  if (end - s < 16) return false;
  int f[5];
  const int pos[5] = {0, 5, 8, 11, 14};
  const int len[5] = {4, 2, 2, 2, 2};
  for (int i = 0; i < 5; i++) {
    int x = 0;
    for (int k = 0; k < len[i]; k++) {
      char c = s[pos[i] + k];
      if (c < '0' || c > '9') return false;
      x = x * 10 + (c - '0');
    }
    f[i] = x;
  }
  if (s[4] != '-' || s[7] != '-' || s[13] != ':') return false;
  if (f[1] < 1 || f[1] > 12 || f[2] < 1 || f[2] > 31 || f[3] > 23 || f[4] > 59)
    return false;

  const char *p = s + 16;
  while (p < end && *p != ',') p++;
  if (p >= end) return false;
  p++;

  char num[24];
  size_t k = 0;
  while (p < end && *p != ',' && k < sizeof(num) - 1) num[k++] = *p++;
  num[k] = 0;
  char *stop;
  const double x = strtod(num, &stop);
  if (stop == num) return false;              // empty or not a number
  while (*stop == ' ' || *stop == '\t') stop++;
  if (*stop) return false;                    // trailing junk in the field
  *t = epoch_from_utc(f[0], f[1], f[2], f[3], f[4]);
  *v = x;
  *rest = p < end ? p + 1 : end;
  return true;
}

// Calls `fn(line_start, line_end)` for each line, stripping a trailing \r.
template <typename F>
static void each_line(const char *text, F fn) {
  const char *s = text;
  while (*s) {
    const char *eol = s;
    while (*eol && *eol != '\n') eol++;
    const char *end = eol;
    if (end > s && end[-1] == '\r') end--;
    if (!fn(s, end)) return;
    s = *eol ? eol + 1 : eol;
  }
}

size_t parse_noaa_csv(const char *text, Extreme *out, size_t max_out) {
  size_t n = 0;
  if (max_out == 0) return 0;
  each_line(text, [&](const char *s, const char *end) {
    epoch_t t;
    double h;
    const char *p;
    if (!parse_time_value(s, end, &t, &h, &p)) return true;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || (*p != 'H' && *p != 'L')) return true;
    // NOAA returns time order already; insist on it so height_at's
    // assumptions hold even if a response ever arrives shuffled.
    size_t i = n;
    while (i > 0 && out[i - 1].t > t) { out[i] = out[i - 1]; i--; }
    out[i] = Extreme{t, (float)h, *p};
    return ++n < max_out;
  });
  return n;
}

size_t parse_noaa_series(const char *text, epoch_t *t, float *v, size_t max_out) {
  size_t n = 0;
  if (max_out == 0) return 0;
  each_line(text, [&](const char *s, const char *end) {
    epoch_t tt;
    double vv;
    const char *rest;
    if (!parse_time_value(s, end, &tt, &vv, &rest)) return true;
    t[n] = tt;
    v[n] = (float)vv;
    return ++n < max_out;
  });
  return n;
}

// Between an extreme (t1, h1) and the next (t2, h2) the tide is very close
// to a half-cosine:  h = h1 + (h2 - h1) * (1 - cos(pi * u)) / 2,  u in 0..1.
// Slope is zero at both ends -- the water is momentarily still at high and
// low tide. Same formula as tide_render.py.
bool height_at(const Extreme *ex, size_t n, epoch_t t, float *h) {
  if (n < 2 || t < ex[0].t || t > ex[n - 1].t) return false;
  size_t lo = 0, hi = n - 1;                 // binary search for the pair
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if (ex[mid].t <= t) lo = mid; else hi = mid;
  }
  const Extreme &a = ex[lo], &b = ex[hi];
  const double span = (double)(b.t - a.t);
  if (span <= 0) { *h = a.h; return true; }
  const double u = (double)(t - a.t) / span;
  *h = (float)(a.h + (b.h - a.h) * (1.0 - cos(M_PI * u)) / 2.0);
  return true;
}

Summary summarize(const Extreme *ex, size_t n, epoch_t now,
                  epoch_t window_lo, epoch_t window_hi) {
  Summary s;
  memset(&s, 0, sizeof(s));
  s.rising = -1;
  s.has_current = height_at(ex, n, now, &s.current);

  for (size_t i = 0; i < n; i++) {
    const Extreme *e = &ex[i];
    if (e->t < window_lo) {
      s.prev = e;
    } else if (e->t <= window_hi) {
      if (!s.turning) s.turning = e;
    } else if (s.n_next < 3) {
      s.next[s.n_next++] = e;
    }
  }
  // Outside a turn the direction can't flip while the image is up: it only
  // changes at an extreme, and there isn't one in the window.
  if (!s.turning && s.n_next > 0) s.rising = s.next[0]->type == 'H' ? 1 : 0;
  return s;
}

// ---- Monterey weather offset ---------------------------------------------

size_t match_anomalies(const epoch_t *obs_t, const float *obs_v, size_t n_obs,
                       const epoch_t *pred_t, const float *pred_v, size_t n_pred,
                       Anomaly *out, size_t max_out) {
  // Both series are 6-minute, in time order: walk them together.
  size_t i = 0, j = 0, n = 0;
  while (i < n_obs && j < n_pred && n < max_out) {
    if (obs_t[i] < pred_t[j]) i++;
    else if (obs_t[i] > pred_t[j]) j++;
    else { out[n++] = Anomaly{obs_t[i], obs_v[i] - pred_v[j]}; i++; j++; }
  }
  return n;
}

static int cmp_float(const void *a, const void *b) {
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

static float median(float *v, size_t n) {
  qsort(v, n, sizeof(float), cmp_float);
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

bool weather_offset(const Anomaly *an, size_t n, epoch_t now, epoch_t max_age,
                    float *out) {
  if (n == 0) return false;
  const epoch_t newest = an[n - 1].t;
  if (newest > now || now - newest > max_age) return false;
  float recent[64];
  size_t k = 0;
  for (size_t i = n; i-- > 0 && k < 64;) {
    if (an[i].t <= newest - OFFSET_AVERAGE_S) break;
    recent[k++] = an[i].a;
  }
  if (k < OFFSET_MIN_POINTS) return false;
  *out = median(recent, k);
  return true;
}

size_t measured_trace(const Anomaly *an, size_t n, const Extreme *ex, size_t ne,
                      epoch_t start, epoch_t *out_t, float *out_h, size_t max_out) {
  // Smooth with a centered rolling median of TRACE_SMOOTH readings, then
  // add each to Santa Cruz's own prediction at that moment. A NAN marks a
  // hole in the gauge data, so the dotted line lifts its pen there.
  const int half = TRACE_SMOOTH / 2;
  size_t first = 0;
  while (first < n && an[first].t < start) first++;
  if (n - first < (size_t)TRACE_SMOOTH) return 0;

  size_t k = 0;
  epoch_t prev = 0;
  for (size_t i = first; i < n && k + 2 <= max_out; i++) {
    float win[TRACE_SMOOTH];
    int w = 0;
    for (long j = (long)i - half; j <= (long)i + half; j++)
      if (j >= (long)first && j < (long)n) win[w++] = an[j].a;
    float base;
    if (!height_at(ex, ne, an[i].t, &base)) continue;
    if (k > 0 && an[i].t - prev > TRACE_MAX_GAP_S) {
      out_t[k] = prev + (an[i].t - prev) / 2;
      out_h[k++] = NAN;
    }
    out_t[k] = an[i].t;
    out_h[k++] = base + median(win, w);
    prev = an[i].t;
  }
  return k;
}
