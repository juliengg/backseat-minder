/* Secondary ESP32: UART commands for the SMS modem and MCP2515 horn. */
#include <Arduino.h>
#ifdef BSM_MCP_DIAGNOSTIC
void mcpDiagnosticSetup();
void mcpDiagnosticPoll();
void setup() { mcpDiagnosticSetup(); }
void loop() { mcpDiagnosticPoll(); }
#else
#include "config.h"
#include "services/primary_link.h"
#include "services/sms_service.h"
#include "services/can_horn.h"

void setup() {
  Serial.begin(DEBUG_BAUD);
  sms_service::begin();
  can_horn::begin(); // Initialization only: never sends a horn frame.
  primary_link::begin();
}

void loop() {
  primary_link::poll(); // Dispatches SMS or horn commands; rejects overlap.
  sms_service::poll();
  delay(2);
}
#endif
