// tide_core.h -- the tide math. Plain C++, no Arduino, no hardware.
//
// Everything here runs identically on the ESP32 and on a laptop, which is
// what lets host/ test it and preview the chart without flashing anything.
//
// Times are seconds since 1970 in UTC ("epoch"). NOAA is asked for GMT, so
// nothing in this file knows about time zones or daylight saving; only the
// chart converts to local time, at the moment it prints a clock.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int64_t epoch_t;

struct Extreme {
  epoch_t t;       // when, UTC
  float   h;       // feet above MLLW
  char    type;    // 'H' or 'L'
};

// Parse NOAA's hilo predictions in CSV form (format=csv, time_zone=gmt):
//
//     Date Time, Prediction, Type
//     2026-09-29 10:52,-0.301,L
//     2026-09-29 17:05,3.912,H
//
// Lines that don't parse (the header, blanks, stray whitespace) are
// skipped. Returns how many extremes were written into `out`, at most
// `max_out`, in time order.
size_t parse_noaa_csv(const char *text, Extreme *out, size_t max_out);

// Height at one instant by half-cosine interpolation between the extremes
// either side of it. Returns false if `t` isn't bracketed -- the chart
// stops there rather than inventing a tail.
bool height_at(const Extreme *ex, size_t n, epoch_t t, float *h);

// Facts for the text panel, sorted relative to the time the image is on
// display: [window_lo, window_hi]. Same rules as tide_render.py:
//   before the window -> LAST
//   inside it         -> NOW, and the tide is "turning"
//   after it          -> NEXT (up to three)
struct Summary {
  bool           has_current;
  float          current;       // feet, at `now`
  int8_t         rising;        // 1 rising, 0 falling, -1 turning/unknown
  const Extreme *turning;       // null unless an extreme is in the window
  const Extreme *prev;          // most recent before the window, or null
  const Extreme *next[3];
  int            n_next;
};
Summary summarize(const Extreme *ex, size_t n, epoch_t now,
                  epoch_t window_lo, epoch_t window_hi);

// Calendar helpers, UTC only: no dependency on the C library's time zone
// handling, which differs between the ESP32 and a laptop.
epoch_t epoch_from_utc(int year, int month, int day, int hour, int minute);
