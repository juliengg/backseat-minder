#include "cellular_link.h"
#include "cellular_diagnostics.h"
#include "cellular_protocol.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {
constexpr const char *TAG = "cellular_link";
constexpr uart_port_t LINK_UART = UART_NUM_2;
constexpr int TX_GPIO = 39;
constexpr int RX_GPIO = 40;
// The current custom camera map uses GPIO4-18; fail if it is reconfigured here.
constexpr bool link_pin(int pin) { return pin == TX_GPIO || pin == RX_GPIO; }
static_assert(!link_pin(CONFIG_CAMERA_PIN_XCLK) && !link_pin(CONFIG_CAMERA_PIN_SIOD) &&
              !link_pin(CONFIG_CAMERA_PIN_SIOC) && !link_pin(CONFIG_CAMERA_PIN_VSYNC) &&
              !link_pin(CONFIG_CAMERA_PIN_HREF) && !link_pin(CONFIG_CAMERA_PIN_PCLK) &&
              !link_pin(CONFIG_CAMERA_PIN_Y2) && !link_pin(CONFIG_CAMERA_PIN_Y3) &&
              !link_pin(CONFIG_CAMERA_PIN_Y4) && !link_pin(CONFIG_CAMERA_PIN_Y5) &&
              !link_pin(CONFIG_CAMERA_PIN_Y6) && !link_pin(CONFIG_CAMERA_PIN_Y7) &&
              !link_pin(CONFIG_CAMERA_PIN_Y8) && !link_pin(CONFIG_CAMERA_PIN_Y9) &&
              !link_pin(CONFIG_CAMERA_PIN_PWDN) && !link_pin(CONFIG_CAMERA_PIN_RESET),
              "Cellular UART conflicts with the camera");
QueueHandle_t requests = nullptr;
std::atomic<bool> busy{false};

void transact(const cellular::Sms &sms)
{
    char command[cellular::MAX_FRAME + 2];
    const int length = snprintf(command, sizeof(command), "SEND_SMS|%s|%s\n",
                                sms.number, sms.message);
    uart_flush_input(LINK_UART); // Discard stale responses before a new manual request.
    if (uart_write_bytes(LINK_UART, command, length) != length) {
        ESP_LOGE(TAG, "UART write failed; SMS outcome unknown, no retry");
        cellular_diagnostics_record("UART_WRITE_FAILED");
        return;
    }
    ESP_LOGI(TAG, "SMS command sent; waiting for ACCEPTED");
    cellular_diagnostics_record("WAITING_ACK");
    const int64_t started = esp_timer_get_time();
    bool accepted = false;
    bool ack_timed_out = false;
    cellular::LineBuffer frame;
    while (esp_timer_get_time() - started < cellular::RESULT_TIMEOUT_MS * 1000LL) {
        char bytes[128];
        const int count = uart_read_bytes(LINK_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        for (int i = 0; i < count; ++i) {
            const auto event = frame.push(bytes[i]);
            if (event == cellular::FrameEvent::Invalid || event == cellular::FrameEvent::Oversized) {
                ESP_LOGW(TAG, "Discarded invalid secondary response");
                cellular_diagnostics_record("INVALID_RESPONSE");
            }
            if (event != cellular::FrameEvent::Ready) continue;
            const char *line = frame.data();
            if (strcmp(line, "ACCEPTED") == 0) {
                accepted = true;
                ESP_LOGI(TAG, "Secondary: ACCEPTED");
                cellular_diagnostics_record("ACCEPTED");
            } else if (strcmp(line, "RESULT|OK") == 0 ||
                       (strncmp(line, "RESULT|ERROR|", 13) == 0 && line[13])) {
                if (accepted && strcmp(line, "RESULT|ERROR|BUSY") == 0) {
                    // This rejects an extra command, not the accepted SMS.
                    ESP_LOGW(TAG, "Secondary: BUSY; still waiting for the accepted SMS");
                    cellular_diagnostics_record("BUSY");
                    continue;
                }
                if (!accepted) ESP_LOGW(TAG, "Result arrived without acknowledgement");
                ESP_LOGI(TAG, "Secondary: %s", line);
                cellular_diagnostics_record(strcmp(line, "RESULT|OK") == 0 ? "OK" : line + 13);
                return;
            } else {
                ESP_LOGW(TAG, "Unexpected secondary response");
            }
        }
        if (!accepted && !ack_timed_out &&
            esp_timer_get_time() - started >= cellular::ACK_TIMEOUT_MS * 1000LL) {
            ack_timed_out = true;
            // An ACK can be lost after the modem started. Keep the link BUSY for
            // the full bounded transaction so another press cannot duplicate it.
            ESP_LOGW(TAG, "ACCEPTED timeout; check secondary power/wiring. Waiting for a possible result; no retry");
            cellular_diagnostics_record("ACK_TIMEOUT");
        }
    }
    ESP_LOGW(TAG, "RESULT timeout; SMS outcome unknown. No automatic retry");
    cellular_diagnostics_record("RESULT_TIMEOUT");
}

void link_task(void *)
{
    cellular::Sms sms;
    for (;;) {
        if (xQueueReceive(requests, &sms, portMAX_DELAY) == pdTRUE) {
            transact(sms);
            busy.store(false);
        }
    }
}
}

esp_err_t cellular_link_init()
{
    if (requests) return ESP_OK;
    uart_config_t config = {};
    config.baud_rate = cellular::BAUD;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_driver_install(LINK_UART, 1024, 512, 0, nullptr, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(LINK_UART, &config);
    if (err == ESP_OK) err = uart_set_pin(LINK_UART, TX_GPIO, RX_GPIO,
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) {
        requests = xQueueCreate(1, sizeof(cellular::Sms));
        if (!requests) err = ESP_ERR_NO_MEM;
        else if (xTaskCreate(link_task, "cellular_link", 4096, nullptr, 3, nullptr) != pdPASS) {
            vQueueDelete(requests);
            requests = nullptr;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err != ESP_OK) uart_driver_delete(LINK_UART);
    else {
        ESP_LOGI(TAG, "UART2 ready: TX=%d RX=%d, 115200 8N1", TX_GPIO, RX_GPIO);
        cellular_diagnostics_record("READY");
    }
    return err;
}

esp_err_t cellular_link_send_sms(const char *number, const char *message)
{
    if (!cellular::valid_number(number) || !cellular::valid_message(message)) {
        ESP_LOGW(TAG, "Invalid SMS number or message");
        cellular_diagnostics_record("INVALID_REQUEST");
        return ESP_ERR_INVALID_ARG;
    }
    if (!requests) {
        ESP_LOGW(TAG, "Cellular link unavailable");
        cellular_diagnostics_record("LINK_UNAVAILABLE");
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "BUSY: SMS already in progress");
        cellular_diagnostics_record("BUSY");
        return ESP_ERR_INVALID_STATE;
    }
    cellular::Sms sms = {};
    strcpy(sms.number, number);
    strcpy(sms.message, message);
    if (xQueueSend(requests, &sms, 0) != pdTRUE) {
        busy.store(false);
        cellular_diagnostics_record("QUEUE_FAILED");
        return ESP_FAIL;
    }
    return ESP_OK;
}
