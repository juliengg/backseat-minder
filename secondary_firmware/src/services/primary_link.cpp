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
bool stopPending = false, hornStopped = true, stopErrorReported = false;
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
  stopPending = false; hornStopped = true; stopErrorReported=false;
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
  can_horn::poll();
  if (stopPending && sms_service::finish_cancel()) {
    if(hornStopped) { primary.print("ALERT_STOPPED\n"); stopPending=false; }
    else if(!stopErrorReported) { primary.print("ALERT_STOP_ERROR|CAN_OFF_FAILED\n"); stopErrorReported=true; }
  }
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
    if (!strcmp(input.data(), cellular::ALERT_STOP_COMMAND)) {
      sms_service::request_cancel();
      hornStopped = can_horn::stop_alarm(); stopPending = true; stopErrorReported=false;
      continue; // Urgent cancellation is accepted even inside an active call.
    }
    if(!strcmp(input.data(),"HORN_REPEAT")) {
      if(stopPending) { primary.print("ALARM_RESULT|ERROR|BUSY\n"); continue; }
      primary.print("ALARM_ACCEPTED\n");
      const char *error=can_horn::start_alarm();
      if(error) primary.printf("ALARM_RESULT|ERROR|%s\n",error);
      else primary.print("ALARM_RESULT|OK\n");
      continue; // CAN scheduling is independent of an active modem transaction.
    }
    const bool horn = strcmp(input.data(), cellular::HORN_COMMAND) == 0;
    const bool voice = !strncmp(input.data(), "CALL|", 5);
    const char *prefix = voice ? "CALL_RESULT" : horn ? "HORN_RESULT" : "RESULT";
    auto errorReply = [&](const char *reason) {
      primary.printf("%s|ERROR|%s\n",prefix,reason);
      Serial.printf("%s|ERROR|%s\n",prefix,reason);
    };
    if (rejectBusy || stopPending || sms_service::transmitting_busy()) {
      errorReply("BUSY"); continue;
    }
    cellular::Sms sms = {};
    const char *error = voice ? (cellular::valid_call_number(input.data()+5) ? nullptr : "BAD_NUMBER") :
                        horn ? nullptr : cellular::parse_sms(input.data(),sms);
    if(error) { errorReply(error); continue; }
    if(voice) strcpy(sms.number,input.data()+5);
    busy = true;
    primary.print(voice ? "CALL_ACCEPTED\n" : horn ? "HORN_ACCEPTED\n" : "ACCEPTED\n");
    error = voice ? sms_service::call(sms.number,poll) :
            horn ? can_horn::chirp(poll) : sms_service::send(sms,poll);
    poll();
    if(error) errorReply(error);
    else primary.printf("%s|OK\n",prefix);
    busy = false;
  }
}
} // namespace primary_link
