/* Send one SMS with a DFRobot TEL0161 (SIM7600G) from a Freenove ESP32-S3 WROOM. */

#include <Arduino.h>
#include "config.h"

HardwareSerial modem(1);
static bool buttonWasPressed = false;

static bool waitFor(const char *expected, uint32_t timeoutMs) {
  String received;
  const uint32_t started = millis();

  while (millis() - started < timeoutMs) {
    while (modem.available()) {
      const char c = static_cast<char>(modem.read());
      Serial.write(c);
      received += c;
      if (received.indexOf(expected) >= 0) return true;
      if (received.indexOf("ERROR") >= 0 || received.indexOf("+CMS ERROR:") >= 0 ||
          received.indexOf("+CME ERROR:") >= 0) return false;
    }
    delay(2);
  }

  Serial.printf("\nTimed out waiting for: %s\n", expected);
  return false;
}

static bool command(const char *atCommand, const char *expected = "OK", uint32_t timeoutMs = 3000) {
  while (modem.available()) modem.read();
  Serial.printf("\n>> %s\n", atCommand);
  modem.print(atCommand);
  modem.print("\r\n");
  return waitFor(expected, timeoutMs);
}

static bool modemIsReady() {
  for (uint8_t attempt = 1; attempt <= 10; ++attempt) {
    if (command("AT")) return true;
    Serial.printf("No response yet (attempt %u/10).\n", attempt);
    delay(1000);
  }
  return false;
}

static bool sendSms(const char *number, const char *message) {
  if (!command("AT+CMGF=1")) return false;
  if (!command("AT+CSCS=\"GSM\"")) return false;

  while (modem.available()) modem.read();
  Serial.printf("\n>> AT+CMGS=\"%s\"\n", number);
  modem.printf("AT+CMGS=\"%s\"\r\n", number);
  if (!waitFor(">", 5000)) {
    Serial.println("The modem did not accept the SMS recipient.");
    return false;
  }

  Serial.printf(">> Sending: %s\n", message);
  modem.print(message);
  modem.write(0x1A);  // Ctrl-Z submits the SMS.
  if (!waitFor("+CMGS:", 60000)) return false;
  return waitFor("OK", 5000);
}

static bool readModemReply(const char *atCommand, String &reply, uint32_t timeoutMs = 5000) {
  while (modem.available()) modem.read();
  reply = "";
  Serial.printf("\n>> %s\n", atCommand);
  modem.print(atCommand);
  modem.print("\r\n");

  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    while (modem.available()) {
      const char c = static_cast<char>(modem.read());
      Serial.write(c);
      reply += c;
      if (reply.indexOf("ERROR") >= 0) return false;
      if (reply.indexOf("\r\nOK\r\n") >= 0) return true;
    }
    delay(2);
  }

  Serial.println("\nTimed out waiting for a modem response.");
  return false;
}

static String gnssField(const String &line, uint8_t wantedField) {
  uint8_t field = 0;
  int start = 0;
  for (int i = 0; i <= line.length(); ++i) {
    if (i == line.length() || line[i] == ',') {
      if (field == wantedField) return line.substring(start, i);
      ++field;
      start = i + 1;
    }
  }
  return "";
}

static bool getCellTowerLocation(String &location) {
  Serial.println("Trying approximate cell-tower location fallback...");
  // The SIM7600 LBS service needs a PDP data connection with the carrier's APN.
  const String pdpContext = String("AT+CGDCONT=1,\"IP\",\"") + CELLULAR_APN + "\"";
  if (!command(pdpContext.c_str()) || !command("AT+CSOCKSETPN=1")) {
    Serial.println("Could not configure cellular data for cell-tower location.");
    return false;
  }

  String reply;
  readModemReply("AT+CNETSTART", reply, 30000);
  if (reply.indexOf("+CNETSTART: 0") < 0 && reply.indexOf("+CNETSTART:0") < 0) {
    Serial.println("Cellular data did not start. Check that the SIM plan includes data and that CELLULAR_APN is correct.");
    return false;
  }

  const bool responseReceived = readModemReply("AT+CLBS=1", reply, 30000);
  command("AT+CNETSTOP", "OK", 10000);
  if (!responseReceived) return false;

  const int prefix = reply.indexOf("+CLBS:");
  if (prefix < 0) return false;
  const int lineEnd = reply.indexOf('\n', prefix);
  String line = reply.substring(prefix + 6, lineEnd < 0 ? reply.length() : lineEnd);
  line.trim();

  const String resultCode = gnssField(line, 0);
  const String latitude = gnssField(line, 1);
  const String longitude = gnssField(line, 2);
  if (resultCode != "0" || latitude.length() == 0 || longitude.length() == 0) {
    Serial.printf("Cell-tower location was unavailable (result code: %s).\n", resultCode.c_str());
    return false;
  }

  location = "[" + longitude + ", " + latitude + "]";
  Serial.println("Using approximate cell-tower location.");
  return true;
}

static void handleTrigger() {
  Serial.println("\nTrigger received; checking the TEL0161...");
  if (!modemIsReady()) {
    Serial.println("ERROR: No modem response. Check power, UART wiring, pins, and baud rate.");
    return;
  }

  command("ATE0");
  command("AT+CMEE=2");
  command("AT+CPIN?");
  command("AT+CSQ");
  command("AT+CREG?");
  command("AT+CEREG?");

  String smsMessage = SMS_MESSAGE;
  String location;
  if (getCellTowerLocation(location)) {
    smsMessage += " Approximate location: ";
    smsMessage += location;
  } else {
    smsMessage += " Location: unavailable";
  }

  if (!SEND_REAL_SMS) {
    Serial.printf("\nTEST MODE: would send to %s:\n%s\n", DESTINATION_NUMBER, smsMessage.c_str());
    Serial.println("Set SEND_REAL_SMS to true in include/config.h once the SIM is active and tested.");
    return;
  }

  Serial.println("\nSending SMS...");
  if (sendSms(DESTINATION_NUMBER, smsMessage.c_str())) {
    Serial.println("\nSUCCESS: modem accepted the SMS for sending.");
  } else {
    Serial.println("\nFAILED: see the modem response above for the reason.");
  }
}

void setup() {
  Serial.begin(DEBUG_BAUD);
  delay(1000);
  Serial.println("\nTEL0161 SMS sender ready.");
  Serial.println("Press and release BOOT to run the SMS test.");

  pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(MODEM_BOOT_WAIT_MS);
}

void loop() {
  // Ignore unsolicited modem bytes while idle. Some modules emit binary
  // diagnostics at startup; responses are still printed during a trigger.
  while (modem.available()) modem.read();

  const bool buttonIsPressed = digitalRead(TRIGGER_BUTTON_PIN) == LOW;
  if (buttonIsPressed && !buttonWasPressed) {
    delay(30);  // Simple debounce before treating it as a trigger.
    if (digitalRead(TRIGGER_BUTTON_PIN) == LOW) handleTrigger();
  }
  buttonWasPressed = buttonIsPressed;
  delay(20);
}
