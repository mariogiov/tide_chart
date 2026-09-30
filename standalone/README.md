# Tide display, standalone version

The display computes and draws its own chart: no Python, no GitHub, no
cron-job.org. It downloads a month of NOAA high/low predictions once a day
and redraws hourly from that without WiFi.

## Layout

    tide_standalone/     the Arduino sketch (open this folder in the IDE)
      tide_core.*        tide math: NOAA CSV parsing, interpolation, NOW/LAST/NEXT
      canvas.*           drawing surface in the panel's 2-bit pixel format
      chart.*            the chart layout, ported from tide_render.py
      font.h, fonts.h    bitmap fonts (fonts.h is generated -- don't edit)
      tide_standalone.ino  device code -- stub until the EE05 arrives
    host/                build and preview the same code on a laptop
    tools/make_fonts.py  regenerates fonts.h

## Preview on the Mac

    cd host
    make                           # builds ./preview and runs the tests
    ./preview --fake --out chart.bmp && open chart.bmp

Flags: `--gray4` (4-level panel), `--offset 0.3` (Monterey line and dots,
made-up readings), `--battery 12` (low-battery icon), `--charge-me` (the
battery-empty screen), `--now "2026-09-29 21:30"` (UTC), `--csv FILE`
(real NOAA data, below).

Real data: save this in a browser as `sc.csv` (edit the dates), then
`./preview --csv sc.csv`:

    https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&interval=hilo&station=9413745&begin_date=20260928&end_date=20261005&datum=MLLW&units=english&time_zone=gmt&format=csv

`make` needs Apple's command-line tools; if it complains, run
`xcode-select --install` once.
