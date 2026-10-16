#pragma once

namespace can_horn {
// Failure leaves SMS available. No CAN data frames are sent by initialization.
bool begin();
// User-tested Q50 sequence; pumps UART during waits and always attempts OFF.
// Returns nullptr when all frames were submitted, otherwise a fixed error token.
const char *chirp(void (*service_primary)());
}
