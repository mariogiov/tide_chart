# Tide display, standalone version

The display computes and draws its own chart: no Python, no GitHub, no
cron-job.org. Every hour it wakes, redraws from what it has cached, and
goes back to sleep. WiFi only comes on when something is due:

| What | How often | Why |
|---|---|---|
| NOAA predictions for Santa Cruz (31 days of highs/lows) | daily | also syncs the clock |
| Monterey's live gauge (offset line + dotted trace) | every 3 h | weather changes over hours |

If WiFi or the time server fails, it backs off (1, 2, 4, 8, then every 12
hours) instead of retrying every hour; the cached month keeps the chart
right meanwhile. If just a download fails (NOAA hiccup, gauge offline), it
tries again on that download's normal schedule.

## Folder

    tide_standalone/          the Arduino sketch (open this folder in the IDE)
      tide_standalone.ino     the wake: battery -> downloads -> draw -> sleep
      config.h                settings: stations, quiet hours, battery, panel
      secrets.h.example       copy to secrets.h, add WiFi (never committed)
      plan.h                  what each wake downloads, and how long to sleep
      tide_core.*             tide math, NOAA parsing, the Monterey offset
      canvas.*, chart.*       drawing, and the chart layout
      font.h, fonts.h         bitmap fonts (fonts.h is generated)
      net.*                   WiFi, clock sync, HTTPS            (ESP32 only)
      panel_waveshare.cpp     the Waveshare board + 7.5" V2      (ESP32 only)
      battery.*               battery voltage, if wired          (ESP32 only)
    host/                     laptop build: preview, tests, full simulation
    tools/make_fonts.py       regenerates fonts.h

## Flashing (Waveshare board you have now)

1. Copy `tide_standalone/secrets.h.example` to `tide_standalone/secrets.h`
   and put in your WiFi name and password.
2. Open the `tide_standalone` folder in the Arduino IDE. Same board and port
   as before. It uses the same Waveshare library, with your 4-gray patch.
3. Upload. Open the serial monitor at 115200 to watch the first wake: it
   should connect, sync the clock, download, and draw within a minute.

It redraws just after each top of the hour. After a reset or power loss it
starts fresh and downloads again.

**Battery:** the Waveshare board can't measure its battery, so the
low-battery icon and the cutoff are off (`BATTERY_ADC_PIN -1` in config.h).
Don't leave it running on a LiPo unattended in that state. The EE05 can
measure its battery; its pins go in config.h once confirmed from its
schematic.

## Settings worth knowing (config.h)

- `OFFSET_ENABLED 0` turns off the Monterey line and dots: WiFi once a day.
- `QUIET_START` / `QUIET_END`: no Monterey fetches between these hours.
  Equal values (the default, 0 and 0) mean none are skipped. `23` and `4`
  would skip 11 PM through 3 AM.
- `PANEL_GRAY4 0` for a black-and-white panel.

## On the laptop

    cd host
    make                           # builds ./preview, runs the tests
    ./preview --fake --out chart.bmp && open chart.bmp

`./preview` flags: `--gray4`, `--offset 0.3`, `--battery 12`, `--charge-me`,
`--now "2026-09-29 21:30"` (UTC), `--csv FILE` (real NOAA data: save this in
a browser, editing the dates, then pass it):

    https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&interval=hilo&station=9413745&begin_date=20260928&end_date=20261005&datum=MLLW&units=english&time_zone=gmt&format=csv

Full simulation -- the real sketch against stand-ins for WiFi, NOAA, the
clock and the panel, three days of wakes in a second:

    cd host/sim
    make && ./sim                  # or: ./sim wifi-outage | noaa-errors | offline-start
    open out_normal/frame_005.bmp  # what the panel would show at each wake

`make` needs Apple's command-line tools; if it complains, run
`xcode-select --install` once.
