#ifdef BSM_MCP_DIAGNOSTIC
#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>
#include "config.h"
#include <cstring>

namespace {
// MCP2515 SPI READ/WRITE instructions and register addresses, also defined by
// the pinned driver's private register API. Access directly for readback tests.
constexpr uint8_t CANSTAT = 0x0e, CANCTRL = 0x0f;
constexpr uint8_t CNF3 = 0x28, CNF2 = 0x29, CNF1 = 0x2a;
uint32_t spiHz = CAN_SPI_CLOCK_HZ;

uint8_t readReg(uint8_t address) {
  SPI.beginTransaction(SPISettings(spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(CAN_CS_PIN, LOW);
  SPI.transfer(0x03);
  SPI.transfer(address);
  const uint8_t value = SPI.transfer(0);
  digitalWrite(CAN_CS_PIN, HIGH);
  SPI.endTransaction();
  return value;
}

void writeReg(uint8_t address, uint8_t value) {
  SPI.beginTransaction(SPISettings(spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(CAN_CS_PIN, LOW);
  SPI.transfer(0x02);
  SPI.transfer(address);
  SPI.transfer(value);
  digitalWrite(CAN_CS_PIN, HIGH);
  SPI.endTransaction();
}

bool registerTest(MCP2515 &controller) {
  const auto reset = controller.reset();
  const auto config = controller.setConfigMode();
  Serial.printf("reset=%d config=%d CANSTAT=0x%02X CANCTRL=0x%02X\n",
                reset, config, readReg(CANSTAT), readReg(CANCTRL));
  if (reset != MCP2515::ERROR_OK || config != MCP2515::ERROR_OK) return false;
  unsigned failures = 0;
  // All bit patterns are writable in these timing registers in config mode.
  // No normal-mode transition occurs with these temporary timing settings.
  const uint8_t addresses[] = {CNF1, CNF2, CNF3};
  const uint8_t patterns[] = {0x00, 0x55, 0xaa, 0xff};
  // CNF3 bits 5:3 are reserved, so mask them out of writes and comparison.
  for (unsigned round = 0; round < 25; ++round) {
    for (uint8_t address : addresses) {
      const uint8_t mask = address == CNF3 ? 0xc7 : 0xff;
      for (uint8_t pattern : patterns) {
        const uint8_t expected = pattern & mask;
        writeReg(address, expected);
        const uint8_t actual = readReg(address) & mask;
        if (actual != expected) {
          if (failures < 8) Serial.printf("READBACK FAIL reg=0x%02X expected=0x%02X actual=0x%02X\n", address, expected, actual);
          ++failures;
        }
      }
    }
    if ((readReg(CANSTAT) & 0xe0) != 0x80 ||
        (readReg(CANCTRL) & 0xe0) != 0x80) ++failures;
  }
  Serial.printf("REGISTER TEST: %s (%u mismatches; 300 write/read pairs plus mode checks)\n",
                failures ? "FAIL" : "PASS", failures);
  return failures == 0;
}

bool loopbackTest(MCP2515 &controller) {
  if (controller.reset() != MCP2515::ERROR_OK ||
      controller.setBitrate(CAN_500KBPS, MCP_8MHZ) != MCP2515::ERROR_OK ||
      controller.setLoopbackMode() != MCP2515::ERROR_OK ||
      (readReg(CANSTAT) & 0xe0) != 0x40) {
    Serial.println("LOOPBACK TEST: FAIL entering loopback");
    return false;
  }
  for (unsigned i = 0; i < 16; ++i) {
    can_frame sent = {};
    sent.can_id = 0x123;
    sent.can_dlc = 8;
    for (unsigned j = 0; j < 8; ++j) sent.data[j] = (i * 17) ^ j;
    const auto tx = controller.sendMessage(&sent);
    can_frame received = {};
    auto rx = MCP2515::ERROR_NOMSG;
    const uint32_t start = millis();
    while (millis() - start < 100) {
      rx = controller.readMessage(&received);
      if (rx != MCP2515::ERROR_NOMSG) break;
      delay(1);
    }
    if (tx != MCP2515::ERROR_OK || rx != MCP2515::ERROR_OK ||
        received.can_id != sent.can_id || received.can_dlc != sent.can_dlc ||
        memcmp(received.data, sent.data, 8) != 0) {
      Serial.printf("LOOPBACK TEST: FAIL frame=%u tx=%d rx=%d id=0x%lX dlc=%u\n",
                    i, tx, rx, static_cast<unsigned long>(received.can_id), received.can_dlc);
      return false;
    }
  }
  Serial.println("LOOPBACK TEST: PASS (16 exact frame matches)");
  return true;
}

void runTests() {
  Serial.println("\nMCP2515 BENCH DIAGNOSTIC: internal loopback only; no horn/primary/modem service.");
  Serial.printf("SCK=%d MOSI=%d MISO=%d CS=%d; CAN=500 kbps, crystal=8 MHz\n",
                CAN_SCK_PIN, CAN_MOSI_PIN, CAN_MISO_PIN, CAN_CS_PIN);
  for (uint32_t speed : {uint32_t(100000), uint32_t(CAN_SPI_CLOCK_HZ)}) {
    spiHz = speed;
    MCP2515 controller(CAN_CS_PIN, speed, &SPI);
    Serial.printf("\n--- SPI %lu Hz ---\n", static_cast<unsigned long>(speed));
    const bool registers = registerTest(controller);
    if (registers) loopbackTest(controller);
    else Serial.println("LOOPBACK TEST: SKIPPED; fix register communication first.");
    // Remain in configuration mode after testing, never normal mode.
    const auto stopped = controller.setConfigMode();
    Serial.printf("Final config mode request=%d CANSTAT=0x%02X\n", stopped, readReg(CANSTAT));
  }
  Serial.println("Done. Send 'r' over USB serial or reset secondary to repeat.");
}
}

void mcpDiagnosticSetup() {
  Serial.begin(DEBUG_BAUD);
  pinMode(CAN_CS_PIN, OUTPUT);
  digitalWrite(CAN_CS_PIN, HIGH);
  SPI.begin(CAN_SCK_PIN, CAN_MISO_PIN, CAN_MOSI_PIN, CAN_CS_PIN);
  delay(1000);
  runTests();
}

void mcpDiagnosticPoll() {
  if (Serial.available() && Serial.read() == 'r') runTests();
  delay(10);
}
#endif
