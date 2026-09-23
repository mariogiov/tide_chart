/*
 * 4-gray support for the 7.5" V2 panel, ported to the ESP32 library.
 *
 * WHY THIS FILE EXISTS
 *   Waveshare's Raspberry Pi C driver has EPD_7IN5_V2_Init_4Gray() and
 *   EPD_7IN5_V2_Display_4Gray(). The esp32-waveshare-epd library does
 *   not -- it only ever got the 1-bit functions. (It DOES have 4-gray
 *   for other panels, e.g. EPD_2IN7_V2_Init_4GRAY, which is why the
 *   compiler suggested that name.) These two functions are copied from
 *   the RPi C driver and use only helpers the ESP32 library already
 *   has: EPD_Reset, EPD_SendCommand, EPD_SendData, EPD_WaitUntilIdle,
 *   DEV_Delay_ms, EPD_7IN5_V2_TurnOnDisplay.
 *
 * HOW TO INSTALL
 *   1. In your Arduino libraries folder, open
 *        esp32-waveshare-epd/EPD_7in5_V2.c      (or .cpp)
 *      -- whichever file defines EPD_7IN5_V2_Init(). Paste the two
 *      function bodies below at the END of that file.
 *
 *      They must go in THAT file, not a new one: EPD_7IN5_V2_TurnOnDisplay
 *      is declared `static`, so it is only visible inside its own
 *      translation unit.
 *
 *   2. Open the matching EPD_7in5_V2.h and add the two declarations
 *      (bottom of this file) next to the existing EPD_7IN5_V2_Init()
 *      declaration.
 *
 *   3. Quit and reopen the Arduino IDE, then rebuild.
 *
 * Source: waveshareteam/e-Paper, RaspberryPi_JetsonNano/c/lib/e-Paper/EPD_7in5_V2.c
 */


/* ============================================================ */
/* ===  PASTE THESE TWO INTO EPD_7in5_V2.c  =================== */
/* ============================================================ */

UBYTE EPD_7IN5_V2_Init_4Gray(void)
{
    EPD_Reset();

    EPD_SendCommand(0x00);          // PANEL SETTING
    EPD_SendData(0x1F);

    EPD_SendCommand(0x50);
    EPD_SendData(0x10);
    EPD_SendData(0x07);

    EPD_SendCommand(0x04);          // POWER ON
    DEV_Delay_ms(100);
    EPD_WaitUntilIdle();

    EPD_SendCommand(0x06);          // Booster soft start
    EPD_SendData(0x27);
    EPD_SendData(0x27);
    EPD_SendData(0x18);
    EPD_SendData(0x17);

    EPD_SendCommand(0xE0);
    EPD_SendData(0x02);
    EPD_SendCommand(0xE5);
    EPD_SendData(0x5F);

    return 0;
}


/*
 * Image is 96,000 bytes: 4 pixels per byte, 2 bits each, leftmost pixel
 * in the HIGH bits (the loop masks 0xC0 then shifts left by 2).
 *
 *   0b11 (0xC0) = white
 *   0b10 (0x80) = gray1
 *   0b01 (0x40) = gray2
 *   0b00 (0x00) = black
 *
 * The panel gets two 1-bit planes, and the pair of plane values encodes
 * which of the four levels each pixel lands on. That is why the same
 * input byte is walked twice with different mappings.
 *
 * This sends 96,000 single-byte SPI writes, so expect it to take
 * noticeably longer than the 1-bit path -- seconds, not milliseconds.
 */
void EPD_7IN5_V2_Display_4Gray(const UBYTE *Image)
{
    UDOUBLE i, j, k;
    UBYTE temp1, temp2, temp3;

    EPD_SendCommand(0x10);          // first plane
    for (i = 0; i < 48000; i++) {
        temp3 = 0;
        for (j = 0; j < 2; j++) {
            temp1 = Image[i * 2 + j];
            for (k = 0; k < 2; k++) {
                temp2 = temp1 & 0xC0;
                if (temp2 == 0xC0)      temp3 |= 0x00;
                else if (temp2 == 0x00) temp3 |= 0x01;
                else if (temp2 == 0x80) temp3 |= 0x01;
                else                    temp3 |= 0x00;
                temp3 <<= 1;

                temp1 <<= 2;
                temp2 = temp1 & 0xC0;
                if (temp2 == 0xC0)      temp3 |= 0x00;
                else if (temp2 == 0x00) temp3 |= 0x01;
                else if (temp2 == 0x80) temp3 |= 0x01;
                else                    temp3 |= 0x00;
                if (j != 1 || k != 1)   temp3 <<= 1;

                temp1 <<= 2;
            }
        }
        EPD_SendData(temp3);
    }

    EPD_SendCommand(0x13);          // second plane
    for (i = 0; i < 48000; i++) {
        temp3 = 0;
        for (j = 0; j < 2; j++) {
            temp1 = Image[i * 2 + j];
            for (k = 0; k < 2; k++) {
                temp2 = temp1 & 0xC0;
                if (temp2 == 0xC0)      temp3 |= 0x00;   // white
                else if (temp2 == 0x00) temp3 |= 0x01;   // black
                else if (temp2 == 0x80) temp3 |= 0x00;   // gray1
                else                    temp3 |= 0x01;   // gray2
                temp3 <<= 1;

                temp1 <<= 2;
                temp2 = temp1 & 0xC0;
                if (temp2 == 0xC0)      temp3 |= 0x00;
                else if (temp2 == 0x00) temp3 |= 0x01;
                else if (temp2 == 0x80) temp3 |= 0x00;
                else                    temp3 |= 0x01;
                if (j != 1 || k != 1)   temp3 <<= 1;

                temp1 <<= 2;
            }
        }
        EPD_SendData(temp3);
    }

    EPD_7IN5_V2_TurnOnDisplay();
}


/* ============================================================ */
/* ===  PASTE THESE TWO INTO EPD_7in5_V2.h  =================== */
/* ============================================================ */
/*
UBYTE EPD_7IN5_V2_Init_4Gray(void);
void  EPD_7IN5_V2_Display_4Gray(const UBYTE *Image);
*/
