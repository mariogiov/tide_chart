// config.h -- settings for the standalone tide display. Edit freely.
#pragma once

// ---- where -------------------------------------------------------------
#define STATION_ID     "9413745"     // Santa Cruz: predictions only
#define STATION_NAME   "Santa Cruz"
#define OFFSET_STATION "9413450"     // Monterey: live gauge, for the offset
// POSIX time zone: Pacific, with US daylight-saving rules.
#define TIMEZONE       "PST8PDT,M3.2.0,M11.1.0"

// ---- panel ---------------------------------------------------------------
// 1 = four gray levels (needs the Init_4Gray/Display_4Gray library patch),
// 0 = black and white.
#define PANEL_GRAY4 1

// ---- the Monterey offset and dotted line --------------------------------
#define OFFSET_ENABLED   1   // 0 = predictions only; WiFi once a day
#define OFFSET_EVERY_H   3   // hours between Monterey fetches
// No Monterey fetches from QUIET_START until QUIET_END (24-hour clock; can
// wrap past midnight, e.g. 23 and 4). Equal values = no quiet hours.
#define QUIET_START      0
#define QUIET_END        0
#define OFFSET_MAX_AGE_H 4   // hide offset and dots once the data is this old

// ---- predictions ---------------------------------------------------------
#define PREDS_DAYS       31  // how far ahead to download
#define PREDS_EVERY_H    24  // refresh (and sync the clock) this often

// ---- battery -------------------------------------------------------------
// The Waveshare driver board can't measure its battery, so sensing is off
// (-1) and so are the low-battery icon and the cutoff. DON'T leave it on a
// LiPo unattended like that. To enable: a 2:1 divider (two 100k resistors)
// from battery + to an ADC1 pin, and set that pin here. The EE05 has its
// own battery sensing; its pins go here once confirmed from the schematic.
#define BATTERY_ADC_PIN    -1
#define BATTERY_ENABLE_PIN -1    // some boards switch the divider on with a pin
#define BATTERY_DIVIDER    2.0f  // battery volts per ADC volt
#define BATTERY_WARN_PCT   20    // show the icon at or below this
#define BATTERY_CUTOFF_MV  3450  // below this: "charge me" screen, stop
#define BATTERY_RESUME_MV  3800  // ...until it's back above this (charging)
#define CUTOFF_CHECK_H     6     // how often to peek at the battery meanwhile

// ---- network -------------------------------------------------------------
#define WIFI_TIMEOUT_MS  20000
#define HTTP_TIMEOUT_MS  20000
#define NTP_TIMEOUT_MS   10000
