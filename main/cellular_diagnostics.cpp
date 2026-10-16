#include "cellular_diagnostics.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include "cellular_protocol.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
struct Event {
    long long uptime_ms;
    char status[48];
    char sender[cellular::PHONE_CAPACITY];
    char message[cellular::MAX_RECEIVED_MESSAGE + 1];
};
Event events[8] = {};
size_t count = 0;
size_t next = 0;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

void record(Event &event) {
    event.uptime_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&mutex);
    events[next] = event;
    next = (next + 1) % 8;
    if (count < 8) ++count;
    portEXIT_CRITICAL(&mutex);
}

bool json_string(char *buffer, size_t capacity, size_t &used, const char *value) {
    if (used + 1 >= capacity) return false;
    buffer[used++] = '"';
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            if (used + 2 >= capacity) return false;
            buffer[used++] = '\\';
            buffer[used++] = *p == '\n' ? 'n' : *p == '\r' ? 'r' : 't';
        } else if (*p < 32) {
            if (used + 6 >= capacity) return false;
            snprintf(buffer + used, capacity - used, "\\u%04x", *p);
            used += 6;
        } else {
            if (used + 2 >= capacity) return false;
            if (*p == '"' || *p == '\\') buffer[used++] = '\\';
            buffer[used++] = static_cast<char>(*p);
        }
    }
    if (used + 1 >= capacity) return false;
    buffer[used++] = '"';
    buffer[used] = '\0';
    return true;
}
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
    strcpy(event.status, status);
    record(event);
}

void cellular_diagnostics_received(const char *sender, const char *message)
{
    if (!sender || !*sender || strlen(sender) >= cellular::PHONE_CAPACITY ||
        !message || strlen(message) > cellular::MAX_RECEIVED_MESSAGE) return;
    Event event = {};
    strcpy(event.status, "SMS_RECEIVED");
    strcpy(event.sender, sender);
    strcpy(event.message, message);
    record(event);
}

size_t cellular_diagnostics_json(char *buffer, size_t capacity,
                                 bool sms_busy, uint32_t sms_requests)
{
    // Keep the larger SMS snapshot off the main task's small stack.
    std::unique_ptr<Event[]> snapshot(new (std::nothrow) Event[8]);
    if (!snapshot) return 0;
    portENTER_CRITICAL(&mutex);
    const size_t size = count;
    for (size_t i = 0; i < size; ++i) snapshot[i] = events[(next + 8 - size + i) % 8];
    portEXIT_CRITICAL(&mutex);
    int written = snprintf(buffer, capacity,
                           "{\"uptime_ms\":%lld,\"sms_control\":true,\"sms_busy\":%s,"
                           "\"sms_requests\":%lu,\"events\":[",
                           static_cast<long long>(esp_timer_get_time() / 1000),
                           sms_busy ? "true" : "false",
                           static_cast<unsigned long>(sms_requests));
    if (written < 0 || static_cast<size_t>(written) >= capacity) return 0;
    size_t used = written;
    for (size_t i = 0; i < size; ++i) {
        written = snprintf(buffer + used, capacity - used,
                           "%s{\"uptime_ms\":%lld,\"status\":\"%s\"",
                           i ? "," : "", snapshot[i].uptime_ms, snapshot[i].status);
        if (written < 0 || static_cast<size_t>(written) >= capacity - used) return 0;
        used += written;
        if (!strcmp(snapshot[i].status, "SMS_RECEIVED")) {
            const char *keys[] = {",\"sender\":", ",\"message\":"};
            const char *values[] = {snapshot[i].sender, snapshot[i].message};
            for (size_t field = 0; field < 2; ++field) {
                const size_t length = strlen(keys[field]);
                if (used + length >= capacity) return 0;
                memcpy(buffer + used, keys[field], length);
                used += length;
                if (!json_string(buffer, capacity, used, values[field])) return 0;
            }
        }
        if (used + 1 >= capacity) return 0;
        buffer[used++] = '}';
    }
    if (capacity - used < 3) return 0;
    memcpy(buffer + used, "]}", 3);
    return used + 2;
}
