#pragma once
#include "cellular_protocol.h"

namespace sms_service {
void begin();
void poll(); // Configure reception and read notified SMS while idle (nonblocking).
// Blocking voice playback using the same modem and SMS notification parser.
// nullptr = recording finished; DRY_RUN means no call was made.
// Successful playback leaves the call connected until recipient hangs up.
const char *call(const char *number, void (*service_primary)());
bool transmitting_busy(); // Also blocks horn dispatch during direct voice calls.
bool receiving_busy(); // Receiver owns the AT channel for a short command.
// Bounded transaction. Pumps the primary UART during waits; nullptr means success.
const char *send(const cellular::Sms &sms, void (*service_primary)());
}

namespace sms_service {
void request_cancel();
bool finish_cancel();
}
