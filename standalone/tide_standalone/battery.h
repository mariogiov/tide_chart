// battery.h -- battery voltage, if the board can measure it.
#pragma once

// Millivolts at the battery, or -1 if BATTERY_ADC_PIN isn't set.
int battery_mv();
