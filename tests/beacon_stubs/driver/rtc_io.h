#pragma once
#include "esp_err.h"
esp_err_t rtc_gpio_deinit(int);
esp_err_t rtc_gpio_pullup_en(int);
esp_err_t rtc_gpio_pulldown_dis(int);
