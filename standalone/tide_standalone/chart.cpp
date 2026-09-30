#include "chart.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fonts.h"

// ---- layout -----------------------------------------------------------
// Positions are fractions of the screen, measured from the BOTTOM-left,
// exactly as in tide_render.py (matplotlib figure coordinates), so the two
// can be compared number for number. X() and Y() turn them into pixels.

static int W = 800, H = 480;
static int X(double fx) { return (int)lround(fx * W); }
static int Y(double fy) { return (int)lround((1.0 - fy) * H); }

static const double TEXT_X = 0.035;
static const double AX_LEFT = 0.30, AX_BOTTOM = 0.13;
static const double AX_WIDTH = 0.665, AX_HEIGHT = 0.75;
static const epoch_t BACK = 6 * 3600;      // chart shows 6 h back...
static const epoch_t FORWARD = 18 * 3600;  // ...and 18 h forward

// ---- local time ---------------------------------------------------------

static struct tm local(epoch_t t) {
  time_t tt = (time_t)t;
  struct tm lt;
  localtime_r(&tt, &lt);
  return lt;
}

static int hour12(const struct tm &lt) {
  int h = lt.tm_hour % 12;
  return h ? h : 12;
}

// "4:37 PM"
static void fmt_clock(char *out, size_t n, epoch_t t) {
  struct tm lt = local(t);
  snprintf(out, n, "%d:%02d %s", hour12(lt), lt.tm_min,
           lt.tm_hour < 12 ? "AM" : "PM");
}

// "4PM"
static void fmt_hour(char *out, size_t n, epoch_t t) {
  struct tm lt = local(t);
  snprintf(out, n, "%d%s", hour12(lt), lt.tm_hour < 12 ? "AM" : "PM");
}

// "Sep 29"
static void fmt_date(char *out, size_t n, epoch_t t) {
  static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm lt = local(t);
  snprintf(out, n, "%s %d", MON[lt.tm_mon], lt.tm_mday);
}

// ---- text panel ---------------------------------------------------------

// Draw one event as two lines ("High  4:37 PM" / "5.10 ft"), tops at
// fractions y1 and y2.
static void draw_event(Canvas &c, const Extreme *e, double y1, double y2,
                       Level secondary) {
  char clock[16], line[40];
  fmt_clock(clock, sizeof(clock), e->t);
  snprintf(line, sizeof(line), "%s  %s", e->type == 'H' ? "High" : "Low", clock);
  c.text(font_body, X(TEXT_X), Y(y1), line, BLACK);
  snprintf(line, sizeof(line), "%.2f ft", e->h);
  c.text(font_body, X(TEXT_X), Y(y2), line, secondary);
}

static void draw_text_panel(Canvas &c, const ChartInput &in, const Summary &s,
                            Level gray) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s Tides", in.station_name);
  c.text(font_title, X(TEXT_X), Y(0.965), buf, BLACK);
  c.text(font_small, X(TEXT_X), Y(0.865), "feet above MLLW", gray);

  if (s.has_current) {
    const char *state;
    if (s.turning) {
      snprintf(buf, sizeof(buf), "%.2f ft", s.current);
      state = s.turning->type == 'H' ? "high tide, turning" : "low tide, turning";
    } else {
      snprintf(buf, sizeof(buf), "%.2f ft %s", s.current,
               s.rising == 1 ? "↑" : "↓");
      state = s.rising == 1 ? "rising" : "falling";
    }
    c.text(font_headline, X(TEXT_X), Y(0.78), buf, BLACK);
    c.text(font_state, X(TEXT_X), Y(0.665), state, gray);
  }

  // The slot under the headline: the turn in progress if there is one,
  // otherwise the most recent high or low.
  double y = 0.58;
  const Extreme *slot = s.turning ? s.turning : s.prev;
  if (slot) {
    c.text(font_small_b, X(TEXT_X), Y(y), s.turning ? "NOW" : "LAST", gray);
    draw_event(c, slot, y - 0.055, y - 0.105, gray);
    y -= 0.185;
  }
  if (s.n_next > 0) {
    c.text(font_small_b, X(TEXT_X), Y(y), "NEXT", gray);
    y -= 0.055;
    for (int i = 0; i < s.n_next; i++) {
      draw_event(c, s.next[i], y, y - 0.05, gray);
      y -= 0.115;
    }
  }
}

// ---- the curve ----------------------------------------------------------

// A "nice" gridline spacing: 0.5, 1, 2 or 5 feet, the smallest giving at
// most 9 lines -- which lands on 1 ft for a normal day, like matplotlib.
static double nice_step(double span) {
  static const double steps[] = {0.5, 1, 2, 5, 10};
  for (double s : steps)
    if (span / s <= 9) return s;
  return 10;
}

struct Axes {
  int x0, x1, y0, y1;   // pixel box: x0 left, x1 right, y0 top, y1 bottom
  epoch_t t0, t1;       // time at x0 and x1
  double h0, h1;        // height at y1 (bottom) and y0 (top)

  int x_of(epoch_t t) const {
    return x0 + (int)lround((double)(t - t0) * (x1 - x0) / (double)(t1 - t0));
  }
  int y_of(double h) const { return (int)lround(yf(h)); }
  float yf(double h) const {       // unrounded, for smooth drawing
    return (float)(y1 - (h - h0) * (y1 - y0) / (h1 - h0));
  }
  float xf(epoch_t t) const {
    return (float)(x0 + (double)(t - t0) * (x1 - x0) / (double)(t1 - t0));
  }
  epoch_t t_of(int x) const {
    return t0 + (epoch_t)llround((double)(x - x0) * (t1 - t0) / (x1 - x0));
  }
};

static void draw_curve(Canvas &c, const ChartInput &in, Level gray) {
  if (in.n < 2) return;
  Axes ax;
  ax.x0 = X(AX_LEFT);
  ax.x1 = X(AX_LEFT + AX_WIDTH);
  ax.y0 = Y(AX_BOTTOM + AX_HEIGHT);
  ax.y1 = Y(AX_BOTTOM);

  // Time span: 6 h back to 18 h forward, cut short where the predictions
  // run out rather than extrapolating.
  ax.t0 = in.now - BACK;
  ax.t1 = in.now + FORWARD;
  if (ax.t0 < in.extremes[0].t) ax.t0 = in.extremes[0].t;
  if (ax.t1 > in.extremes[in.n - 1].t) ax.t1 = in.extremes[in.n - 1].t;
  if (ax.t1 <= ax.t0) return;

  // Sample the predicted curve once per pixel column.
  const int ncol = ax.x1 - ax.x0 + 1;
  float *col = new float[ncol];
  double lo = 1e9, hi = -1e9;
  for (int i = 0; i < ncol; i++) {
    float h = 0;
    height_at(in.extremes, in.n, ax.t_of(ax.x0 + i), &h);
    col[i] = h;
    if (h < lo) lo = h;
    if (h > hi) hi = h;
  }
  // The measured trace, clipped to the chart's time span.
  const bool has_trace = in.trace_n > 0 && in.trace_t && in.trace_h;
  for (size_t i = 0; has_trace && i < in.trace_n; i++) {
    const float h = in.trace_h[i];
    if (isnan(h) || in.trace_t[i] < ax.t0 || in.trace_t[i] > ax.t1) continue;
    if (h < lo) lo = h;
    if (h > hi) hi = h;
  }
  const double pad = (hi - lo) * 0.22 + 0.3;
  ax.h0 = lo - pad;
  ax.h1 = hi + pad;

  // 1. Water: light gray from the curve down to the bottom axis.
  for (int i = 0; i < ncol; i++) c.vline(ax.x0 + i, ax.y_of(col[i]), ax.y1, LIGHT);

  // 2. Horizontal gridlines with labels, drawn over the water so the depth
  //    scale reads straight across it.
  const double step = nice_step(ax.h1 - ax.h0);
  const int cap = Canvas::cap_height(font_small);
  for (double g = ceil(ax.h0 / step) * step; g <= ax.h1 + 1e-9; g += step) {
    const int y = ax.y_of(g);
    c.hline(ax.x0, ax.x1, y, DARK);
    c.fill_rect(ax.x0 - 6, y, 6, 2, BLACK);                       // tick
    char lab[16];
    const double v = fabs(g) < 1e-9 ? 0.0 : g;
    const char *minus = v < 0 ? "−" : "";   // a real minus, like matplotlib
    if (step >= 1) snprintf(lab, sizeof(lab), "%s%d", minus, (int)lround(fabs(v)));
    else snprintf(lab, sizeof(lab), "%s%.1f", minus, fabs(v));
    c.text(font_small, ax.x0 - 10, y - font_small.ascent + cap / 2, lab, BLACK, RIGHT);
  }

  // 3. Zero (mean lower low water), dashed.
  if (ax.h0 < 0 && ax.h1 > 0) c.dashed_hline(ax.x0, ax.x1, ax.y_of(0), 5, 5, gray);

  // 4. The now line.
  if (in.now >= ax.t0 && in.now <= ax.t1) {
    const int x = ax.x_of(in.now);
    c.fill_rect(x - 1, ax.y0, 2, ax.y1 - ax.y0 + 1, BLACK);
  }

  // 5. The predicted curve, drawn smooth at sub-pixel heights.
  float *xs = new float[ncol], *ys = new float[ncol];
  for (int i = 0; i < ncol; i++) { xs[i] = (float)(ax.x0 + i); ys[i] = ax.yf(col[i]); }
  c.stroke(xs, ys, ncol, 3.2f, BLACK);
  delete[] xs;
  delete[] ys;

  // 6. Measured water, dotted: a small dot every DOT_GAP pixels of path.
  //    Where it agrees with the prediction the dots vanish into the curve.
  if (has_trace) {
    const double DOT_GAP = 7.0;
    const float DOT_R = 1.6f;
    double carry = 0;
    bool pen = false;
    float px = 0, py = 0;
    for (size_t i = 0; i < in.trace_n; i++) {
      const float h = in.trace_h[i];
      const epoch_t t = in.trace_t[i];
      if (isnan(h) || t < ax.t0 || t > ax.t1) { pen = false; continue; }
      const float x = ax.xf(t), y = ax.yf(h);
      if (!pen) {
        c.disc(x, y, DOT_R, BLACK);
        carry = 0;
        pen = true;
      } else {
        const double dx = x - px, dy = y - py, len = sqrt(dx * dx + dy * dy);
        double d = DOT_GAP - carry;
        while (d <= len) {
          c.disc((float)(px + dx * d / len), (float)(py + dy * d / len), DOT_R, BLACK);
          d += DOT_GAP;
        }
        carry = len - (d - DOT_GAP);
      }
      px = x;
      py = y;
    }
  }

  // 7. Every high and low in view: a dot and its height. The label sits
  //    above the curve, or above the dotted line if that runs higher.
  for (size_t i = 0; i < in.n; i++) {
    const Extreme &e = in.extremes[i];
    if (e.t < ax.t0 || e.t > ax.t1) continue;
    const int x = ax.x_of(e.t);
    c.disc(ax.xf(e.t), ax.yf(e.h), 3.6f, BLACK);
    double anchor = e.h;
    for (size_t k = 0; has_trace && k < in.trace_n; k++) {
      const epoch_t dt = in.trace_t[k] - e.t;
      if (dt >= -1800 && dt <= 1800 && !isnan(in.trace_h[k]) && in.trace_h[k] > anchor)
        anchor = in.trace_h[k];
    }
    char lab[16];
    snprintf(lab, sizeof(lab), "%.1f", e.h);
    const int baseline = ax.y_of(anchor) - 14;
    c.text(font_small, x, baseline - font_small.ascent, lab, BLACK, CENTER);
  }

  // 8. Hour labels every 4 hours, anchored so one always lands on the now
  //    line's hour. Stepping through real hours and asking the local clock
  //    keeps this right across daylight-saving changes.
  const int now_hour = local(in.now).tm_hour;
  for (epoch_t t = (ax.t0 / 3600 + 1) * 3600; t <= ax.t1; t += 3600) {
    const struct tm lt = local(t);
    if (lt.tm_min != 0 || (lt.tm_hour - now_hour + 24) % 4 != 0) continue;
    const int x = ax.x_of(t);
    c.fill_rect(x - 1, ax.y1, 2, 6, BLACK);
    char lab[8];
    fmt_hour(lab, sizeof(lab), t);
    c.text(font_small, x, ax.y1 + 9, lab, BLACK, CENTER);
  }

  // 9. Axis lines, left and bottom, last so nothing paints over them.
  c.fill_rect(ax.x0 - 1, ax.y0, 2, ax.y1 - ax.y0 + 2, BLACK);
  c.fill_rect(ax.x0 - 1, ax.y1, ax.x1 - ax.x0 + 2, 2, BLACK);

  delete[] col;
}

// ---- corners ------------------------------------------------------------

static void draw_battery_icon(Canvas &c, int x, int y, int pct) {
  // 24 x 12 body, 2 x 6 nub, fill proportional to charge.
  c.rect(x, y, 24, 12, BLACK);
  c.rect(x + 1, y + 1, 22, 10, BLACK);
  c.fill_rect(x + 24, y + 3, 3, 6, BLACK);
  const int fill = pct <= 0 ? 0 : (pct * 18 + 99) / 100;
  if (fill > 0) c.fill_rect(x + 3, y + 3, fill, 6, BLACK);
}

void draw_chart(Canvas &c, const ChartInput &in) {
  W = c.width();
  H = c.height();
  c.clear(WHITE);
  c.set_antialias(in.gray4);
  // On a 1-bit panel a gray would come out dithered, which breaks up text
  // and thin rules. So "gray" means dark gray on a 4-level panel and plain
  // black on a 1-bit one, as in the Python version.
  const Level gray = in.gray4 ? DARK : BLACK;

  const Summary s = summarize(in.extremes, in.n, in.now, in.window_lo, in.window_hi);
  draw_text_panel(c, in, s, gray);
  draw_curve(c, in, gray);

  // Top right: the weather offset, only when it's big enough to matter.
  char buf[64];
  const bool has_trace = in.trace_n > 0;
  if (!isnan(in.offset_ft) && fabs(in.offset_ft) >= 0.1) {
    snprintf(buf, sizeof(buf), "sea running %.1f ft %s prediction",
             fabs(in.offset_ft), in.offset_ft > 0 ? "above" : "below");
    c.text(font_state_b, X(0.965), Y(0.955), buf, BLACK, RIGHT);
  }
  if (has_trace)
    c.text(font_tiny, X(0.965), Y(0.91), "dotted: measured, via Monterey gauge",
           gray, RIGHT);

  // Bottom right: when the chart is from. If this stops advancing, the
  // display has stopped updating.
  char clock[16], date[16];
  fmt_clock(clock, sizeof(clock), in.now);
  fmt_date(date, sizeof(date), in.now);
  snprintf(buf, sizeof(buf), "as of %s %s", clock, date);
  const int baseline = Y(0.035);
  const int fw = c.text(font_tiny, X(0.965), baseline - font_tiny.ascent, buf, gray, RIGHT);

  // Low battery: icon and percentage just left of the footer.
  if (in.battery_pct >= 0 && in.battery_pct <= in.battery_warn_pct) {
    snprintf(buf, sizeof(buf), "%d%%", in.battery_pct);
    const int right = X(0.965) - fw - 18;
    const int tw = c.text(font_tiny, right, baseline - font_tiny.ascent, buf, BLACK, RIGHT);
    draw_battery_icon(c, right - tw - 34, baseline - 11, in.battery_pct);
  }
}

void draw_charge_me(Canvas &c, const char *station_name, bool gray4) {
  W = c.width();
  H = c.height();
  c.clear(WHITE);
  c.set_antialias(gray4);
  const int cx = W / 2;

  // A big empty battery, drawn from rectangles.
  const int bw = 160, bh = 80, bx = cx - bw / 2 - 8, by = Y(0.72);
  for (int k = 0; k < 6; k++) c.rect(bx + k, by + k, bw - 2 * k, bh - 2 * k, BLACK);
  c.fill_rect(bx + bw, by + bh / 2 - 18, 16, 36, BLACK);

  c.text(font_headline, cx, Y(0.44), "Battery empty", BLACK, CENTER);
  c.text(font_body, cx, Y(0.32), "Plug in a USB-C cable to charge.", BLACK, CENTER);
  c.text(font_body, cx, Y(0.26), "The tide chart comes back on its own.", BLACK, CENTER);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s Tides", station_name);
  c.text(font_tiny, cx, Y(0.08), buf, BLACK, CENTER);
}

void draw_message(Canvas &c, const char *title, const char *line1,
                  const char *line2, bool gray4) {
  W = c.width();
  H = c.height();
  c.clear(WHITE);
  c.set_antialias(gray4);
  const int cx = W / 2;
  c.text(font_title, cx, Y(0.62), title, BLACK, CENTER);
  if (line1) c.text(font_body, cx, Y(0.50), line1, BLACK, CENTER);
  if (line2) c.text(font_body, cx, Y(0.44), line2, BLACK, CENTER);
}
