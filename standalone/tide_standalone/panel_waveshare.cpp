// Waveshare e-Paper ESP32 Driver Board + 7.5" V2 panel.
//
// Deliberately includes nothing from this project but panel.h: Waveshare's
// headers #define BLACK and WHITE, which would collide with canvas.h.
#include "panel.h"

#include "DEV_Config.h"
#include "EPD.h"

void panel_show(const uint8_t *frame, bool gray4) {
  DEV_Module_Init();
  if (gray4) {
    EPD_7IN5_V2_Init_4Gray();                 // from the library patch
    EPD_7IN5_V2_Display_4Gray((UBYTE *)frame);
  } else {
    EPD_7IN5_V2_Init();
    EPD_7IN5_V2_Display((UBYTE *)frame);
  }
  EPD_7IN5_V2_Sleep();
}
