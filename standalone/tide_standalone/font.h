// Bitmap font format shared by fonts.h (generated) and canvas.cpp.
#pragma once
#include <stdint.h>

// Every glyph is stored twice (see tools/make_fonts.py):
//   mono  1 bit/pixel, hinted -- crisp on 1-bit panels
//   aa    2 bits/pixel of coverage, 0..3 -- smooth edges on gray panels
// Offsets are into the font's bitmap, in bytes. x/y place the bitmap's
// top-left corner relative to the pen position on the baseline (y < 0 is
// above the baseline).
struct Glyph {
  uint32_t codepoint;
  uint8_t  advance;     // how far the pen moves after this glyph
  uint32_t mono_offset;
  uint8_t  mono_w, mono_h;
  int8_t   mono_x, mono_y;
  uint32_t aa_offset;
  uint8_t  aa_w, aa_h;
  int8_t   aa_x, aa_y;
};

struct Font {
  const uint8_t *bitmap;
  const Glyph   *glyphs;   // sorted by codepoint
  uint16_t       count;
  uint8_t        ascent;   // pixels above the baseline
  uint8_t        descent;  // pixels below it
};
