#include "battery.h"

#include <Arduino.h>

#include "config.h"

int battery_mv() {
#if BATTERY_ADC_PIN >= 0
#if BATTERY_ENABLE_PIN >= 0
  pinMode(BATTERY_ENABLE_PIN, OUTPUT);
  digitalWrite(BATTERY_ENABLE_PIN, HIGH);
  delay(5);
#endif
  analogReadResolution(12);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATTERY_ADC_PIN);
#if BATTERY_ENABLE_PIN >= 0
  digitalWrite(BATTERY_ENABLE_PIN, LOW);
#endif
  return (int)(sum / 16 * BATTERY_DIVIDER);
#else
  return -1;
#endif
}
