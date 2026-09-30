// test.cpp -- checks for the tide math, parsing and time handling.
// `make test` builds and runs it; any failure prints and exits nonzero.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "canvas.h"
#include "tide_core.h"

static int failures = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } \
  } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

int main() {
  // --- calendar ---------------------------------------------------------
  CHECK(epoch_from_utc(1970, 1, 1, 0, 0) == 0);
  CHECK(epoch_from_utc(2000, 3, 1, 0, 0) == 951868800);        // leap-year edge
  CHECK(epoch_from_utc(2026, 9, 29, 21, 30) == 1790717400);
  CHECK(epoch_from_utc(2038, 1, 19, 3, 14) == 2147483640);     // past 32-bit time_t

  // --- CSV parsing ------------------------------------------------------
  const char *csv =
      "Date Time, Prediction, Type\r\n"
      "2026-09-29 10:52,-0.301,L\r\n"
      "\r\n"
      "2026-09-29 17:05, 3.912,H\r\n"
      "garbage line\n"
      "2026-09-30 02:40,1.4,L";                                   // no final newline
  Extreme ex[10];
  size_t n = parse_noaa_csv(csv, ex, 10);
  CHECK(n == 3);
  CHECK(ex[0].t == epoch_from_utc(2026, 9, 29, 10, 52));
  NEAR(ex[0].h, -0.301, 1e-6);
  CHECK(ex[0].type == 'L' && ex[1].type == 'H' && ex[2].type == 'L');
  NEAR(ex[1].h, 3.912, 1e-6);
  CHECK(parse_noaa_csv(csv, ex, 2) == 2);                         // respects max
  CHECK(parse_noaa_csv("", ex, 10) == 0);
  // Out of order input comes back sorted.
  n = parse_noaa_csv("2026-09-29 17:05,3.9,H\n2026-09-29 10:52,-0.3,L\n", ex, 10);
  CHECK(n == 2 && ex[0].type == 'L' && ex[1].type == 'H');

  // --- interpolation ----------------------------------------------------
  Extreme e[3] = {{0, 0.0f, 'L'}, {6 * 3600, 4.0f, 'H'}, {12 * 3600, 1.0f, 'L'}};
  float h;
  CHECK(height_at(e, 3, 0, &h)); NEAR(h, 0.0, 1e-6);
  CHECK(height_at(e, 3, 6 * 3600, &h)); NEAR(h, 4.0, 1e-6);
  CHECK(height_at(e, 3, 3 * 3600, &h)); NEAR(h, 2.0, 1e-5);        // midpoint
  CHECK(height_at(e, 3, 9 * 3600, &h)); NEAR(h, 2.5, 1e-5);
  CHECK(height_at(e, 3, 1 * 3600, &h)); NEAR(h, 4.0 * (1 - cos(M_PI / 6)) / 2, 1e-5);
  CHECK(!height_at(e, 3, -1, &h));                                 // unbracketed
  CHECK(!height_at(e, 3, 12 * 3600 + 1, &h));

  // --- summarize --------------------------------------------------------
  Extreme s[6] = {{0, 5, 'H'}, {6 * 3600, 0, 'L'}, {12 * 3600, 4, 'H'},
                  {18 * 3600, 1, 'L'}, {24 * 3600, 5, 'H'}, {30 * 3600, 0, 'L'}};
  // Between extremes: falling toward the low at 6h.
  Summary r = summarize(s, 6, 3 * 3600, 3 * 3600, 3 * 3600 + 4200);
  CHECK(r.has_current && !r.turning && r.rising == 0);
  CHECK(r.prev == &s[0] && r.n_next == 3 && r.next[0] == &s[1]);
  // A low inside the display window: turning, NOW slot, no direction.
  r = summarize(s, 6, 5 * 3600 + 1800, 5 * 3600 + 1800, 5 * 3600 + 1800 + 4200);
  CHECK(r.turning == &s[1] && r.rising == -1 && r.prev == &s[0] && r.next[0] == &s[2]);
  // Rising after the low has passed.
  r = summarize(s, 6, 7 * 3600, 7 * 3600, 7 * 3600 + 4200);
  CHECK(r.rising == 1 && r.prev == &s[1]);

  // --- local time across DST ------------------------------------------
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
  struct tm lt;
  time_t t = (time_t)epoch_from_utc(2026, 9, 29, 21, 30);          // PDT, UTC-7
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 14 && lt.tm_min == 30);
  t = (time_t)epoch_from_utc(2026, 12, 1, 21, 30);                 // PST, UTC-8
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 13);
  t = (time_t)epoch_from_utc(2027, 3, 14, 10, 0);                  // 3 AM PDT, just after spring-forward
  localtime_r(&t, &lt);
  CHECK(lt.tm_hour == 3);

  // --- canvas -----------------------------------------------------------
  Canvas c(8, 4);
  CHECK(c.ok() && c.bytes() == 8);
  c.set(0, 0, BLACK);
  c.set(5, 1, LIGHT);
  CHECK(c.get(0, 0) == BLACK && c.get(1, 0) == WHITE && c.get(5, 1) == LIGHT);
  CHECK(c.data()[0] == 0x3F);            // leftmost pixel in the high bits
  CHECK(c.data()[3] == 0xEF);            // row 1, pixels 4-7: W L W W
  uint8_t bits[4];
  c.to_1bit(bits);
  CHECK((bits[0] & 0x80) == 0);          // black stays black
  CHECK((bits[0] & 0x40) != 0);          // white stays white

  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
