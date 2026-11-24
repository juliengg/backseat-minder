#pragma once
#include <stddef.h>
#include <stdint.h>

// Retains eight events; received SMS includes sender and UTF-8 body for USB display.
void cellular_diagnostics_record(const char *status);
void cellular_diagnostics_received(const char *sender, const char *message);

struct TelemetryTestStatus {
    bool busy = false;
    bool alert_active = false;
    uint32_t sms_requests = 0;
    uint32_t call_requests = 0;
    uint32_t horn_requests = 0;
};
// Thread-safe JSON snapshot, including primary uptime; returns zero on overflow.
size_t cellular_diagnostics_json(char *buffer, size_t capacity,
                                 const TelemetryTestStatus &tests = {});
