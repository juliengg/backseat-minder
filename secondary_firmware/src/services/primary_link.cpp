#include "primary_link.h"
#include "sms_service.h"
#include "can_horn.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

namespace primary_link {
namespace {
HardwareSerial primary(2);
cellular::LineBuffer input;
bool busy = false;
bool frameReceivedWhileBusy = false;

void reportError(const char *reason, bool horn = false) {
  const char *prefix = horn ? "HORN_RESULT" : "RESULT";
  primary.printf("%s|ERROR|%s\n", prefix, reason);
  Serial.printf("%s|ERROR|%s\n", prefix, reason);
}
}

void begin() {
  input = cellular::LineBuffer{};
  busy = frameReceivedWhileBusy = false;
  primary.setRxBufferSize(1024);
  primary.begin(cellular::BAUD, SERIAL_8N1, PRIMARY_RX_PIN, PRIMARY_TX_PIN);
  Serial.printf("\nSMS/CAN UART service ready: UART2 RX%d/TX%d, 115200 8N1.\n",
                PRIMARY_RX_PIN, PRIMARY_TX_PIN);
}

void receive_status(const char *status) {
  primary.printf("SMS_RX_STATUS|%s\n", status);
}

void received_sms(const char *sender, const char *text) {
  primary.printf("RECEIVED_SMS|%s|", sender);
  const char *hex = "0123456789ABCDEF";
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
    primary.write(hex[*p >> 4]);
    primary.write(hex[*p & 15]);
  }
  primary.print("\n");
  Serial.println("Incoming SMS forwarded to primary.");
}

void poll() {
  // Bound each poll so malformed traffic cannot starve modem/CAN deadlines.
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
    const bool horn = strcmp(input.data(), cellular::HORN_COMMAND) == 0;
    if (rejectBusy || (!horn && sms_service::receiving_busy())) {
      reportError("BUSY", horn);
      continue;
    }
    cellular::Sms sms = {};
    const char *error = horn ? nullptr : cellular::parse_sms(input.data(), sms);
    if (error) {
      reportError(error);
      continue;
    }
    busy = true;
    primary.print(horn ? "HORN_ACCEPTED\n" : "ACCEPTED\n");
    Serial.println(horn ? "HORN_CHIRP accepted" : "SEND_SMS accepted (recipient/message redacted)");
    error = horn ? can_horn::chirp(poll) : sms_service::send(sms, poll);
    // Reject buffered commands before releasing the shared transaction slot.
    poll();
    if (error) reportError(error, horn);
    else {
      primary.print(horn ? "HORN_RESULT|OK\n" : "RESULT|OK\n");
      Serial.println(horn ? "Horn sequence submitted (vehicle action not verified)."
                          : "SMS accepted by modem (not a delivery receipt).");
    }
    busy = false;
  }
}
} // namespace primary_link
