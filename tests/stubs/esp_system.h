#pragma once

enum esp_reset_reason_t { ESP_RST_TASK_WDT = 6 };

esp_reset_reason_t esp_reset_reason();
[[noreturn]] void esp_restart();
