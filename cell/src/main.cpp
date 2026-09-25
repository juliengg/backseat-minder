/* Secondary ESP32: bounded UART SMS service for the TEL0161/SIM7600G. */
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "cellular_protocol.h"

HardwareSerial modem(1);
HardwareSerial primary(2);

static cellular::LineBuffer input;
static bool busy = false;
static bool frameReceivedWhileBusy = false;
static uint32_t transactionStarted = 0;
static uint32_t modemStarted = 0;

static void servicePrimary();

static bool transactionExpired() {
  return millis() - transactionStarted >= cellular::MODEM_TIMEOUT_MS;
}

static void serviceDelay(uint32_t duration) {
  const uint32_t started = millis();
  while (millis() - started < duration && !transactionExpired()) {
    servicePrimary();
    delay(2);
  }
}

static void discardModemInput() {
  // Bounded even if a faulty module streams unsolicited bytes continuously.
  for (size_t n = 0; n < 1024 && modem.available(); ++n) modem.read();
}

static bool waitReply(char *reply, size_t capacity, const char *expected,
                      uint32_t timeoutMs, bool requireFinalOk = false) {
  size_t used = 0;
  reply[0] = '\0';
  const uint32_t started = millis();
  while (millis() - started < timeoutMs && !transactionExpired()) {
    servicePrimary(); // Reject another command promptly, including during CMGS.
    for (size_t n = 0; n < 64 && modem.available(); ++n) {
      const char c = static_cast<char>(modem.read());
      Serial.write(c);
      if (used + 1 >= capacity) {
        Serial.println("\nModem response too long.");
        return false;
      }
      reply[used++] = c;
      reply[used] = '\0';
      if (strstr(reply, "ERROR")) return false;
      if (strstr(reply, expected) &&
          (!requireFinalOk || strstr(reply, "\r\nOK\r\n"))) return true;
    }
    delay(2);
  }
  Serial.println("\nModem response timeout.");
  return false;
}

static bool command(const char *atCommand, char *reply, size_t capacity,
                    uint32_t timeoutMs = 3000) {
  if (transactionExpired()) return false;
  discardModemInput();
  Serial.printf("\n>> %s\n", atCommand);
  modem.print(atCommand);
  modem.print("\r\n");
  return waitReply(reply, capacity, "\r\nOK\r\n", timeoutMs);
}

static bool registered(const char *reply, const char *prefix) {
  const char *p = strstr(reply, prefix);
  if (!p) return false;
  p += strlen(prefix);
  char *end = nullptr;
  long status = strtol(p, &end, 10);
  if (end == p) return false;
  while (*end == ' ') ++end;
  if (*end == ',') {
    p = end + 1; // Read response is <n>,<stat>; URCs can contain just <stat>.
    status = strtol(p, &end, 10);
    if (end == p) return false;
  }
  return status == 1 || status == 5; // Home or roaming.
}

static const char *sendSms(const cellular::Sms &sms) {
  char reply[768];
  // The listener is live immediately, even during modem power-up.
  while (millis() - modemStarted < MODEM_BOOT_WAIT_MS && !transactionExpired()) {
    serviceDelay(20);
  }
  bool ready = false;
  for (uint8_t attempt = 1; attempt <= 10 && !transactionExpired(); ++attempt) {
    if (command("AT", reply, sizeof(reply))) {
      ready = true;
      break;
    }
    Serial.printf("No modem response (attempt %u/10).\n", attempt);
    serviceDelay(1000);
  }
  if (!ready) return "MODEM_UNAVAILABLE";
  if (!command("ATE0", reply, sizeof(reply)) ||
      !command("AT+CMEE=2", reply, sizeof(reply))) return "MODEM_SETUP";
  if (!command("AT+CPIN?", reply, sizeof(reply)) ||
      !strstr(reply, "+CPIN: READY")) return "SIM_NOT_READY";
  command("AT+CSQ", reply, sizeof(reply)); // Diagnostic, not a registration test.
  const bool circuitRegistered = command("AT+CREG?", reply, sizeof(reply)) &&
                                 registered(reply, "+CREG:");
  const bool epsRegistered = command("AT+CEREG?", reply, sizeof(reply)) &&
                             registered(reply, "+CEREG:");
  if (!circuitRegistered && !epsRegistered) return "NOT_REGISTERED";
  if (!command("AT+CMGF=1", reply, sizeof(reply)) ||
      !command("AT+CSCS=\"GSM\"", reply, sizeof(reply))) return "SMS_SETUP";
  if (transactionExpired()) return "MODEM_TIMEOUT";

  if (!SEND_REAL_SMS) {
    Serial.printf("DRY RUN: modem ready; would send %u characters. Recipient redacted.\n",
                  static_cast<unsigned>(strlen(sms.message)));
    // Never claim success for an SMS that was deliberately not submitted.
    return "DRY_RUN";
  }

  discardModemInput();
  Serial.println("\n>> AT+CMGS (recipient redacted)");
  modem.printf("AT+CMGS=\"%s\"\r\n", sms.number);
  if (!waitReply(reply, sizeof(reply), ">", 5000)) {
    modem.write(0x1B); // Escape text entry if the prompt arrived late.
    return "SMS_PROMPT";
  }
  if (transactionExpired()) {
    modem.write(0x1B);
    return "MODEM_TIMEOUT";
  }
  Serial.printf("Submitting %u characters.\n", static_cast<unsigned>(strlen(sms.message)));
  modem.print(sms.message);
  modem.write(0x1A);
  // One buffer retains both +CMGS and OK, even if they arrive in the same burst.
  if (!waitReply(reply, sizeof(reply), "+CMGS:", 65000, true)) {
    if (strstr(reply, "ERROR")) return "SMS_REJECTED";
    // After Ctrl-Z the network may already have accepted the message. Drain
    // late replies for the rest of the transaction window before going idle.
    // A later deliberate press is a new request; we never retry this one.
    while (!transactionExpired()) {
      servicePrimary();
      discardModemInput();
      delay(2);
    }
    return "SMS_OUTCOME_UNKNOWN";
  }
  return nullptr;
}

static void reportError(const char *reason) {
  primary.printf("RESULT|ERROR|%s\n", reason);
  Serial.printf("RESULT|ERROR|%s\n", reason);
}

static void servicePrimary() {
  // Bound each poll so malformed traffic cannot starve modem deadlines.
  for (size_t n = 0; n < 256 && primary.available(); ++n) {
    const char c = static_cast<char>(primary.read());
    frameReceivedWhileBusy |= busy;
    const auto event = input.push(c);
    const bool rejectBusy = frameReceivedWhileBusy;
    if (c == '\n') frameReceivedWhileBusy = false;
    if (event == cellular::FrameEvent::None) continue;
    if (event == cellular::FrameEvent::Oversized) {
      reportError("FRAME_TOO_LONG");
      continue;
    }
    if (event == cellular::FrameEvent::Invalid) {
      reportError("BAD_FRAME");
      continue;
    }
    if (rejectBusy) {
      reportError("BUSY");
      continue;
    }
    cellular::Sms sms = {};
    const char *error = cellular::parse_sms(input.data(), sms);
    if (error) {
      reportError(error);
      continue;
    }
    busy = true;
    transactionStarted = millis();
    primary.print("ACCEPTED\n");
    Serial.printf("SEND_SMS|<redacted>|%s\nACCEPTED\n", sms.message);
    error = sendSms(sms);
    // Handle already-buffered extra commands as BUSY before completing this one.
    servicePrimary();
    if (error) reportError(error);
    else {
      primary.print("RESULT|OK\n");
      Serial.println("RESULT|OK (modem accepted SMS; not a delivery receipt)");
    }
    busy = false;
  }
}

void setup() {
  Serial.begin(DEBUG_BAUD);
  primary.setRxBufferSize(1024);
  primary.begin(cellular::BAUD, SERIAL_8N1, PRIMARY_RX_PIN, PRIMARY_TX_PIN);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  modemStarted = millis();
  Serial.printf("\nTEL0161 UART SMS service ready: UART2 RX%d/TX%d, 115200 8N1.\n",
                PRIMARY_RX_PIN, PRIMARY_TX_PIN);
  Serial.println(SEND_REAL_SMS ? "Real SMS enabled." : "DRY RUN: real SMS disabled.");
}

void loop() {
  servicePrimary();
  discardModemInput();
  delay(2);
}
