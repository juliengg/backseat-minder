#pragma once

// Freenove ESP32-S3 WROOM UART1 pins from its supplied pinout.
// ESP32 RX receives TEL0161 TX; ESP32 TX drives TEL0161 RX.
constexpr int MODEM_RX_PIN = 18;
constexpr int MODEM_TX_PIN = 17;

// The Freenove BOOT button connects to GPIO0. It is the temporary sensor trigger.
// Do not hold it while powering/resetting the ESP32 because GPIO0 is also a boot pin.
constexpr int TRIGGER_BUTTON_PIN = 0;

constexpr uint32_t DEBUG_BAUD = 115200;
constexpr uint32_t MODEM_BAUD = 115200;
constexpr uint32_t MODEM_BOOT_WAIT_MS = 8000;

// Use international E.164 form: +, country code, then number; no spaces/dashes.
constexpr char DESTINATION_NUMBER[] = "+15129622126";
constexpr char SMS_MESSAGE[] = "Hello, from Backseat Minder!";

// APN used only for the cell-tower location fallback. "wholesale" is Tello's APN.
// If you use a different carrier, replace this with that carrier's data APN.
constexpr char CELLULAR_APN[] = "wholesale";

// Leave false until you have an active, SMS-capable SIM and have tested the wiring.
constexpr bool SEND_REAL_SMS = true;
