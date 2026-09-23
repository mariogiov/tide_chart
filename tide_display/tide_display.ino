/*
 * Tide display firmware — Waveshare 7.5" e-Paper + e-Paper ESP32 Driver Board
 *
 * Wake -> WiFi -> sync clock -> fetch image -> redraw if it's new -> deep sleep.
 * Everything happens in setup(); deep sleep resets the chip, so setup()
 * runs again on every wake and loop() is never reached.
 *
 * When it wakes (see schedule.h): at a fixed minute each hour, 20 minutes
 * after GitHub renders, rather than every 60 minutes from whenever it
 * happened to boot. If the fetch fails, or GitHub hasn't produced a new
 * image yet, it retries in 10 minutes, up to 3 times.
 *
 * Files in this folder:
 *   tide_display.ino   this file
 *   schedule.h         wake-time arithmetic (tested separately)
 *   secrets.h          WiFi + image URL; gitignored. If the compiler says
 *                      `secrets.h: No such file or directory`, copy
 *                      secrets.h.example to secrets.h and fill it in.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <sys/time.h>
#include <time.h>
#include "DEV_Config.h"
#include "EPD.h"

// Quotes, not angle brackets, so the compiler looks in this folder first.
#include "secrets.h"    // WIFI_SSID, WIFI_PASS, IMAGE_URL
#include "schedule.h"   // FETCH_MINUTE, retry policy, sleep arithmetic

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

#if GRAY4
const size_t IMAGE_BYTES = 800 * 480 / 4;   // 96000, 4 px per byte
#else
const size_t IMAGE_BYTES = 800 * 480 / 8;   // 48000, 8 px per byte
#endif

const uint32_t WIFI_TIMEOUT_MS = 20000;
const uint32_t HTTP_TIMEOUT_MS = 20000;
const uint32_t NTP_TIMEOUT_MS  = 10000;

// ---------------------------------------------------------------- memory that survives sleep
//
// RTC_DATA_ATTR puts a variable in a small block of memory that stays
// powered during deep sleep, so it remembers its value between wakes.
// It resets to these starting values on power-up, a reset-button press,
// or a reflash -- which means pressing reset always forces a fresh
// download and redraw.

RTC_DATA_ATTR char lastEtag[100] = "";   // fingerprint of the image on screen
RTC_DATA_ATTR int  retriesUsed   = 0;

// Did we power up the panel on this wake? (Normal variable: resets every wake.)
static bool panelAwake = false;

// ---------------------------------------------------------------- sleep

void sleepFor(uint32_t seconds) {
  // Put the panel to sleep BEFORE the chip -- leaving the driver powered
  // holds a DC bias on the panel. But only if we woke it this cycle. On
  // wakes where nothing changed, the panel is still asleep from last time,
  // and the library's busy-wait has no timeout, so it's safest not to poke
  // a sleeping controller at all.
  if (panelAwake) EPD_7IN5_V2_Sleep();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.printf("sleeping %lu s (~%lu min)\n",
                (unsigned long)seconds, (unsigned long)((seconds + 30) / 60));
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- steps

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println("wifi timeout");
      return false;
    }
    delay(250);
  }
  Serial.printf("wifi ok, %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// Get real time from the internet. Returns true once it has it.
//
// The chip keeps a clock running through deep sleep, but it drifts. So we
// zero the clock first: getLocalTime() then can't succeed until a genuine
// time-server answer arrives. Without that, it would hand back the drifted
// time immediately and we'd schedule off a wrong clock without knowing.
//
// Everything here is UTC. Minutes past the hour are identical in UTC and
// Pacific, and GitHub's cron runs in UTC too, so no timezone is needed.
bool syncClock(struct tm *now) {
  struct timeval zero = {0, 0};
  settimeofday(&zero, nullptr);
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  if (!getLocalTime(now, NTP_TIMEOUT_MS)) {
    Serial.println("clock sync failed");
    return false;
  }
  Serial.printf("clock %02d:%02d:%02d UTC\n", now->tm_hour, now->tm_min, now->tm_sec);
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

// FETCH_UPDATED   new image in `buffer`, its fingerprint in `newEtag`
// FETCH_UNCHANGED same image as the one on screen -- GitHub hasn't rendered yet
// FETCH_FAILED    anything else
FetchOutcome downloadImage(uint8_t *buffer, char *newEtag, size_t newEtagSize) {
  newEtag[0] = '\0';
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

  // The ETag is a fingerprint the server attaches to each version of a
  // file. Response headers are only kept if we ask for them before GET.
  const char *wanted[] = {"ETag"};
  http.collectHeaders(wanted, 1);

  // Tell the server which version we already have. If that's still the
  // current one, it answers 304 "not modified" with no body -- no 96KB
  // download, no redraw.
  if (lastEtag[0] != '\0') http.addHeader("If-None-Match", lastEtag);

  int code = http.GET();
  if (code == HTTP_CODE_NOT_MODIFIED) {
    Serial.println("not modified (304)");
    http.end();
    return FETCH_UNCHANGED;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("http %d\n", code);
    http.end();
    return FETCH_FAILED;
  }

  // Some servers ignore If-None-Match and send the whole file anyway, so
  // compare fingerprints ourselves too. Only trust a match when both sides
  // actually have one: with no ETag at all, every fetch counts as new,
  // which is exactly how the old firmware behaved.
  String etag = http.header("ETag");
  if (etag.length() > 0 && lastEtag[0] != '\0' &&
      strcmp(etag.c_str(), lastEtag) == 0) {
    Serial.println("unchanged (same ETag)");
    http.end();
    return FETCH_UNCHANGED;
  }
  // Remember the new fingerprint -- unless it's too long to store whole.
  // A truncated one would never match, so store nothing instead.
  if (etag.length() > 0 && etag.length() < newEtagSize) {
    etag.toCharArray(newEtag, newEtagSize);
  }

  // Content-Length is -1 when the server uses chunked transfer encoding,
  // which GitHub sometimes does. So -1 is normal, not an error; only a
  // stated length that disagrees with the framebuffer is fatal.
  int len = http.getSize();
  if (len >= 0 && len != (int)IMAGE_BYTES) {
    Serial.printf("bad length %d, want %u\n", len, (unsigned)IMAGE_BYTES);
    http.end();
    return FETCH_FAILED;
  }

  size_t got = drain(http.getStreamPtr(), buffer);
  http.end();

  Serial.printf("got %u / %u bytes\n", (unsigned)got, (unsigned)IMAGE_BYTES);
  // A short read would blit garbage into the tail of the screen, so a
  // partial download is a failure, not a partial success.
  return (got == IMAGE_BYTES) ? FETCH_UPDATED : FETCH_FAILED;
}

// ---------------------------------------------------------------- entry

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n--- wake (retries used so far: %d) ---\n", retriesUsed);

  // Pins and SPI only. The panel itself stays asleep until there's
  // something new to draw.
  DEV_Module_Init();

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
    sleepFor(FALLBACK_MINUTES * 60);
  }

  // TLS wants roughly another 40KB on top of the framebuffer. If this
  // prints under ~45000, HTTPS is where it will fail; see the notes in
  // downloadImage() for the two ways out.
  Serial.printf("free heap after malloc %u\n", (unsigned)ESP.getFreeHeap());

  FetchOutcome outcome = FETCH_FAILED;
  bool clockOk = false;
  struct tm now = {};
  char newEtag[sizeof(lastEtag)];

  if (connectWiFi()) {
    clockOk = syncClock(&now);
    outcome = downloadImage(buffer, newEtag, sizeof(newEtag));
  }

  if (outcome == FETCH_UPDATED) {
#if GRAY4
    EPD_7IN5_V2_Init_4Gray();
#else
    EPD_7IN5_V2_Init();
#endif
    panelAwake = true;
    Serial.println("display");
#if GRAY4
    // Noticeably slower than the 1-bit refresh: reaching the intermediate
    // levels takes more waveform passes.
    EPD_7IN5_V2_Display_4Gray(buffer);
#else
    EPD_7IN5_V2_Display(buffer);
#endif
    strcpy(lastEtag, newEtag);   // this is what's on screen now
  } else {
    // Leave the previous image on screen. Stale tides beat a blank panel,
    // and e-paper holds the last frame for free.
    Serial.println(outcome == FETCH_UNCHANGED ? "image not updated yet, keeping current"
                                              : "fetch failed, keeping current");
  }

  free(buffer);

  // Measure the next slot from when we actually go to sleep: the download
  // and a 4-gray refresh take real time.
  if (clockOk) getLocalTime(&now, 0);
  sleepFor(chooseSleepSeconds(outcome, clockOk, now.tm_min, now.tm_sec, &retriesUsed));
}

void loop() {
  // never reached — deep sleep restarts the chip
}
