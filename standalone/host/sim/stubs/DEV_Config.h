#pragma once
#include <stdint.h>
typedef uint8_t UBYTE;
void DEV_Module_Init();
// The real Waveshare headers define these; keep them here to prove the
// sketch's own BLACK/WHITE enum never meets them.
#define BLACK 0x00
#define WHITE 0xFF
