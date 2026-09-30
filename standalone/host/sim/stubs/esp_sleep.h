#pragma once
#include <stdint.h>
void esp_sleep_enable_timer_wakeup(uint64_t us);
[[noreturn]] void esp_deep_sleep_start();
