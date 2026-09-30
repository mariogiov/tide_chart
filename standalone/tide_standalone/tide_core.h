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

// Any NOAA "time,value,..." CSV, such as 6-minute water levels
// (product=water_level) or predictions (product=predictions, interval=6):
//
//     Date Time, Water Level, Sigma, ...
//     2026-09-29 14:00,3.281,0.023,...
//
// Only the time and first value are kept. Rows with an empty value -- a
// hole in real-time data -- are skipped, not read as zero.
size_t parse_noaa_series(const char *text, epoch_t *t, float *v, size_t max_out);

// ---- Monterey weather offset --------------------------------------------
// Predictions are astronomy only. Monterey has a live gauge, so its
// measured level minus its own prediction is the weather's effect, and on
// the scale of storms the whole bay moves together. Same method as
// tide_render.py.

struct Anomaly {
  epoch_t t;
  float   a;      // measured - predicted, feet
};

const epoch_t OFFSET_AVERAGE_S = 3600;   // summarize the newest hour
const int     OFFSET_MIN_POINTS = 5;     // of ~10 readings in a healthy hour
const int     TRACE_SMOOTH = 5;          // rolling median, readings (30 min)
const epoch_t TRACE_MAX_GAP_S = 18 * 60; // lift the pen across longer holes

// Pair up readings that share a timestamp. Both inputs in time order.
size_t match_anomalies(const epoch_t *obs_t, const float *obs_v, size_t n_obs,
                       const epoch_t *pred_t, const float *pred_v, size_t n_pred,
                       Anomaly *out, size_t max_out);

// Median anomaly over the newest hour of data. False if there's too little
// data, or the newest reading is more than `max_age` older than `now`.
bool weather_offset(const Anomaly *an, size_t n, epoch_t now, epoch_t max_age,
                    float *out);

// The dotted line: estimated real water at Santa Cruz from `start` on.
// Writes up to `max_out` points; NAN heights mark gaps. Returns the count.
size_t measured_trace(const Anomaly *an, size_t n, const Extreme *ex, size_t ne,
                      epoch_t start, epoch_t *out_t, float *out_h, size_t max_out);

// Calendar helpers, UTC only: no dependency on the C library's time zone
// handling, which differs between the ESP32 and a laptop.
epoch_t epoch_from_utc(int year, int month, int day, int hour, int minute);
void utc_fields(epoch_t t, int *year, int *month, int *day, int *hour, int *minute);
