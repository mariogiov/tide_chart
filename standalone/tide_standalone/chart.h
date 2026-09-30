// chart.h -- draws the tide chart onto a Canvas.
//
// A port of tide_render.py's layout: text panel on the left, curve on the
// right, same positions and sizes. Everything it needs comes in through
// ChartInput, so the same call makes the image on the ESP32 and in the
// laptop preview.
//
// Local time: clocks are printed with localtime_r(), so the caller sets the
// time zone once at startup:
//     setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1); tzset();
#pragma once
#include <math.h>
#include "canvas.h"
#include "tide_core.h"

struct ChartInput {
  const Extreme *extremes;
  size_t         n;
  const char    *station_name;

  epoch_t now;          // where the "now" line goes (render time)
  epoch_t window_lo;    // the stretch of time the image will be on the
  epoch_t window_hi;    //   wall, for the NOW / LAST / NEXT slots

  bool gray4;           // 4-level panel: gray text and rules. 1-bit: black

  // Optional: Monterey weather offset, feet. NAN = don't show.
  float offset_ft = NAN;
  // Optional: measured-water trace for the dotted line. NAN heights = gaps.
  const epoch_t *trace_t = nullptr;
  const float   *trace_h = nullptr;
  size_t         trace_n = 0;

  // Optional: battery percentage; the icon appears at or below
  // battery_warn_pct. Negative = no battery reading, never shown.
  int battery_pct = -1;
  int battery_warn_pct = 20;
};

void draw_chart(Canvas &c, const ChartInput &in);

// The last thing the display shows before it stops waking to protect the
// battery. E-ink holds it with no power, so it stays up until someone
// plugs the display in.
void draw_charge_me(Canvas &c, const char *station_name);
