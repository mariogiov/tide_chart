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

# ---- weather offset from Monterey -------------------------------------------
# Predictions are astronomy only. Wind, storms and air pressure push the real
# sea above or below them. Santa Cruz has no live gauge, so borrow Monterey's:
# its measured level minus its own prediction is the weather offset, and on
# the scale of storms the two sides of the bay move together.
OFFSET_STATION = "9413450"          # Monterey, CA -- real-time gauge
OFFSET_LOOKBACK = dt.timedelta(hours=3)   # fetch this much recent data
OFFSET_AVERAGE = dt.timedelta(hours=1)    # ...and summarize the last hour
OFFSET_MAX_AGE = dt.timedelta(minutes=90) # older than this = gauge is down
OFFSET_MIN_POINTS = 5               # 6-min data: 10 per hour when healthy
OFFSET_MIN_SHOW = 0.1               # ft; smaller offsets aren't worth a line
TRACE_SMOOTH = 5                    # readings (x 6 min) in the dotted line's
                                    # rolling median -- irons out bay sloshing
TRACE_MAX_GAP = dt.timedelta(minutes=18)  # break the dotted line at data holes


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

# Where the "now" line goes.
#   False (default): at the moment of rendering. The measured dotted line then
#       runs right up to it. Render shortly before the display fetches
#       (e.g. trigger at :25, FETCH_MINUTE 30) so the image is fresh when it
#       lands; it then ages about an hour until the next one replaces it.
#   True (--snap-hour): at the top of the next hour, so an image rendered at
#       :10 and shown :30 -> :30 is centered on the hour it depicts. The
#       dotted line then stops short of the now line, since it can't know
#       the future.
SNAP_TO_NEXT_HOUR = False

# Each image stays on the wall for about an hour. A high or low that falls
# while it's up is neither clearly "last" nor "next" -- it happens while
# you're looking -- so it gets its own NOW slot, and "rising"/"falling"
# becomes "turning". The window is that hour, placed per the mode above:
#   snapped:   centered on the drawn-for hour  (now - 30 min .. now + 30 min)
#   unsnapped: starting at render time         (now .. now + DISPLAY_SPAN)
VIEW_HALF_WINDOW = dt.timedelta(minutes=30)
DISPLAY_SPAN = dt.timedelta(minutes=70)   # wait until fetch + an hour on the wall

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


def _noaa_get(params):
    """One CO-OPS request. NOAA reports errors in-band with a 200."""
    r = requests.get(API, params=params, timeout=30)
    r.raise_for_status()
    data = r.json()
    if "error" in data:
        raise RuntimeError(data["error"].get("message", str(data)[:200]))
    return data


def _series(rows):
    """[{"t": "2026-09-29 00:06", "v": "3.412", ...}] -> {datetime: float}.

    Real-time data has holes: a missing reading arrives with v == "".
    Those are dropped rather than treated as zero.
    """
    out = {}
    for row in rows:
        v = row.get("v", "")
        if v in ("", None):
            continue
        out[dt.datetime.strptime(row["t"], "%Y-%m-%d %H:%M")] = float(v)
    return out


def fetch_offset_data(station, begin, end):
    """Observed and predicted 6-minute water levels for [begin, end].

    Returns (observed, predicted), each {datetime: feet above MLLW}.
    """
    common = {
        "station": station,
        "begin_date": begin.strftime("%Y%m%d %H:%M"),
        "end_date": end.strftime("%Y%m%d %H:%M"),
        "datum": "MLLW",
        "units": "english",
        "time_zone": "lst_ldt",
        "format": "json",
    }
    obs = _noaa_get({**common, "product": "water_level"})
    pred = _noaa_get({**common, "product": "predictions", "interval": "6"})
    return _series(obs.get("data", [])), _series(pred.get("predictions", []))


def anomaly_series(observed, predicted, now):
    """[(time, observed - predicted)] at every matched reading up to now."""
    return sorted((t, observed[t] - predicted[t])
                  for t in observed if t in predicted and t <= now)


def weather_offset(observed, predicted, now):
    """Median of (observed - predicted) over the most recent hour, in feet.

    Positive: the sea is running above prediction. Returns None when the
    data can't support a number -- too few matched readings, or the newest
    one too old (gauge down, or NOAA hasn't posted recent data yet). The
    median shrugs off the odd spike from a wave or a bad sample.
    """
    matched = anomaly_series(observed, predicted, now)
    if not matched:
        return None
    newest = matched[-1][0]
    if now - newest > OFFSET_MAX_AGE:
        return None
    recent = [d for t, d in matched if t > newest - OFFSET_AVERAGE]
    if len(recent) < OFFSET_MIN_POINTS:
        return None
    return float(np.median(recent))


def measured_trace(anomalies, extremes, start):
    """Estimated real water level at Santa Cruz, for the dotted line.

    Santa Cruz's own prediction plus Monterey's weather anomaly at each
    6-minute reading, smoothed with a short rolling median. Returns
    (times, heights) with NaN wherever the gauge data has a hole, so the
    line breaks instead of drawing a straight bridge across missing data.
    """
    pts = [(t, a) for t, a in anomalies if t >= start]
    if len(pts) < TRACE_SMOOTH:
        return [], []
    half = TRACE_SMOOTH // 2
    raw = [a for _, a in pts]
    smooth = [float(np.median(raw[max(0, i - half): i + half + 1]))
              for i in range(len(raw))]

    times, heights, prev = [], [], None
    for (t, _), a in zip(pts, smooth):
        base = height_at(extremes, t)
        if base is None:
            continue
        if prev is not None and t - prev > TRACE_MAX_GAP:
            times.append(prev + (t - prev) / 2)   # NaN = pen up
            heights.append(float("nan"))
        times.append(t)
        heights.append(base + a)
        prev = t
    return times, heights


def fake_anomalies(offset, start, end):
    """Synthetic gauge anomalies for --fake-offset: the given offset, drifting
    a little and wobbling the way a real gauge record does."""
    out, t, i = [], start, 0
    while t <= end:
        drift = 0.12 * (i / 50) - 0.06
        wobble = 0.05 * math.sin(i / 2.3) + 0.03 * math.sin(i * 1.7)
        out.append((t, offset + drift + wobble))
        t += dt.timedelta(minutes=6)
        i += 1
    return out


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


def summarize(extremes, now, window=None):
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
    lo, hi = window or (now - VIEW_HALF_WINDOW, now + VIEW_HALF_WINDOW)
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
           rendered=None, offset=None, trace=None):
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

    # ---- top right: weather offset ----------------------------------------
    # Only when it's big enough to matter. The curve and headline stay pure
    # prediction; this line says how far the real sea is off from them.
    if offset is not None and abs(offset) >= OFFSET_MIN_SHOW:
        way = "above" if offset > 0 else "below"
        fig.text(0.965, 0.955,
                 f"sea running {abs(offset):.1f} ft {way} prediction",
                 fontsize=11, weight="bold", ha="right", va="top")
    has_trace = bool(trace and trace[0])
    if has_trace:
        fig.text(0.965, 0.91, "dotted: measured, via Monterey gauge",
                 fontsize=9, ha="right", va="top", color="0.35")

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

        # Measured water, dotted, drawn over the predicted line so where the
        # two agree the dots simply vanish into it.
        if has_trace:
            ax.plot(trace[0], trace[1], color="black", linewidth=2.2,
                    linestyle=(0, (0.1, 2.2)), dash_capstyle="round")

        # mark every extreme inside the visible window -- not just the ones
        # in `summary`, which only holds prev + next three
        lo, hi = times[0], times[-1]
        for when, height, _ in extremes:
            if lo <= when <= hi:
                ax.plot([when], [height], "o", color="black", markersize=5)
                # Lift the label clear of the dotted line where it runs higher.
                anchor = height
                if has_trace:
                    near = [h for t, h in zip(*trace)
                            if abs(t - when) <= dt.timedelta(minutes=30)
                            and not math.isnan(h)]
                    if near:
                        anchor = max(height, max(near))
                ax.annotate(f"{height:.1f}", (when, anchor),
                            textcoords="offset points", xytext=(0, 10),
                            ha="center", fontsize=10)

        ax.set_xlim(lo, hi)
        shown = list(heights)
        if has_trace:
            shown += [h for h in trace[1] if not math.isnan(h)]
        pad = (max(shown) - min(shown)) * 0.22 + 0.3
        ax.set_ylim(min(shown) - pad, max(shown) + pad)

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
    if rendered is None or rendered == now:
        footer = now.strftime("as of %-I:%M %p %b %-d")
    else:
        footer = now.strftime("for %-I:%M %p %b %-d")
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
    ap.add_argument("--no-offset", action="store_true",
                    help="skip the Monterey weather-offset line")
    ap.add_argument("--fake-offset", type=float, default=None, metavar="FT",
                    help="pretend the offset is FT feet (layout testing)")
    ap.add_argument("--snap-hour", action="store_true",
                    help="put the now line at the next top of the hour "
                         "instead of the moment of rendering")
    args = ap.parse_args()

    rendered = station_now()
    snap = SNAP_TO_NEXT_HOUR or args.snap_hour
    if snap:
        now = rendered.replace(minute=0) + dt.timedelta(hours=1)
        window = (now - VIEW_HALF_WINDOW, now + VIEW_HALF_WINDOW)
    else:
        now = rendered
        window = (now, now + DISPLAY_SPAN)
    start, end = now - BACK, now + FORWARD

    if args.fake:
        extremes = fake_extremes(now)
    else:
        try:
            extremes = fetch_extremes(STATION, start - PAD, end + PAD)
        except Exception as exc:
            print(f"fetch failed: {exc}", file=sys.stderr)
            return 1

    # The offset is measured up to the moment of rendering, not the hour the
    # chart is drawn for -- it's about the weather, which changes over hours.
    # Any failure here just drops the line; it must never cost the chart.
    offset, anomalies = None, []
    if args.fake_offset is not None:
        offset = args.fake_offset
        anomalies = fake_anomalies(offset, start, rendered)
    elif not (args.fake or args.no_offset):
        try:
            observed, predicted = fetch_offset_data(
                OFFSET_STATION, min(start, rendered - OFFSET_LOOKBACK), rendered)
            offset = weather_offset(observed, predicted, rendered)
            anomalies = anomaly_series(observed, predicted, rendered)
        except Exception as exc:
            print(f"offset fetch failed (chart unaffected): {exc}",
                  file=sys.stderr)
        print("weather offset: " +
              ("unavailable" if offset is None else f"{offset:+.2f} ft"))

    times, heights = interpolate(extremes, start, end)
    trace = measured_trace(anomalies, extremes, start) if anomalies else None
    if not times:
        print("no curve: extremes did not bracket the window", file=sys.stderr)
        return 1

    img = render(times, heights, summarize(extremes, now, window), extremes, now,
                 gray4=args.gray4, rendered=rendered, offset=offset, trace=trace)

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
