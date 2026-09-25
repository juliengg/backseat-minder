#pragma once

#include <stdint.h>

// Secondary ESP32-WROOM-32: explicitly routed UART1 modem pins.
// ESP32 RX receives TEL0161 TX; ESP32 TX drives TEL0161 RX.
constexpr int MODEM_RX_PIN = 18;
constexpr int MODEM_TX_PIN = 19;

// UART2 to the primary ESP32. Cross TX/RX and connect grounds.
// Primary S3 TX39 -> secondary RX26; secondary TX27 -> primary S3 RX40.
constexpr int PRIMARY_RX_PIN = 26;
constexpr int PRIMARY_TX_PIN = 27;

constexpr uint32_t DEBUG_BAUD = 115200;
constexpr uint32_t MODEM_BAUD = 115200;
constexpr uint32_t MODEM_BOOT_WAIT_MS = 8000;

// Leave false until you have an active, SMS-capable SIM and have tested the wiring.
// A build flag can override this for the real-SMS build and host tests.
#ifndef BSM_SEND_REAL_SMS
#define BSM_SEND_REAL_SMS 0
#endif
constexpr bool SEND_REAL_SMS = BSM_SEND_REAL_SMS != 0;
