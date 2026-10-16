#pragma once
#include "cellular_protocol.h"

namespace sms_service {
void begin();
void poll(); // Configure reception and read notified SMS while idle (nonblocking).
bool receiving_busy(); // Receiver owns the AT channel for a short command.
// Bounded transaction. Pumps the primary UART during waits; nullptr means success.
const char *send(const cellular::Sms &sms, void (*service_primary)());
}
