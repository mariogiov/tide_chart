#include "canvas.h"

#include <stdlib.h>
#include <string.h>

Canvas::Canvas(int width, int height, uint8_t *buffer)
    : w_(width), h_(height), buf_(buffer), owned_(false) {
  if (!buf_) {
    buf_ = (uint8_t *)malloc(bytes());
    owned_ = true;
  }
  if (buf_) clear(WHITE);
}

Canvas::~Canvas() {
  if (owned_) free(buf_);
}

void Canvas::clear(Level c) {
  const uint8_t b = (uint8_t)(c << 6 | c << 4 | c << 2 | c);
  memset(buf_, b, bytes());
}

void Canvas::set(int x, int y, Level c) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  size_t i = (size_t)y * (w_ / 4) + x / 4;
  int shift = 6 - 2 * (x & 3);
  buf_[i] = (uint8_t)((buf_[i] & ~(3 << shift)) | (c << shift));
}

Level Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return WHITE;
  size_t i = (size_t)y * (w_ / 4) + x / 4;
  return (Level)((buf_[i] >> (6 - 2 * (x & 3))) & 3);
}

void Canvas::hline(int x0, int x1, int y, Level c) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) set(x, y, c);
}

void Canvas::vline(int x, int y0, int y1, Level c) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) set(x, y, c);
}

void Canvas::fill_rect(int x, int y, int w, int h, Level c) {
  for (int j = y; j < y + h; j++) hline(x, x + w - 1, j, c);
}

void Canvas::rect(int x, int y, int w, int h, Level c) {
  hline(x, x + w - 1, y, c);
  hline(x, x + w - 1, y + h - 1, c);
  vline(x, y, y + h - 1, c);
  vline(x + w - 1, y, y + h - 1, c);
}

void Canvas::fill_circle(int cx, int cy, int r, Level c) {
  // r2 + r gives rounder small discs than r2 alone.
  const int lim = r * r + r;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= lim) set(cx + dx, cy + dy, c);
}

void Canvas::line(int x0, int y0, int x1, int y1, int thickness, Level c) {
  const int r = thickness / 2;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (r <= 0) set(x0, y0, c);
    else if (thickness % 2 == 0) fill_rect(x0 - r + 1, y0 - r + 1, 2 * r, 2 * r, c);
    else fill_circle(x0, y0, r, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void Canvas::dashed_hline(int x0, int x1, int y, int on, int off, Level c) {
  const int period = on + off;
  for (int x = x0; x <= x1; x++)
    if ((x - x0) % period < on) set(x, y, c);
}

// ---- text -------------------------------------------------------------

// Next codepoint from a UTF-8 string; advances *s. Bad bytes read as '?'.
static uint32_t next_cp(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  uint32_t cp;
  int extra;
  if (p[0] < 0x80) { cp = p[0]; extra = 0; }
  else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
  else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
  else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
  else { *s += 1; return '?'; }
  for (int i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80) { *s += i; return '?'; }
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  *s += 1 + extra;
  return cp;
}

static const Glyph *find_glyph(const Font &f, uint32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (f.glyphs[mid].codepoint == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].codepoint < cp) lo = mid + 1; else hi = mid - 1;
  }
  return cp == '?' ? nullptr : find_glyph(f, '?');
}

int Canvas::cap_height(const Font &f) {
  const Glyph *g = find_glyph(f, 'H');
  return g ? -g->yoff : f.ascent;
}

int Canvas::text_width(const Font &f, const char *s) const {
  int w = 0;
  while (*s) {
    const Glyph *g = find_glyph(f, next_cp(&s));
    if (g) w += g->advance;
  }
  return w;
}

int Canvas::text(const Font &f, int x, int y, const char *s, Level c,
                 Align align) {
  const int width = text_width(f, s);
  if (align == CENTER) x -= width / 2;
  else if (align == RIGHT) x -= width;
  const int baseline = y + f.ascent;
  while (*s) {
    const Glyph *g = find_glyph(f, next_cp(&s));
    if (!g) continue;
    const uint8_t *bits = f.bitmap + g->offset;
    int bit = 0;
    for (int j = 0; j < g->height; j++) {
      for (int i = 0; i < g->width; i++, bit++) {
        if (bits[bit >> 3] & (0x80 >> (bit & 7)))
          set(x + g->xoff + i, baseline + g->yoff + j, c);
      }
    }
    x += g->advance;
  }
  return width;
}

// ---- 1-bit export -------------------------------------------------------

// 4x4 Bayer matrix, values 0..15. A pixel of a given gray goes black where
// its threshold is below the gray's darkness: an even, regular screen that
// e-ink renders cleanly (error diffusion looks dirty at this density).
static const uint8_t BAYER4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void Canvas::to_1bit(uint8_t *out) const {
  // How many of the 16 cells go black for each level. Light gray at 3/16
  // matches the Python version's dotted fill; dark gray at 8/16 turns
  // 1px gridlines into an even dotted line.
  static const uint8_t black_cells[4] = {16, 8, 3, 0};
  memset(out, 0, (size_t)w_ * h_ / 8);
  for (int y = 0; y < h_; y++) {
    for (int x = 0; x < w_; x++) {
      const Level c = get(x, y);
      const bool black = BAYER4[y & 3][x & 3] < black_cells[c];
      if (!black) out[((size_t)y * w_ + x) >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
  }
}
