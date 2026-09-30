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

// "2026-09-29 10:52,-0.301,L" -> one Extreme. Strict about the date so the
// header line and junk are rejected; lenient about spaces around fields.
static bool parse_line(const char *s, const char *end, Extreme *e) {
  while (s < end && (*s == ' ' || *s == '\t')) s++;
  if (end - s < 16) return false;
  int f[5];
  const int pos[5] = {0, 5, 8, 11, 14};
  const int len[5] = {4, 2, 2, 2, 2};
  for (int i = 0; i < 5; i++) {
    int v = 0;
    for (int k = 0; k < len[i]; k++) {
      char c = s[pos[i] + k];
      if (c < '0' || c > '9') return false;
      v = v * 10 + (c - '0');
    }
    f[i] = v;
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
  double h = strtod(num, &stop);
  if (stop == num) return false;
  if (p >= end) return false;
  p++;

  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p >= end || (*p != 'H' && *p != 'L')) return false;

  e->t = epoch_from_utc(f[0], f[1], f[2], f[3], f[4]);
  e->h = (float)h;
  e->type = *p;
  return true;
}

size_t parse_noaa_csv(const char *text, Extreme *out, size_t max_out) {
  size_t n = 0;
  const char *s = text;
  while (*s && n < max_out) {
    const char *eol = s;
    while (*eol && *eol != '\n') eol++;
    const char *end = eol;
    if (end > s && end[-1] == '\r') end--;
    Extreme e;
    if (parse_line(s, end, &e)) {
      // NOAA returns time order already; insist on it so height_at's
      // assumptions hold even if a response ever arrives shuffled.
      size_t i = n;
      while (i > 0 && out[i - 1].t > e.t) { out[i] = out[i - 1]; i--; }
      out[i] = e;
      n++;
    }
    s = *eol ? eol + 1 : eol;
  }
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
