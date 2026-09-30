// Stand-in for the Arduino core, just enough to compile and run the sketch
// on a laptop. Time is simulated (sim_clock) so days pass in milliseconds.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>

#define RTC_DATA_ATTR
#define OUTPUT 1
#define HIGH 1
#define LOW 0

extern int64_t sim_clock;          // UTC seconds
extern uint32_t sim_millis;

class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String operator+(const String &o) const { return String(s_ + o.s_); }
  String operator+(const char *o) const { return String(s_ + o); }
  friend String operator+(const char *a, const String &b) { return String(std::string(a) + b.s_); }
  const char *c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  String substring(unsigned a, unsigned b) const { return String(s_.substr(a, b - a)); }
  bool indexOf_contains(const char *x) const { return s_.find(x) != std::string::npos; }
 private:
  std::string s_;
};

struct SerialStub {
  void begin(int) {}
  void flush() {}
  template <typename... A> void printf(const char *f, A... a) { ::printf(f, a...); }
  void println(const char *s) { ::printf("%s\n", s); }
  void println(const String &s) { ::printf("%s\n", s.c_str()); }
};
extern SerialStub Serial;

inline void delay(uint32_t ms) { sim_millis += ms; }
inline uint32_t millis() { return sim_millis += 10; }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void analogReadResolution(int) {}
inline uint32_t analogReadMilliVolts(int) { return 1900; }

// The sketch reads the clock with time(nullptr); route it to the simulated
// device clock.
time_t sim_time(time_t *);
#define time(x) sim_time(x)
