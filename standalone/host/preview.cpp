// preview.cpp -- render the tide chart on a laptop, no ESP32 needed.
//
// Uses the exact same drawing code the firmware will (../tide_standalone),
// so what you see here is what the panel will show, pixel for pixel.
//
//     make                                   # build
//     ./preview --fake --out chart.bmp       # synthetic tides
//     ./preview --csv santacruz.csv          # real NOAA data (see README)
//     ./preview --fake --offset 0.3          # with the Monterey line + dots
//     ./preview --fake --battery 12          # low-battery icon
//     ./preview --charge-me                  # the "battery empty" screen
//     ./preview --fake --gray4               # 4-level panel instead of 1-bit
//
// Output is a .bmp (opens in Preview / any image viewer) and a .bin of the
// raw panel bytes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "canvas.h"
#include "chart.h"
#include "tide_core.h"

static const char *TZ_PACIFIC = "PST8PDT,M3.2.0,M11.1.0";

static bool write_bmp(const char *path, int w, int h, const std::vector<uint8_t> &gray) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  const int row = (w * 3 + 3) & ~3;
  const uint32_t size = 54 + row * h;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (v >> (8 * i)) & 255; };
  put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
  hdr[26] = 1; hdr[28] = 24; put32(34, row * h);
  fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> line(row, 0);
  for (int y = h - 1; y >= 0; y--) {           // BMP rows run bottom-up
    for (int x = 0; x < w; x++) line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = gray[y * w + x];
    fwrite(line.data(), 1, row, f);
  }
  fclose(f);
  return true;
}

static std::string read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

// Same synthetic tides as tide_render.py --fake.
static size_t fake_extremes(epoch_t now, Extreme *out) {
  const float heights[] = {5.1f, -0.3f, 3.9f, 1.4f};
  const epoch_t base = (now - 2 * 86400 - 6 * 3600) / 3600 * 3600;
  for (int i = 0; i < 24; i++) {
    out[i].t = base + (epoch_t)(i * 372.5 * 60);
    out[i].h = heights[i % 4];
    out[i].type = out[i].h > 2 ? 'H' : 'L';
  }
  return 24;
}

int main(int argc, char **argv) {
  const char *csv = nullptr, *out = "chart.bmp", *station = "Santa Cruz";
  bool fake = false, gray4 = false, charge_me = false;
  float offset = NAN;
  int battery = -1;
  epoch_t now = (epoch_t)time(nullptr) / 60 * 60;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
    if (!strcmp(a, "--fake")) fake = true;
    else if (!strcmp(a, "--gray4")) gray4 = true;
    else if (!strcmp(a, "--charge-me")) charge_me = true;
    else if (!strcmp(a, "--csv") && v) { csv = v; i++; }
    else if (!strcmp(a, "--out") && v) { out = v; i++; }
    else if (!strcmp(a, "--offset") && v) { offset = strtof(v, nullptr); i++; }
    else if (!strcmp(a, "--battery") && v) { battery = atoi(v); i++; }
    else if (!strcmp(a, "--now") && v) {             // "YYYY-MM-DD HH:MM", UTC
      int Y, M, D, h, m;
      if (sscanf(v, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) != 5) { fprintf(stderr, "bad --now\n"); return 2; }
      now = epoch_from_utc(Y, M, D, h, m);
      i++;
    } else { fprintf(stderr, "unknown argument: %s\n", a); return 2; }
  }

  setenv("TZ", TZ_PACIFIC, 1);
  tzset();

  const int W = 800, H = 480;
  Canvas canvas(W, H);
  if (!canvas.ok()) { fprintf(stderr, "out of memory\n"); return 1; }

  static Extreme ex[2000];
  size_t n = 0;
  std::vector<epoch_t> trace_t;
  std::vector<float> trace_h;

  if (charge_me) {
    draw_charge_me(canvas, station, gray4);
  } else {
    if (csv) {
      std::string text = read_file(csv);
      n = parse_noaa_csv(text.c_str(), ex, 2000);
      if (n < 2) { fprintf(stderr, "no extremes parsed from %s\n", csv); return 1; }
    } else if (fake) {
      n = fake_extremes(now, ex);
    } else {
      fprintf(stderr, "need --fake or --csv FILE (or --charge-me)\n");
      return 2;
    }

    ChartInput in;
    in.extremes = ex;
    in.n = n;
    in.station_name = station;
    in.now = now;
    in.window_lo = now;
    in.window_hi = now + 70 * 60;
    in.gray4 = gray4;
    in.battery_pct = battery;

    if (!isnan(offset)) {
      // Pretend gauge readings, like tide_render.py --fake-offset: the
      // offset, drifting a little and wobbling, every 6 minutes.
      in.offset_ft = offset;
      int i = 0;
      for (epoch_t t = now - 6 * 3600; t <= now; t += 360, i++) {
        float base;
        if (!height_at(ex, n, t, &base)) continue;
        const float a = offset + 0.12f * (i / 50.0f) - 0.06f +
                        0.05f * sinf(i / 2.3f) + 0.03f * sinf(i * 1.7f);
        trace_t.push_back(t);
        trace_h.push_back(base + a);
      }
      in.trace_t = trace_t.data();
      in.trace_h = trace_h.data();
      in.trace_n = trace_t.size();
    }
    draw_chart(canvas, in);
  }

  // Export: the panel's own bytes, plus a viewable image of exactly them.
  std::vector<uint8_t> gray(W * H);
  std::string bin = std::string(out).substr(0, std::string(out).rfind('.')) + ".bin";
  FILE *fb = fopen(bin.c_str(), "wb");
  if (gray4) {
    static const uint8_t ramp[4] = {0, 85, 170, 255};
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) gray[y * W + x] = ramp[canvas.get(x, y)];
    if (fb) fwrite(canvas.data(), 1, canvas.bytes(), fb);
  } else {
    std::vector<uint8_t> bits(W * H / 8);
    canvas.to_1bit(bits.data());
    for (int i = 0; i < W * H; i++) gray[i] = (bits[i >> 3] & (0x80 >> (i & 7))) ? 255 : 0;
    if (fb) fwrite(bits.data(), 1, bits.size(), fb);
  }
  if (fb) fclose(fb);
  if (!write_bmp(out, W, H, gray)) { fprintf(stderr, "can't write %s\n", out); return 1; }
  printf("wrote %s and %s  (%zu extremes)\n", out, bin.c_str(), n);
  return 0;
}
