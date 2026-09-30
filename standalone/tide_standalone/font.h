// Bitmap font format shared by fonts.h (generated) and canvas.cpp.
#pragma once
#include <stdint.h>

struct Glyph {
  uint32_t codepoint;
  uint32_t offset;     // into the font's bitmap, in bytes
  uint8_t  width, height;
  int8_t   xoff;       // bitmap's left edge, relative to the pen position
  int8_t   yoff;       // bitmap's top edge, relative to the baseline (<0 = above)
  uint8_t  advance;    // how far the pen moves after this glyph
};

struct Font {
  const uint8_t *bitmap;
  const Glyph   *glyphs;   // sorted by codepoint
  uint16_t       count;
  uint8_t        ascent;   // pixels above the baseline
  uint8_t        descent;  // pixels below it
};
