/*
 * Tide display firmware — Waveshare 7.5" e-Paper + e-Paper ESP32 Driver Board
 *
 * Wake -> WiFi -> download one packed framebuffer -> blit -> deep sleep.
 * Everything happens in setup(); deep sleep resets the chip, so setup()
 * runs again on every wake and loop() is never reached.
 *
 * Credentials live in secrets.h, which is gitignored. If the compiler
 * says `secrets.h: No such file or directory`, copy secrets.h.example
 * to secrets.h and fill it in.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "DEV_Config.h"
#include "EPD.h"

// WIFI_SSID, WIFI_PASS, IMAGE_URL. Quotes, not angle brackets, so the
// compiler looks in this sketch folder first.
#include "secrets.h"

// ---------------------------------------------------------------- config

// 0 = black/white, 1 bit per pixel, 48,000 bytes.
// 1 = four gray levels, 2 bits per pixel, 96,000 bytes.
//
// This MUST match how the image was rendered. Point IMAGE_URL at a
// tide.bin produced with --gray4 when this is 1, and one produced
// without it when this is 0. Mismatched sizes are rejected rather
// than blitted, so the screen keeps its old image and the serial log
// says what it got.
#define GRAY4 1

const uint32_t SLEEP_MINUTES = 60;

#if GRAY4
const size_t IMAGE_BYTES = 800 * 480 / 4;   // 96000, 4 px per byte
#else
const size_t IMAGE_BYTES = 800 * 480 / 8;   // 48000, 8 px per byte
#endif

const uint32_t WIFI_TIMEOUT_MS = 20000;
const uint32_t HTTP_TIMEOUT_MS = 20000;

// ---------------------------------------------------------------- sleep

void sleepNow() {
  // Put the panel to sleep BEFORE the chip. Leaving the driver powered
  // holds a DC bias on the panel, which wastes current and is bad for it
  // over time.
  EPD_7IN5_V2_Sleep();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.printf("sleeping %lu min\n", (unsigned long)SLEEP_MINUTES);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- steps

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) return false;
    delay(250);
  }
  Serial.printf("wifi ok, %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// Read the body into `buffer` until we have IMAGE_BYTES or the stream
// goes quiet.
size_t drain(WiFiClient *stream, uint8_t *buffer) {
  size_t got = 0;
  uint32_t lastData = millis();

  while (got < IMAGE_BYTES) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t want = IMAGE_BYTES - got;
      if (avail < want) want = avail;
      got += stream->readBytes(buffer + got, want);
      lastData = millis();
    } else {
      if (!stream->connected() && stream->available() == 0) break;
      if (millis() - lastData > HTTP_TIMEOUT_MS) break;
      delay(5);
    }
  }
  return got;
}

bool downloadImage(uint8_t *buffer) {
  bool https = (strncmp(IMAGE_URL, "https:", 6) == 0);

  // Scoped so the TLS buffers (~30-40KB of heap) are released as soon as
  // the download finishes, well before the panel refresh.
  WiFiClientSecure secure;
  HTTPClient http;

  if (https) {
    // No certificate validation. This fetches a public image over a link
    // you already trust enough to run your WiFi on, and the alternative
    // is pinning a root CA that expires and silently bricks the display
    // months later. If that tradeoff bothers you, pin GitHub's root with
    // secure.setCACert(...) and set a calendar reminder.
    secure.setInsecure();
    // If 4-gray runs out of heap, the two ways out are:
    //   1. uncomment this -- smaller TLS buffers, but a server that sends
    //      TLS records larger than the rx buffer will fail the handshake
    //   2. serve tide.bin over plain HTTP from a machine on your LAN,
    //      which skips TLS entirely and frees the whole ~40KB
    // secure.setBufferSizes(4096, 2048);
    http.begin(secure, IMAGE_URL);
  } else {
    http.begin(IMAGE_URL);
  }

  http.setTimeout(HTTP_TIMEOUT_MS);
  // raw.githubusercontent.com redirects; without this you get a 302 and
  // an empty body.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("http %d\n", code);
    http.end();
    return false;
  }

  // Content-Length is -1 when the server uses chunked transfer encoding,
  // which GitHub sometimes does. So -1 is normal, not an error; only a
  // stated length that disagrees with the framebuffer is fatal.
  int len = http.getSize();
  if (len >= 0 && len != (int)IMAGE_BYTES) {
    Serial.printf("bad length %d, want %u\n", len, (unsigned)IMAGE_BYTES);
    http.end();
    return false;
  }

  size_t got = drain(http.getStreamPtr(), buffer);
  http.end();

  Serial.printf("got %u / %u bytes\n", (unsigned)got, (unsigned)IMAGE_BYTES);
  // A short read would blit garbage into the tail of the screen, so a
  // partial download is a failure, not a partial success.
  return got == IMAGE_BYTES;
}

// ---------------------------------------------------------------- entry

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- wake ---");

  DEV_Module_Init();
#if GRAY4
  EPD_7IN5_V2_Init_4Gray();
#else
  EPD_7IN5_V2_Init();
#endif

  // getMaxAllocHeap is the largest CONTIGUOUS block, which is what a single
  // big malloc actually needs -- total free heap can look fine while no one
  // block is large enough.
  Serial.printf("want %u bytes; largest free block %u\n",
                (unsigned)IMAGE_BYTES, (unsigned)ESP.getMaxAllocHeap());

  uint8_t *buffer = (uint8_t *)malloc(IMAGE_BYTES);
  if (buffer == NULL) {
    // In 4-gray this is the likely failure: 96KB has to be ONE contiguous
    // block, and total free heap can look fine while no single block is
    // big enough.
    Serial.println("malloc failed -- not enough contiguous heap");
    sleepNow();
  }

  // TLS wants roughly another 40KB on top of the framebuffer. If this
  // prints under ~45000, HTTPS is where it will fail; see the notes in
  // downloadImage() for the two ways out.
  Serial.printf("free heap after malloc %u\n", (unsigned)ESP.getFreeHeap());

  if (connectWiFi() && downloadImage(buffer)) {
    Serial.println("display");
#if GRAY4
    // Noticeably slower than the 1-bit refresh: reaching the intermediate
    // levels takes more waveform passes.
    EPD_7IN5_V2_Display_4Gray(buffer);
#else
    EPD_7IN5_V2_Display(buffer);
#endif
  } else {
    // Leave the previous image on screen rather than clearing it. Stale
    // tides beat a blank panel, and e-paper holds the last frame for free.
    Serial.println("fetch failed, keeping previous image");
  }

  free(buffer);
  sleepNow();
}

void loop() {
  // never reached — deep sleep restarts the chip
}
