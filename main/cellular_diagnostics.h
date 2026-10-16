#pragma once
#include <stddef.h>
#include <stdint.h>

// Retains eight events; received SMS includes sender and UTF-8 body for USB display.
void cellular_diagnostics_record(const char *status);
void cellular_diagnostics_received(const char *sender, const char *message);
// Thread-safe JSON snapshot, including primary uptime; returns zero on overflow.
size_t cellular_diagnostics_json(char *buffer, size_t capacity,
                                 bool sms_busy = false, uint32_t sms_requests = 0);
