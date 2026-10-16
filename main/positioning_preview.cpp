#include "positioning_preview.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
constexpr int64_t FRAME_INTERVAL_US = 1000000;
constexpr int64_t DEMAND_TIMEOUT_US = 2500000;
constexpr size_t MAX_JPEG_LENGTH = 64 * 1024;
SemaphoreHandle_t mutex = nullptr;
bool active = false;
bool attempted = false;
int64_t demand_until = 0;
int64_t last_attempt = 0;
int64_t captured_at = 0;
uint8_t *cached_jpeg = nullptr;
size_t cached_length = 0;
}

void positioning_preview_init()
{
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!mutex) ESP_LOGW("positioning_preview", "Preview mutex unavailable");
}

void positioning_preview_set_active(bool enabled)
{
    if (!mutex || xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    active = enabled;
    demand_until = 0;
    attempted = false;
    free(cached_jpeg);
    cached_jpeg = nullptr;
    cached_length = 0;
    xSemaphoreGive(mutex);
}

bool positioning_preview_frame_due(int64_t now_us)
{
    if (!mutex || xSemaphoreTake(mutex, 0) != pdTRUE) return false;
    const bool due = active && now_us < demand_until &&
                     (!attempted || now_us - last_attempt >= FRAME_INTERVAL_US);
    if (due) {
        last_attempt = now_us;
        attempted = true;
    }
    xSemaphoreGive(mutex);
    return due;
}

void positioning_preview_publish(const uint8_t *jpeg, size_t length)
{
    if (!jpeg || !length || length > MAX_JPEG_LENGTH || !mutex) return;
    uint8_t *copy = static_cast<uint8_t *>(malloc(length));
    if (!copy) return;
    memcpy(copy, jpeg, length);
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        free(copy);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (active && now < demand_until) {
        free(cached_jpeg);
        cached_jpeg = copy;
        cached_length = length;
        captured_at = now;
        copy = nullptr;
    }
    xSemaphoreGive(mutex);
    free(copy);
}

uint8_t *positioning_preview_snapshot(size_t *length)
{
    if (!length) return nullptr;
    *length = 0;
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) return nullptr;
    uint8_t *copy = nullptr;
    const int64_t now = esp_timer_get_time();
    if (active) {
        demand_until = now + DEMAND_TIMEOUT_US;
        if (cached_jpeg && now - captured_at < DEMAND_TIMEOUT_US) {
            copy = static_cast<uint8_t *>(malloc(cached_length));
            if (copy) {
                memcpy(copy, cached_jpeg, cached_length);
                *length = cached_length;
            }
        }
    }
    xSemaphoreGive(mutex);
    return copy;
}
