# /// script
# requires-python = ">=3.11"
# dependencies = ["requests", "matplotlib", "pillow", "numpy"]
# ///
"""
Tide chart renderer for a 7.5" e-paper display (800x480, 1-bit).

    NOAA hilo extremes -> interpolated curve -> matplotlib image -> 1-bit BMP

Runs off-device (laptop / Pi / cron). The ESP32 fetches the output and blits it.

Station 9413745 (Santa Cruz) is subordinate: the API serves ONLY high/low
extremes, so interval=hilo is required and the smooth curve is ours to build.

    python3 tide_render.py                 # live NOAA data -> tide.bmp
    python3 tide_render.py --fake          # synthetic data, no network
    python3 tide_render.py --out /tmp/x.bmp --png
"""

import argparse
import datetime as dt
import math
import sys
from zoneinfo import ZoneInfo

import requests
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
import numpy as np
from PIL import Image


STATION = "9413745"
STATION_NAME = "Santa Cruz"

# NOAA returns extreme times in lst_ldt -- the STATION's local time, with
# daylight saving. So "now" has to be the station's local time too, not the
# machine's. A GitHub Actions runner is UTC, which would put the now-line
# 7 or 8 hours ahead of tide times that were already correct: a chart that
# is internally inconsistent rather than uniformly wrong. ZoneInfo tracks
# PDT/PST the same way lst_ldt does, so the two stay in step across DST.
STATION_TZ = ZoneInfo("America/Los_Angeles")
API = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"


def station_now():
    """Current time at the station, as a naive datetime.

    Naive, so it compares directly against the naive datetimes parsed out
    of NOAA's response -- everything downstream stays in one timezone-free
    world anchored to the station.
    """
    return dt.datetime.now(STATION_TZ).replace(tzinfo=None,
                                               second=0, microsecond=0)

BACK = dt.timedelta(hours=6)        # how far back the chart shows
FORWARD = dt.timedelta(hours=18)   # how far forward
PAD = dt.timedelta(days=2)          # extra fetched each side, so the curve
                                    # always has a bracketing pair at the edges

# Draw the chart for the top of the NEXT hour rather than the moment of
# rendering. Timing, all three of which move together:
#   GitHub renders at :10            (cron "10 * * * *" in main.yml)
#   the display fetches at :30       (FETCH_MINUTE in schedule.h)
#   so it's on the wall :30 -> :30, with the top of the hour in the middle
# Tide predictions don't change depending on when you compute them, so a
# chart drawn at 7:10 for 8:00 is exactly as accurate as one drawn at 8:00.
# Pass --live to draw for the actual current time instead (for testing).
SNAP_TO_NEXT_HOUR = True

# Each image stays on the wall for about an hour, centered on the time it's
# drawn for. A high or low that falls inside that hour is neither clearly
# "last" nor "next" -- it happens while you're looking -- so it gets its own
# NOW slot, and "rising"/"falling" becomes "turning".
VIEW_HALF_WINDOW = dt.timedelta(minutes=30)

WIDTH, HEIGHT, DPI = 800, 480, 100
THRESHOLD = 170                     # plain cutoff for everything else
INVERT_RAW = False                  # flip if the panel renders a negative
FILL_GRAY = "0.80"                  # 1-bit: under-curve fill, dithered
DITHER_BAND = (185, 225)            # 1-bit: values in here become a dot screen

# --- 4-gray mode ---
# The panel has four levels: black, dark gray, light gray, white. These are
# the source-gray cut points between them, and the fill/gridline grays are
# chosen to land squarely inside their buckets rather than near a boundary.
GRAY_CUTS = (64, 128, 192)
FILL_GRAY_4 = "0.72"                # -> 184, light-gray bucket
RULE_GRAY_4 = "0.45"                # -> 115, dark-gray bucket


# ---------------------------------------------------------------- data

def fetch_extremes(station, begin, end):
    """Fetch high/low tide predictions from NOAA CO-OPS.

    Returns list[(datetime, float, str)] sorted by time, where str is
    "H" or "L". Datetimes are naive local time (lst_ldt).
    """
    params = {
        "product": "predictions",
        "interval": "hilo",
        "station": station,
        "begin_date": begin.strftime("%Y%m%d"),
        "end_date": end.strftime("%Y%m%d"),
        "datum": "MLLW",
        "units": "english",
        "time_zone": "lst_ldt",
        "format": "json",
    }
    r = requests.get(API, params=params, timeout=30)
    r.raise_for_status()
    data = r.json()

    if "predictions" not in data:
        # NOAA reports errors in-band with a 200. Fail loudly rather than
        # rendering a blank chart with no clue why.
        raise RuntimeError(data.get("error", {}).get("message", str(data)[:200]))

    out = [
        (dt.datetime.strptime(p["t"], "%Y-%m-%d %H:%M"), float(p["v"]), p["type"])
        for p in data["predictions"]
    ]
    return sorted(out, key=lambda e: e[0])


def height_at(extremes, when):
    """Interpolated height at a single instant, or None if unbracketed.

    Between an extreme at (t1, h1) and the next at (t2, h2) the tide is
    very close to a half-cosine:

        u = (when - t1) / (t2 - t1)
        h = h1 + (h2 - h1) * (1 - cos(pi * u)) / 2

    u=0 -> h1, u=1 -> h2, u=0.5 -> midpoint, slope zero at both ends
    (the water is momentarily still at high and low tide).
    """
    for (t1, h1, _), (t2, h2, _) in zip(extremes, extremes[1:]):
        if t1 <= when <= t2:
            span = (t2 - t1).total_seconds()
            if span <= 0:
                return h1
            u = (when - t1).total_seconds() / span
            return h1 + (h2 - h1) * (1 - math.cos(math.pi * u)) / 2
    return None


def interpolate(extremes, start, end, step_minutes=10):
    """Sample the curve across [start, end].

    Stops at the first unbracketed timestamp rather than extrapolating.
    A short chart is honest; a fabricated tail diverges silently on
    exactly the day nobody is watching.
    """
    times, heights = [], []
    step = dt.timedelta(minutes=step_minutes)
    when = start
    while when <= end:
        h = height_at(extremes, when)
        if h is None:
            break
        times.append(when)
        heights.append(h)
        when += step
    return times, heights


def summarize(extremes, now, half_window=VIEW_HALF_WINDOW):
    """Flat dict of facts for the text panel. The renderer only formats.

    Extremes are sorted into three groups relative to the hour the image is
    on display, not the single instant it's drawn for:

        past    before the window   -> LAST
        during  inside the window   -> NOW, and the tide is "turning"
        future  after the window    -> NEXT

    so every label stays true for the whole time the image is up. Outside a
    turn, rising/falling can't flip mid-window either: the direction only
    changes at an extreme, and there isn't one in the window.
    """
    lo, hi = now - half_window, now + half_window
    past = [e for e in extremes if e[0] < lo]
    during = [e for e in extremes if lo <= e[0] <= hi]
    future = [e for e in extremes if e[0] > hi]

    current = height_at(extremes, now)
    rising = future[0][2] == "H" if (future and not during) else None

    return {
        "current": current,
        "rising": rising,
        "turning": during[0] if during else None,
        "prev": past[-1] if past else None,
        "next": future[:3],
    }


# ---------------------------------------------------------------- render

def _fmt_event(event):
    """(datetime, height, type) -> ('High', '4:37 PM', '2.07 ft')"""
    when, height, kind = event
    label = "High" if kind == "H" else "Low"
    clock = when.strftime("%-I:%M %p")
    return label, clock, f"{height:.2f} ft"


def render(times, heights, summary, extremes, now, gray4=False,
           rendered=None):
    """Draw the layout and return a PIL Image at exactly 800x480."""
    fig = plt.figure(figsize=(WIDTH / DPI, HEIGHT / DPI), dpi=DPI)
    fig.patch.set_facecolor("white")

    # ---- left: text panel -------------------------------------------------
    fig.text(0.035, 0.965, f"{STATION_NAME} Tides", fontsize=19, weight="bold",
             va="top")
    fig.text(0.035, 0.865, "feet above MLLW", fontsize=10, va="top",
             color="0.35")

    if summary["current"] is not None:
        if summary["turning"]:
            kind = "high" if summary["turning"][2] == "H" else "low"
            headline = f"{summary['current']:.2f} ft"
            state = f"{kind} tide, turning"
        else:
            arrow = "\u2191" if summary["rising"] else "\u2193"
            headline = f"{summary['current']:.2f} ft {arrow}"
            state = "rising" if summary["rising"] else "falling"
        fig.text(0.035, 0.78, headline, fontsize=26, weight="bold", va="top")
        fig.text(0.035, 0.665, state, fontsize=11, va="top", color="0.35")

    # The slot under the headline shows the turn in progress if there is
    # one, otherwise the most recent high or low.
    y = 0.58
    slot = (("NOW", summary["turning"]) if summary["turning"]
            else ("LAST", summary["prev"]) if summary["prev"] else None)
    if slot:
        title, event = slot
        label, clock, height = _fmt_event(event)
        fig.text(0.035, y, title, fontsize=10, weight="bold", color="0.35",
                 va="top")
        fig.text(0.035, y - 0.055, f"{label}  {clock}", fontsize=12, va="top")
        fig.text(0.035, y - 0.105, height, fontsize=12, va="top", color="0.3")
        y -= 0.185

    if summary["next"]:
        fig.text(0.035, y, "NEXT", fontsize=10, weight="bold", color="0.35",
                 va="top")
        y -= 0.055
        for event in summary["next"]:
            label, clock, height = _fmt_event(event)
            fig.text(0.035, y, f"{label}  {clock}", fontsize=12, va="top")
            fig.text(0.035, y - 0.05, height, fontsize=12, va="top",
                     color="0.3")
            y -= 0.115

    # ---- right: the curve -------------------------------------------------
    ax = fig.add_axes([0.30, 0.13, 0.665, 0.75])

    if times:
        ax.plot(times, heights, color="black", linewidth=2.4, solid_capstyle="round")
        ax.fill_between(times, heights, min(heights) - 2,
                        facecolor=FILL_GRAY_4 if gray4 else FILL_GRAY,
                        edgecolor="none", linewidth=0.0)

        ax.axhline(0, color=RULE_GRAY_4 if gray4 else "0.6",
                   linewidth=1.1 if gray4 else 0.9, linestyle=(0, (4, 4)))
        ax.axvline(now, color="black", linewidth=1.6)

        # mark every extreme inside the visible window -- not just the ones
        # in `summary`, which only holds prev + next three
        lo, hi = times[0], times[-1]
        for when, height, _ in extremes:
            if lo <= when <= hi:
                ax.plot([when], [height], "o", color="black", markersize=5)
                ax.annotate(f"{height:.1f}", (when, height),
                            textcoords="offset points", xytext=(0, 10),
                            ha="center", fontsize=10)

        ax.set_xlim(lo, hi)
        pad = (max(heights) - min(heights)) * 0.22 + 0.3
        ax.set_ylim(min(heights) - pad, max(heights) + pad)

        # Labels every 4 hours, anchored so one always falls on the chart's
        # own hour -- the "now" line then always sits on a labeled tick.
        ax.xaxis.set_major_locator(
            mdates.HourLocator(byhour=range(now.hour % 4, 24, 4)))
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%-I%p"))

    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_linewidth(1.2)

    ax.tick_params(labelsize=10, width=1.2, length=4)
    if gray4:
        ax.grid(axis="y", color=RULE_GRAY_4, linewidth=0.8)
    else:
        ax.grid(axis="y", color="black", linewidth=0.6, linestyle=(0, (1, 6)))
    ax.set_axisbelow(True)

    # "for" the time the chart shows, "drawn" when it was actually rendered.
    # A drawn time more than ~an hour old means updates have stopped.
    footer = now.strftime("for %-I:%M %p %b %-d")
    if rendered is not None and rendered != now:
        footer += rendered.strftime("  \u00b7  drawn %-I:%M %p")
    fig.text(0.965, 0.035, footer, fontsize=9, color="0.35", ha="right")

    fig.canvas.draw()
    img = Image.frombuffer(
        "RGBA", fig.canvas.get_width_height(),
        fig.canvas.buffer_rgba(), "raw", "RGBA", 0, 1,
    ).convert("RGB")
    plt.close(fig)
    return img


# 4x4 ordered (Bayer) matrix. Ordered dithering gives a regular screen
# pattern, which e-ink renders cleanly; error-diffusion gives noise that
# looks dirty at this pixel density.
BAYER4 = np.array([[0, 8, 2, 10],
                   [12, 4, 14, 6],
                   [3, 11, 1, 9],
                   [15, 7, 13, 5]], dtype=np.float32) / 16.0


def to_1bit(img):
    """Threshold to 1-bit, dot-screening only the fill gray.

    Everything else gets a hard cutoff: Floyd-Steinberg across the whole
    image turns small text to mush. Dithering is confined to a narrow band
    of gray values, so only the deliberate fill is screened and strokes
    and glyphs stay solid.
    """
    a = np.asarray(img.convert("L"), dtype=np.float32)
    h, w = a.shape

    tile = np.tile(BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w]
    lo, hi = DITHER_BAND
    in_band = (a >= lo) & (a <= hi)

    out = np.where(a > THRESHOLD, 255, 0).astype(np.uint8)
    out[in_band] = np.where(a[in_band] / 255.0 > tile[in_band], 255, 0)

    return Image.fromarray(out, mode="L").convert("1")


def to_raw(bw):
    """Pack a 1-bit image into the 48,000 bytes the panel expects.

    800 x 480 = 384,000 pixels, 8 per byte. PIL's "1" mode already packs
    MSB-first, row-major, top-to-bottom, with a SET bit meaning white --
    which matches Waveshare's GUI_Paint, where WHITE is 0xFF and
    Paint_Clear(WHITE) fills the buffer with 0xFF.

    I have NOT verified this against real hardware. If the first refresh
    comes out as a photographic negative, set INVERT_RAW = True. That is
    the only thing that can be wrong here -- the geometry is fixed.
    """
    if bw.size != (WIDTH, HEIGHT):
        raise ValueError(f"expected {WIDTH}x{HEIGHT}, got {bw.size}")

    data = bw.tobytes()
    expected = WIDTH * HEIGHT // 8
    if len(data) != expected:
        raise ValueError(f"expected {expected} bytes, got {len(data)}")

    return bytes(b ^ 0xFF for b in data) if INVERT_RAW else data


def to_4gray(img):
    """Quantize to the panel's four levels. Returns a uint8 array of
    level indices 0..3, where 0 is black and 3 is white.

    No dithering here: with four real levels, matplotlib's own
    antialiasing already supplies the intermediate tones, and that is
    exactly what makes 4-gray text look better than 1-bit text.
    """
    a = np.asarray(img.convert("L"))
    lo, mid, hi = GRAY_CUTS
    return (np.digitize(a, (lo, mid, hi))).astype(np.uint8)   # 0..3


def to_raw_4gray(levels):
    """Pack level indices into the 96,000 bytes Display_4Gray expects.

    800 x 480 at 2 bits per pixel = 4 pixels per byte, leftmost pixel in
    the high bits, row-major, top to bottom. 0b11 is white, matching the
    driver initialising its buffer to 0xFF.

    MSB-first is inferred from the 1-bit convention (verified working on
    this panel) plus the buffer-size math, NOT from reading Waveshare's
    packing loop. If the image comes out with fine vertical smearing --
    detail shuffled within each group of 4 pixels -- the bit order is
    reversed and the fix is to flip the shift order below.
    """
    if levels.shape != (HEIGHT, WIDTH):
        raise ValueError(f"expected {HEIGHT}x{WIDTH}, got {levels.shape}")

    v = levels
    if INVERT_RAW:
        v = 3 - v

    g = v.reshape(HEIGHT, WIDTH // 4, 4)
    packed = (g[:, :, 0] << 6) | (g[:, :, 1] << 4) | (g[:, :, 2] << 2) | g[:, :, 3]

    data = packed.astype(np.uint8).tobytes()
    expected = WIDTH * HEIGHT // 4
    if len(data) != expected:
        raise ValueError(f"expected {expected} bytes, got {len(data)}")
    return data


def preview_4gray(levels):
    """Level indices -> a viewable 8-bit image, for --png."""
    ramp = np.array([0, 85, 170, 255], dtype=np.uint8)
    return Image.fromarray(ramp[levels], mode="L")


# ---------------------------------------------------------------- fake data

def fake_extremes(now):
    """Synthetic semidiurnal tides for offline layout work."""
    out = []
    base = (now - PAD - BACK).replace(minute=0, second=0, microsecond=0)
    heights = [5.1, -0.3, 3.9, 1.4]
    for i in range(24):
        when = base + dt.timedelta(minutes=int(i * 372.5))   # 6h12.5m
        height = heights[i % 4]
        out.append((when, height, "H" if height > 2 else "L"))
    return out


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tide.bmp")
    ap.add_argument("--fake", action="store_true",
                    help="synthetic data, no network")
    ap.add_argument("--png", action="store_true",
                    help="also write a full-color PNG for previewing")
    ap.add_argument("--raw", action="store_true",
                    help="also write packed bytes for the ESP32 to blit")
    ap.add_argument("--gray4", action="store_true",
                    help="4-level grayscale: 96,000 bytes, needs the "
                         "Init_4Gray/Display_4Gray firmware")
    ap.add_argument("--live", action="store_true",
                    help="draw for the actual current time instead of the "
                         "next top of the hour")
    args = ap.parse_args()

    rendered = station_now()
    if SNAP_TO_NEXT_HOUR and not args.live:
        now = rendered.replace(minute=0) + dt.timedelta(hours=1)
    else:
        now = rendered
    start, end = now - BACK, now + FORWARD

    if args.fake:
        extremes = fake_extremes(now)
    else:
        try:
            extremes = fetch_extremes(STATION, start - PAD, end + PAD)
        except Exception as exc:
            print(f"fetch failed: {exc}", file=sys.stderr)
            return 1

    times, heights = interpolate(extremes, start, end)
    if not times:
        print("no curve: extremes did not bracket the window", file=sys.stderr)
        return 1

    img = render(times, heights, summarize(extremes, now), extremes, now,
                 gray4=args.gray4, rendered=rendered)

    if args.gray4:
        levels = to_4gray(img)
        shown = preview_4gray(levels)
        raw = to_raw_4gray(levels)
    else:
        shown = to_1bit(img)
        raw = to_raw(shown)

    if args.png:
        shown.convert("RGB").save(args.out.rsplit(".", 1)[0] + ".png")
    shown.save(args.out)

    if args.raw:
        raw_path = args.out.rsplit(".", 1)[0] + ".bin"
        with open(raw_path, "wb") as fh:
            fh.write(raw)
        print(f"wrote {raw_path}  ({len(raw)} bytes)")

    print(f"wrote {args.out}  ({len(times)} points, "
          f"{min(heights):.2f}..{max(heights):.2f} ft)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
