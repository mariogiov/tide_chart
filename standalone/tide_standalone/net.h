// net.h -- WiFi, clock sync and HTTPS downloads. ESP32 only.
#pragma once
#include <Arduino.h>

bool wifi_connect();                       // join the network in secrets.h
void wifi_off();                           // radio fully off before drawing
bool ntp_sync();                           // true once the clock is set
bool http_get(const String &url, String &body);
