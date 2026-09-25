#pragma once

#include <stdio.h>
#include <string.h>
#include "cellular_protocol.h"

// Keep the message compatible with the already-flashed secondary SMS protocol.
inline bool build_alert_message(const char *name, char *message, size_t capacity)
{
    if (!message || !capacity) return false;
    message[0] = '\0';
    if (!name) return false;
    while (*name && strchr(" \t\r\n", *name)) ++name;
    size_t length = strlen(name);
    while (length && strchr(" \t\r\n", name[length - 1])) --length;
    if (!length || length > 63) return false;
    const int written = snprintf(message, capacity,
        "ALERT: %.*s's Backseat Minder device has detected an unattended passenger in their vehicle.",
        static_cast<int>(length), name);
    if (written < 0 || static_cast<size_t>(written) >= capacity ||
        !cellular::valid_message(message)) {
        message[0] = '\0';
        return false;
    }
    return true;
}
