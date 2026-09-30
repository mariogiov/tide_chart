// canvas.h -- a small drawing surface in the panel's own pixel format.
//
// Four levels per pixel, 2 bits each, packed 4 pixels per byte with the
// leftmost pixel in the high bits, rows top to bottom -- byte for byte what
// Display_4Gray on the Waveshare board takes, and what tide_render.py's
// --gray4 output produces. 800x480 is 96,000 bytes.
//
// For 1-bit panels, to_1bit() converts at the end: the two grays become
// fine dot patterns, black and white stay solid.
//
// No Arduino dependencies, so the same drawing code runs on a laptop.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "font.h"

enum Level : uint8_t { BLACK = 0, DARK = 1, LIGHT = 2, WHITE = 3 };
enum Align { LEFT, CENTER, RIGHT };

class Canvas {
 public:
  // Uses `buffer` if given (must hold bytes()), otherwise allocates.
  Canvas(int width, int height, uint8_t *buffer = nullptr);
  ~Canvas();
  bool ok() const { return buf_ != nullptr; }

  int width() const { return w_; }
  int height() const { return h_; }
  size_t bytes() const { return (size_t)w_ * h_ / 4; }
  const uint8_t *data() const { return buf_; }

  void clear(Level c = WHITE);
  void set(int x, int y, Level c);
  Level get(int x, int y) const;

  void hline(int x0, int x1, int y, Level c);
  void vline(int x, int y0, int y1, Level c);
  void fill_rect(int x, int y, int w, int h, Level c);
  void rect(int x, int y, int w, int h, Level c);          // 1px outline
  void fill_circle(int cx, int cy, int r, Level c);
  // Thick line with round ends: a disc stamped along the segment.
  void line(int x0, int y0, int x1, int y1, int thickness, Level c);
  // Dashed horizontal line: `on` pixels drawn, `off` skipped, repeating.
  void dashed_hline(int x0, int x1, int y, int on, int off, Level c);

  // Text. `y` is the TOP of the line (like matplotlib's va="top"); the
  // baseline sits `font.ascent` below it. UTF-8, for the arrows and dots.
  // Returns the width drawn.
  int text(const Font &f, int x, int y, const char *s, Level c,
           Align align = LEFT);
  int text_width(const Font &f, const char *s) const;
  // Height of a capital letter above the baseline -- for centering text
  // on a point, which ascent (it includes accent room) is too tall for.
  static int cap_height(const Font &f);

  // Pack to 1 bit per pixel (8 per byte, MSB first, set bit = white) into
  // `out`, which must hold width*height/8 bytes. Light gray becomes a
  // sparse 4x4 ordered-dither screen, dark gray a denser one.
  void to_1bit(uint8_t *out) const;

 private:
  int w_, h_;
  uint8_t *buf_;
  bool owned_;
};
