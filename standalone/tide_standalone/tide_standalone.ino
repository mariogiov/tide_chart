// tide_standalone.ino -- the tide display that computes its own chart.
//
// NOT WIRED UP YET. The drawing code (tide_core, canvas, chart) is done
// and tested on a laptop -- see ../host. This sketch gets filled in once
// the EE05 board arrives and its pins are confirmed:
//
//   1. once a day: WiFi -> NOAA hilo CSV for ~30 days -> save to flash,
//      sync the clock, WiFi off
//   2. every hour:  read battery -> compute + draw the chart -> refresh
//      the panel -> deep sleep
//   3. battery nearly empty: draw_charge_me(), then stop waking
//
// It's here now so the folder opens in the Arduino IDE as a sketch.

#include "tide_core.h"
#include "canvas.h"
#include "chart.h"

void setup() {}
void loop() {}
