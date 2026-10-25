#pragma once
#include <cstdint>
#include "esp_err.h"
enum esp_sleep_wakeup_cause_t { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_EXT0 };
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause();
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t);
esp_err_t esp_sleep_enable_ext0_wakeup(int, int);
void esp_deep_sleep_start();
