#include "can_horn.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>

namespace can_horn {
namespace {
MCP2515 controller(CAN_CS_PIN, CAN_SPI_CLOCK_HZ, &SPI);
bool ready = false;
const can_frame hornOn = {0x358, 8, {0x42, 0x02, 0, 0, 0, 0, 0, 0}};
const can_frame hornOff = {0x358, 8, {0x02, 0x02, 0, 0, 0, 0, 0, 0}};

bool sendFrame(const char *label, const can_frame &frame) {
  const auto result = controller.sendMessage(&frame);
  Serial.printf("t=%lu ms %s ID=0x%03lX DATA=", static_cast<unsigned long>(millis()),
                label, static_cast<unsigned long>(frame.can_id));
  for (uint8_t i = 0; i < frame.can_dlc; ++i) Serial.printf("%02X ", frame.data[i]);
  Serial.printf("result=%d\n", static_cast<int>(result));
  if (result == MCP2515::ERROR_OK) return true;
  Serial.printf("TEC=%u REC=%u EFLG=0x%02X\n", controller.errorCountTX(),
                controller.errorCountRX(), controller.getErrorFlags());
  return false;
}
}

bool begin() {
  ready = false;
  pinMode(CAN_INT_PIN, INPUT); // Reserved/wired; this transmit-only service polls.
  SPI.begin(CAN_SCK_PIN, CAN_MISO_PIN, CAN_MOSI_PIN, CAN_CS_PIN);
  auto result = controller.reset();
  if (result == MCP2515::ERROR_OK) result = controller.setBitrate(CAN_500KBPS, MCP_8MHZ);
  if (result == MCP2515::ERROR_OK) result = controller.setNormalMode();
  ready = result == MCP2515::ERROR_OK;
  Serial.printf("MCP2515 %s (code=%d): 500 kbps, 8 MHz, SPI 1 MHz; no startup frames.\n",
                ready ? "ready" : "unavailable", static_cast<int>(result));
  return ready;
}

const char *chirp(void (*service_primary)()) {
  if (!ready) return "CAN_UNAVAILABLE";
  Serial.println("----- Q50 HORN CHIRP START -----");
  const uint32_t start = millis();
  constexpr uint32_t onTimes[] = {0, 20, 120, 220};
  bool success = true;
  for (const auto at : onTimes) {
    while (millis() - start < at) {
      service_primary();
      delay(1);
    }
    // Do not short-circuit the sequence after an error; OFF must be attempted.
    if (!sendFrame("ON", hornOn)) success = false;
  }
  while (millis() - start < 240) {
    service_primary();
    delay(1);
  }
  const bool offSent = sendFrame("OFF", hornOff);
  Serial.println("------ Q50 HORN CHIRP END ------");
  if (!offSent || !success) {
    // Clear pending frames after a reported failure rather than leaving late ONs
    // queued on a disconnected bus. A new initialization is required to recover.
    controller.reset();
    ready = false;
    return offSent ? "CAN_SEND_FAILED" : "CAN_OFF_FAILED";
  }
  return nullptr;
}
} // namespace can_horn
