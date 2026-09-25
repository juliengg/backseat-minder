#pragma once
#include <stddef.h>

// Retains eight summary events without USB I/O or recipient/message contents.
void cellular_diagnostics_record(const char *status);
// Thread-safe JSON snapshot, including primary uptime; returns zero on overflow.
size_t cellular_diagnostics_json(char *buffer, size_t capacity);
