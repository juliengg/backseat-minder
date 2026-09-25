#pragma once

#include "esp_err.h"

// UART2, TX GPIO39 / RX GPIO40, 115200 8N1. Keep the microSD slot empty.
esp_err_t cellular_link_init();
// Copies and queues one SMS without waiting for the secondary or modem.
// ESP_ERR_INVALID_STATE means BUSY or link unavailable. Never retries an SMS.
esp_err_t cellular_link_send_sms(const char *number, const char *message);
