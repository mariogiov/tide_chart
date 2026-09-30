#pragma once
#include "Arduino.h"
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum { WIFI_STA = 1, WIFI_OFF = 0 };
struct WiFiStub {
  void mode(int) {}
  void begin(const char *, const char *) {}
  int status();
  void disconnect(bool) {}
  int RSSI() { return -60; }
};
extern WiFiStub WiFi;
