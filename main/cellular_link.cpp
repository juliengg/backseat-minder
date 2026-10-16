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
cellular::ReceiveLineBuffer incoming;

bool receive_event(char *line)
{
    constexpr const char *prefix = "RECEIVED_SMS|";
    if (!strncmp(line, "SMS_RX_STATUS|", 14)) {
        const char *status = line + 14;
        if (!strcmp(status, "RX_READY") || !strcmp(status, "RX_ERROR") ||
            !strcmp(status, "RX_QUEUE_FULL")) cellular_diagnostics_record(status);
        return true;
    }
    if (strncmp(line, prefix, strlen(prefix))) return false;
    char *sender = line + strlen(prefix);
    char *body = strchr(sender, '|');
    if (!body) return true;
    *body++ = '\0';
    const size_t length = strlen(sender);
    if (!length || length >= cellular::PHONE_CAPACITY) return true;
    for (size_t i = sender[0] == '+' ? 1 : 0; i < length; ++i)
        if (sender[i] < '0' || sender[i] > '9') return true;
    if (!strcmp(sender, "+")) return true;
    char message[cellular::MAX_RECEIVED_MESSAGE + 1];
    size_t size = 0;
    if (!cellular::decode_hex(body, message, sizeof(message), size) ||
        memchr(message, '\0', size)) return true;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = message[i];
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return true;
    }
    cellular_diagnostics_received(sender, message);
    ESP_LOGI(TAG, "Incoming SMS received; available in USB cellular log");
    return true;
}

void poll_incoming(TickType_t wait)
{
    char bytes[128];
    const int count = uart_read_bytes(LINK_UART, bytes, sizeof(bytes), wait);
    for (int i = 0; i < count; ++i)
        if (incoming.push(bytes[i]) == cellular::FrameEvent::Ready)
            receive_event(incoming.data());
}

void record_status(bool horn, const char *status)
{
    char token[48];
    snprintf(token, sizeof(token), "%s%s", horn ? "HORN_" : "", status);
    cellular_diagnostics_record(token);
}

void transact(const cellular::Request &request)
{
    const bool horn = request.action == cellular::Action::Horn;
    const char *label = horn ? "Horn" : "SMS";
    const char *ack = horn ? "HORN_ACCEPTED" : "ACCEPTED";
    const char *ok = horn ? "HORN_RESULT|OK" : "RESULT|OK";
    const char *error_prefix = horn ? "HORN_RESULT|ERROR|" : "RESULT|ERROR|";
    const size_t error_length = strlen(error_prefix);
    const uint32_t deadline = horn ? cellular::HORN_RESULT_TIMEOUT_MS : cellular::RESULT_TIMEOUT_MS;
    const uint32_t ack_deadline = horn ? cellular::HORN_ACK_TIMEOUT_MS : cellular::ACK_TIMEOUT_MS;
    char command[cellular::MAX_FRAME + 2];
    const int length = horn
        ? snprintf(command, sizeof(command), "%s\n", cellular::HORN_COMMAND)
        : snprintf(command, sizeof(command), "SEND_SMS|%s|%s\n", request.sms.number, request.sms.message);
    // Preserve incoming SMS and partial frames while discarding stale action replies.
    for (int i = 0; i < 16; ++i) poll_incoming(0);
    if (uart_write_bytes(LINK_UART, command, length) != length) {
        ESP_LOGE(TAG, "%s UART write failed; outcome unknown, no retry", label);
        record_status(horn, "UART_WRITE_FAILED");
        return;
    }
    ESP_LOGI(TAG, "%s command sent; waiting for %s", label, ack);
    record_status(horn, "WAITING_ACK");
    const int64_t started = esp_timer_get_time();
    bool accepted = false;
    bool ack_timed_out = false;
    while (esp_timer_get_time() - started < deadline * 1000LL) {
        char bytes[128];
        const int count = uart_read_bytes(LINK_UART, bytes, sizeof(bytes), pdMS_TO_TICKS(20));
        bool completed = false;
        for (int i = 0; i < count; ++i) {
            const auto event = incoming.push(bytes[i]);
            if (event == cellular::FrameEvent::Invalid || event == cellular::FrameEvent::Oversized) {
                ESP_LOGW(TAG, "Discarded invalid secondary response");
                record_status(horn, "INVALID_RESPONSE");
            }
            if (event != cellular::FrameEvent::Ready) continue;
            if (receive_event(incoming.data())) continue;
            const char *line = incoming.data();
            if (strcmp(line, ack) == 0) {
                accepted = true;
                ESP_LOGI(TAG, "Secondary: %s", ack);
                record_status(horn, "ACCEPTED");
            } else if (strcmp(line, ok) == 0 ||
                       (strncmp(line, error_prefix, error_length) == 0 && line[error_length])) {
                if (accepted && strcmp(line, ok) != 0 && strcmp(line + error_length, "BUSY") == 0) {
                    // A rejection of an extra command cannot finish the active action.
                    ESP_LOGW(TAG, "Secondary: BUSY; still waiting for the accepted action");
                    record_status(horn, "BUSY");
                    continue;
                }
                if (!accepted) ESP_LOGW(TAG, "Result arrived without acknowledgement");
                ESP_LOGI(TAG, "Secondary: %s", line);
                record_status(horn, strcmp(line, ok) == 0 ? "OK" : line + error_length);
                completed = true;
            } else {
                // Typed horn replies must never complete an SMS, or vice versa.
                ESP_LOGW(TAG, "Unexpected secondary response");
            }
        }
        if (completed) return; // Process the whole burst, including SMS after RESULT.
        if (!accepted && !ack_timed_out && esp_timer_get_time() - started >= ack_deadline * 1000LL) {
            ack_timed_out = true;
            ESP_LOGW(TAG, "ACCEPTED timeout; check secondary power/wiring. Waiting for a possible result; no retry");
            record_status(horn, "ACK_TIMEOUT");
        }
    }
    ESP_LOGW(TAG, "RESULT timeout; %s outcome unknown. No automatic retry", label);
    record_status(horn, "RESULT_TIMEOUT");
}

esp_err_t enqueue(const cellular::Request &request)
{
    const bool horn = request.action == cellular::Action::Horn;
    if (!requests) {
        record_status(horn, "LINK_UNAVAILABLE");
        return ESP_ERR_INVALID_STATE;
    }
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "BUSY: SMS or horn action already in progress; request rejected");
        record_status(horn, "BUSY");
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        busy.store(false);
        record_status(horn, "QUEUE_FAILED");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void link_task(void *)
{
    cellular::Request request;
    for (;;) {
        if (xQueueReceive(requests, &request, 0) == pdTRUE) {
            transact(request);
            busy.store(false);
        } else poll_incoming(pdMS_TO_TICKS(20));
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
        requests = xQueueCreate(1, sizeof(cellular::Request));
        if (!requests) err = ESP_ERR_NO_MEM;
        else if (xTaskCreate(link_task, "cellular_link", 8192, nullptr, 3, nullptr) != pdPASS) {
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
    cellular::Request request;
    strcpy(request.sms.number, number);
    strcpy(request.sms.message, message);
    return enqueue(request);
}

esp_err_t cellular_link_send_horn()
{
    cellular::Request request;
    request.action = cellular::Action::Horn;
    return enqueue(request);
}

bool cellular_link_busy()
{
    return busy.load();
}
