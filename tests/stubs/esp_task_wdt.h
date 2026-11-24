#pragma once

#include "esp_err.h"

esp_err_t esp_task_wdt_add(void *task);
esp_err_t esp_task_wdt_reset();
esp_err_t esp_task_wdt_status(void *task);
