#pragma once

#include "esp_err.h"

// UART2, TX GPIO39 / RX GPIO40, 115200 8N1. Keep the microSD slot empty.
esp_err_t cellular_link_init();
// Copies and queues one SMS without waiting for the secondary or modem.
// ESP_ERR_INVALID_STATE means BUSY or link unavailable. Never retries an SMS.
esp_err_t cellular_link_send_sms(const char *number, const char *message);
// Same UART worker and BUSY gate as SMS. No queueing behind an active action.
// Horn results use HORN_* diagnostics and a separate bounded timeout; no retries.
esp_err_t cellular_link_send_horn();
// Includes queued requests and the entire SMS/horn acknowledgement/result wait.
bool cellular_link_busy();
