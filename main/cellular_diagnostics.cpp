#include "cellular_diagnostics.h"
#include <cstdio>
#include <cstring>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
struct Event { long long uptime_ms; char status[48]; };
Event events[8] = {};
size_t count = 0;
size_t next = 0;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
}

void cellular_diagnostics_record(const char *status)
{
    // Only protocol status tokens go into JSON, never arbitrary modem text.
    bool valid = status && *status;
    size_t length = 0;
    if (status) {
        for (; length < 48 && status[length]; ++length) {
            const char c = status[length];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) valid = false;
        }
    }
    if (!valid || length >= 48) status = "UNKNOWN_ERROR";
    Event event = {};
    event.uptime_ms = esp_timer_get_time() / 1000;
    strcpy(event.status, status);
    portENTER_CRITICAL(&mutex);
    events[next] = event;
    next = (next + 1) % 8;
    if (count < 8) ++count;
    portEXIT_CRITICAL(&mutex);
}

size_t cellular_diagnostics_json(char *buffer, size_t capacity)
{
    Event snapshot[8];
    portENTER_CRITICAL(&mutex);
    const size_t size = count;
    for (size_t i = 0; i < size; ++i) snapshot[i] = events[(next + 8 - size + i) % 8];
    portEXIT_CRITICAL(&mutex);
    int written = snprintf(buffer, capacity, "{\"uptime_ms\":%lld,\"events\":[",
                           static_cast<long long>(esp_timer_get_time() / 1000));
    if (written < 0 || static_cast<size_t>(written) >= capacity) return 0;
    size_t used = written;
    for (size_t i = 0; i < size; ++i) {
        written = snprintf(buffer + used, capacity - used,
                           "%s{\"uptime_ms\":%lld,\"status\":\"%s\"}",
                           i ? "," : "", snapshot[i].uptime_ms, snapshot[i].status);
        if (written < 0 || static_cast<size_t>(written) >= capacity - used) return 0;
        used += written;
    }
    if (capacity - used < 3) return 0;
    memcpy(buffer + used, "]}", 3);
    return used + 2;
}
