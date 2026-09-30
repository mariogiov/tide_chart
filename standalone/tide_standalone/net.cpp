#include "net.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_sntp.h>

#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy secrets.h.example to secrets.h and put your WiFi name and password in it."
#endif

bool wifi_connect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("wifi: timed out");
      return false;
    }
    delay(100);
  }
  Serial.printf("wifi: connected in %lu ms, rssi %d\n",
                (unsigned long)(millis() - start), WiFi.RSSI());
  return true;
}

void wifi_off() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Waits for a fresh answer from a time server -- not just "the clock looks
// plausible", which after deep sleep it always does.
bool ntp_sync() {
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  const uint32_t start = millis();
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if (millis() - start > NTP_TIMEOUT_MS) {
      Serial.println("ntp: timed out");
      return false;
    }
    delay(100);
  }
  Serial.printf("ntp: synced in %lu ms\n", (unsigned long)(millis() - start));
  return true;
}

bool http_get(const String &url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();   // public, read-only data: no certificate check
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("http: bad url");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("http: status %d for %s\n", code, url.c_str());
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  Serial.printf("http: %u bytes\n", (unsigned)body.length());
  return true;
}
