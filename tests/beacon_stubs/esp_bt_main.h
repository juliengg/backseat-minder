#pragma once
#include "esp_err.h"
enum esp_bluedroid_status_t { ESP_BLUEDROID_STATUS_UNINITIALIZED, ESP_BLUEDROID_STATUS_INITIALIZED, ESP_BLUEDROID_STATUS_ENABLED };
esp_bluedroid_status_t esp_bluedroid_get_status();
esp_err_t esp_bluedroid_init();
esp_err_t esp_bluedroid_enable();
esp_err_t esp_bluedroid_disable();
esp_err_t esp_bluedroid_deinit();
