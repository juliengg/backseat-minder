#include "can_horn.h"
#include "config.h"
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>

namespace can_horn {
namespace {
MCP2515 controller(CAN_CS_PIN, CAN_SPI_CLOCK_HZ, &SPI);
bool ready = false;
bool alarm = false;
bool stopUnconfirmed = false;
bool chirping = false, chirpCancelled = false;
uint32_t alarmStarted = 0, lastOn = 0;
bool onPhase = false;
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
  ready = false; alarm = onPhase = stopUnconfirmed = chirping = chirpCancelled = false;
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

const char *start_alarm() {
  if (!ready) return "CAN_UNAVAILABLE";
  if (alarm) return nullptr;
  alarm = onPhase = true; alarmStarted = lastOn = millis();
  if (sendFrame("ALARM ON", hornOn)) return nullptr;
  stopUnconfirmed = !sendFrame("ALARM OFF", hornOff); controller.reset(); ready = alarm = onPhase = false;
  return "CAN_SEND_FAILED";
}
bool stop_alarm() {
  if(chirping) chirpCancelled=true;
  if (!alarm && !onPhase && !stopUnconfirmed && !chirping) return true;
  alarm = false;
  const bool ok = sendFrame("ALARM OFF", hornOff);
  onPhase = false; stopUnconfirmed = !ok;
  if (!ok) { controller.reset(); ready = false; }
  return ok;
}
void poll() {
  if (!alarm || !ready) return;
  const uint32_t now = millis();
  const bool shouldOn = (now - alarmStarted) % 3000 < 1000;
  if (shouldOn && (!onPhase || now - lastOn >= 100)) {
    if (!sendFrame("ALARM ON", hornOn)) {
      stop_alarm(); controller.reset(); ready = false; return;
    }
    lastOn = now;
  } else if (!shouldOn && onPhase) {
    if (!sendFrame("ALARM OFF", hornOff)) {
      alarm = false; stopUnconfirmed = true; controller.reset(); ready = false;
    }
  }
  onPhase = shouldOn;
}

const char *chirp(void (*service_primary)()) {
  if (!ready) return "CAN_UNAVAILABLE";
  if (alarm) return "BUSY";
  chirping=true; chirpCancelled=false;
  Serial.println("----- Q50 HORN CHIRP START -----");
  const uint32_t start = millis();
  constexpr uint32_t onTimes[] = {0, 20, 120, 220};
  bool success = true;
  for (const auto at : onTimes) {
    while (!chirpCancelled && millis() - start < at) {
      service_primary();
      delay(1);
    }
    if(chirpCancelled) break;
    // Do not short-circuit the sequence after an error; OFF must be attempted.
    if (!sendFrame("ON", hornOn)) success = false;
  }
  while (!chirpCancelled && millis() - start < 240) {
    service_primary();
    delay(1);
  }
  const bool offSent = sendFrame("OFF", hornOff);
  chirping=false;
  Serial.println("------ Q50 HORN CHIRP END ------");
  if (!offSent) stopUnconfirmed=true;
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
