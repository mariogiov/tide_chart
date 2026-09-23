/*
 * schedule.h -- when the display wakes up.
 *
 * Pure arithmetic, no hardware calls, so it can be tested on a laptop.
 * tide_display.ino includes it; keep the two files in the same folder.
 */
#pragma once
#include <stdint.h>

// ---- tunables --------------------------------------------------------------

// GitHub renders at :10 (cron "10 * * * *" in .github/workflows/main.yml)
// and draws the chart for the next top of the hour. Fetching at :30 keeps
// each image on the wall from :30 to :30 -- centered on the hour it shows.
// The 20-minute gap after the render absorbs GitHub's usual scheduling
// delay. If you change the cron minute, change this to match: cron minute
// + 20, and keep the top of the hour near the middle of :FETCH -> :FETCH.
#define FETCH_MINUTE      30

#define RETRY_MINUTES     10   // after a failed fetch, or an image not updated yet
#define MAX_RETRIES        3   // then give up until the next hourly slot
#define FALLBACK_MINUTES  60   // when the clock couldn't be synced
#define MIN_GAP_MINUTES   15   // never schedule the next slot sooner than this

enum FetchOutcome { FETCH_UPDATED, FETCH_UNCHANGED, FETCH_FAILED };

// Seconds from now (given as minutes and seconds past the hour) until the
// next FETCH_MINUTE:00 that is at least MIN_GAP_MINUTES away.
//
// The gap matters because the sleep timer drifts. A wake that lands at :47
// has already served the :50 slot; without the gap it would sleep three
// minutes, wake at :50, and fetch the same image again.
static inline uint32_t secondsUntilSlot(int min, int sec) {
  int32_t wait = FETCH_MINUTE * 60 - (min * 60 + sec);
  if (wait < MIN_GAP_MINUTES * 60) wait += 3600;
  return (uint32_t)wait;
}

// How long to sleep after this wake. `retries` survives deep sleep.
static inline uint32_t chooseSleepSeconds(FetchOutcome outcome, bool clockOk,
                                          int min, int sec, int *retries) {
  if (outcome != FETCH_UPDATED && *retries < MAX_RETRIES) {
    (*retries)++;
    return RETRY_MINUTES * 60;
  }
  *retries = 0;
  if (!clockOk) return FALLBACK_MINUTES * 60;
  return secondsUntilSlot(min, sec);
}
