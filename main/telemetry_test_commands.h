#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace telemetry_test {
constexpr uint8_t SMS = 1;
constexpr uint8_t CALL = 2;
constexpr uint8_t HORN = 4;

inline uint8_t parse_command(const char *command, size_t length)
{
    if (length == 8 && !std::memcmp(command, "SEND_SMS", length)) return SMS;
    if (length == 9 && !std::memcmp(command, "TEST_CALL", length)) return CALL;
    if (length == 9 && !std::memcmp(command, "TEST_HORN", length)) return HORN;
    return 0;
}
}
