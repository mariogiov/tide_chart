// panel.h -- put a finished frame on the e-paper and power the panel down.
//
// One implementation per driver board. panel_waveshare.cpp is the Waveshare
// e-Paper ESP32 Driver Board with the 7.5" V2 panel. For the EE05 this file
// gets a sibling built on Seeed's library, and nothing else has to change.
#pragma once
#include <stdint.h>

// `frame`: 96,000 bytes of 2-bit pixels when gray4, else 48,000 of 1-bit.
void panel_show(const uint8_t *frame, bool gray4);
