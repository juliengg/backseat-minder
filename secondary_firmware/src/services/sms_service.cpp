#include "sms_service.h"
#include "config.h"
#include "alert_audio.h"
#include "primary_link.h"
#include "can_horn.h"
#include "received_sms.h"
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

namespace sms_service {
namespace {
HardwareSerial modem(1);
uint32_t transactionStarted = 0;
uint32_t transactionLimit = cellular::MODEM_TIMEOUT_MS;
bool calling = false, voiceConnected = false;
bool cancelRequested = false, privateReply = false, reserved = false;
bool voiceEnded = false, wavDone = false, wavError = false;
bool audioReady = false, transferUncertain = false;
uint32_t modemStarted = 0;
void (*servicePrimary)() = nullptr;
cellular::BasicLineBuffer<512> modemLine;
struct PendingSms { char storage[3]; unsigned index; };
PendingSms pending[8] = {};
size_t pendingCount = 0;
enum class RxState { Idle, Setup, Storage, Read, Delete };
RxState rxState = RxState::Idle;
bool receiveReady = false, sending = false, waitingBody = false, haveMessage = false;
bool receiveAnnounced = false;
unsigned setupStep = 0;
uint32_t rxStarted = 0, retryAt = 0;
received_sms::Message received;
const char *const receiveSetup[] = {"ATE0", "AT+CMGF=0", "AT+CNMI=2,1,0,0,0"};
const char *rxCommand = "none";

void startRxCommand(RxState state, const char *command) {
  rxState = state;
  rxStarted = millis();
  // Command names contain no phone numbers or message content.
  rxCommand = state == RxState::Setup ? receiveSetup[setupStep] :
              state == RxState::Storage ? "AT+CPMS" :
              state == RxState::Read ? "AT+CMGR" : "AT+CMGD";
  Serial.printf("[SMS RX] >> %s\n", rxCommand);
  modem.print(command);
  modem.print("\r\n");
}

void finishReceive(bool success, const char *reason = "unspecified") {
  if (!success) {
    Serial.printf("[SMS RX] %s failed after %lu ms: %s\n", rxCommand,
                  static_cast<unsigned long>(millis() - rxStarted), reason);
    primary_link::receive_status("RX_ERROR");
    receiveAnnounced = false;
  }
  if (rxState == RxState::Setup) {
    receiveReady = false;
    setupStep = 0;
    retryAt = millis() + 15000;
  } else if (pendingCount) {
    for (size_t i = 1; i < pendingCount; ++i) pending[i - 1] = pending[i];
    --pendingCount;
  }
  rxState = RxState::Idle;
  waitingBody = haveMessage = false;
}

void modemInput(char c) {
  const auto event = modemLine.push(c);
  if (event == cellular::FrameEvent::Invalid || event == cellular::FrameEvent::Oversized) {
    if (!sending && rxState != RxState::Idle) finishReceive(false, "invalid or oversized modem line");
    return;
  }
  if (event != cellular::FrameEvent::Ready) return;
  const char *line = modemLine.data();
  if (!strcmp(line, "NO CARRIER") || !strcmp(line, "BUSY") ||
      !strcmp(line, "NO ANSWER") || !strncmp(line, "VOICE CALL: END", 15)) {
    voiceEnded = true;
    voiceConnected = false;
  }
  if (calling) {
    if (!strcmp(line, "+WAVSTATE: wav play stop")) wavDone = true;
    if (strstr(line, "wav play error")) wavError = true;
  }
  if (strncmp(line, "+CMTI:", 6) == 0) {
    PendingSms item = {};
    char extra;
    // Only known storage names and a numeric slot may enter an AT command.
    if (sscanf(line, "+CMTI: \"%2[A-Z]\",%u %c", item.storage, &item.index, &extra) != 2 ||
        (strcmp(item.storage, "SM") && strcmp(item.storage, "ME")) || item.index > 65535) return;
    for (size_t i = 0; i < pendingCount; ++i)
      if (pending[i].index == item.index && !strcmp(pending[i].storage, item.storage)) return;
    if (pendingCount == 8) primary_link::receive_status("RX_QUEUE_FULL");
    else pending[pendingCount++] = item;
    return;
  }
  if (sending || rxState == RxState::Idle) return;
  // Log only terminal responses; SMS headers and PDUs remain private.
  if (!strcmp(line, "OK") || !strcmp(line, "ERROR") ||
      !strncmp(line, "+CMS ERROR:", 11) || !strncmp(line, "+CME ERROR:", 11))
    Serial.printf("[SMS RX] << %s\n", line);
  if (waitingBody) {
    waitingBody = false;
    haveMessage = received_sms::decode(line, received);
    return;
  }
  if (rxState == RxState::Read && strncmp(line, "+CMGR:", 6) == 0) {
    waitingBody = true;
  } else if (!strcmp(line, "ERROR") || !strncmp(line, "+CMS ERROR:", 11) ||
             !strncmp(line, "+CME ERROR:", 11)) {
    finishReceive(false, "modem rejected command (see response above)");
  } else if (!strcmp(line, "OK")) {
    char command[48];
    switch (rxState) {
      case RxState::Setup:
        if (++setupStep < sizeof(receiveSetup) / sizeof(receiveSetup[0]))
          startRxCommand(RxState::Setup, receiveSetup[setupStep]);
        else {
          receiveReady = true;
          Serial.println("[SMS RX] Incoming SMS reception ready.");
          rxState = RxState::Idle;
          if (!receiveAnnounced) primary_link::receive_status("RX_READY");
          receiveAnnounced = true;
        }
        break;
      case RxState::Storage:
        waitingBody = haveMessage = false;
        snprintf(command, sizeof(command), "AT+CMGR=%u", pending[0].index);
        startRxCommand(RxState::Read, command);
        break;
      case RxState::Read:
        if (!haveMessage) { finishReceive(false, "missing or unsupported SMS PDU"); break; }
        primary_link::received_sms(received.sender, received.text);
        // Remove only the forwarded slot so the SIM's SMS storage cannot fill.
        snprintf(command, sizeof(command), "AT+CMGD=%u", pending[0].index);
        startRxCommand(RxState::Delete, command);
        break;
      case RxState::Delete: finishReceive(true); break;
      case RxState::Idle: break;
    }
  }
}

void drain() {
  for (size_t n = 0; n < 1024 && modem.available(); ++n)
    modemInput(static_cast<char>(modem.read()));
}
}

void begin() {
  cancelRequested = privateReply = reserved = false;
  calling = voiceConnected = voiceEnded = wavDone = wavError = false;
  // Audio cache is valid only for the current modem/ESP32 boot session.
  audioReady = transferUncertain = false;
  transactionLimit = cellular::MODEM_TIMEOUT_MS;
  modemLine = cellular::BasicLineBuffer<512>{};
  pendingCount = setupStep = 0;
  rxState = RxState::Idle;
  receiveReady = sending = waitingBody = haveMessage = false;
  receiveAnnounced = false;
  retryAt = 0;
  modem.setRxBufferSize(2048);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  modemStarted = millis();
  Serial.println(SEND_REAL_SMS ? "Real SMS enabled." : "DRY RUN: real SMS disabled.");
}

static bool transactionExpired() {
  return millis() - transactionStarted >= transactionLimit;
}

static void serviceDelay(uint32_t duration) {
  const uint32_t started = millis();
  while (millis() - started < duration && !transactionExpired()) {
    can_horn::poll();
    if (servicePrimary) servicePrimary();
    delay(2);
  }
}

void poll() {
  drain();
  if (sending || transferUncertain) return;
  if (rxState != RxState::Idle) {
    if (millis() - rxStarted >= 5000) {
      finishReceive(false, "no final response within 5000 ms; retrying setup in 15 s");
      // Let late replies drain before starting another background command.
      receiveReady = false;
      setupStep = 0;
      retryAt = millis() + 15000;
    }
    return;
  }
  if (millis() - modemStarted < MODEM_BOOT_WAIT_MS) return;
  if (!receiveReady) {
    if (static_cast<int32_t>(millis() - retryAt) < 0) return;
    setupStep = 0;
    startRxCommand(RxState::Setup, receiveSetup[0]);
  } else if (pendingCount) {
    char command[48];
    snprintf(command, sizeof(command), "AT+CPMS=\"%s\"", pending[0].storage);
    startRxCommand(RxState::Storage, command);
  }
}

bool transmitting_busy() { return sending || reserved || transferUncertain; }
bool receiving_busy() { return transmitting_busy() || rxState != RxState::Idle; }

static bool waitReply(char *reply, size_t capacity, const char *expected,
                      uint32_t timeoutMs, bool requireFinalOk = false) {
  size_t used = 0;
  reply[0] = '\0';
  const uint32_t started = millis();
  while (millis() - started < timeoutMs && !transactionExpired()) {
    if (calling && cancelRequested && !transferUncertain) return false;
    can_horn::poll();
    if (servicePrimary) servicePrimary(); // Reject another command promptly, including during CMGS.
    for (size_t n = 0; n < 64 && modem.available(); ++n) {
      const char c = static_cast<char>(modem.read());
      modemInput(c); // Preserve +CMTI notifications during outgoing transactions.
      if (!privateReply) Serial.write(c);
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
  drain();
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

// A short background inbox read must not discard an escalation command.
struct Reservation {
  Reservation() { reserved=true; }
  ~Reservation() { reserved=false; }
};
static bool finishBackgroundReceive(void (*service)()) {
  const uint32_t started=millis();
  while(rxState!=RxState::Idle && millis()-started<6000) {
    if(service) service();
    if(cancelRequested) return false;
    can_horn::poll();
    poll();
    delay(2);
  }
  return rxState==RxState::Idle;
}

const char *send(const cellular::Sms &sms, void (*service)()) {
  if (transmitting_busy()) return "BUSY";
  Reservation reservation;
  cancelRequested=false;
  if(!finishBackgroundReceive(service)) return cancelRequested?"CANCELLED":"BUSY";
  struct SendingGuard {
    SendingGuard() { sending = true; receiveReady = false; setupStep = 0; retryAt = 0; }
    ~SendingGuard() { sending = false; }
  } guard;
  servicePrimary = service;
  transactionStarted = millis();
  transactionLimit = cellular::MODEM_TIMEOUT_MS;
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

  drain();
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
    // A later deliberate request is a new request; we never retry this one.
    while (!transactionExpired()) {
      can_horn::poll();
    if (servicePrimary) servicePrimary();
      drain();
      delay(2);
    }
    return "SMS_OUTCOME_UNKNOWN";
  }
  return nullptr;
}


// Read stored SMS between voice AT transactions so STOP works while dialing.
static void voiceInbox() {
  if (!pendingCount || cancelRequested || transferUncertain) return;
  privateReply = true;
  char at[48], reply[1536];
  snprintf(at,sizeof(at),"AT+CPMS=\"%s\"",pending[0].storage);
  if (command(at,reply,sizeof(reply))) {
    snprintf(at,sizeof(at),"AT+CMGR=%u",pending[0].index);
    if (command(at,reply,sizeof(reply))) {
      char *header=strstr(reply,"+CMGR:");
      char *pdu=header ? strchr(header,'\n') : nullptr;
      if(pdu) {
        while(*pdu=='\r'||*pdu=='\n') ++pdu;
        if(char *end=strpbrk(pdu,"\r\n")) *end='\0';
        received_sms::Message msg;
        if(received_sms::decode(pdu,msg)) primary_link::received_sms(msg.sender,msg.text);
      }
      // Complete forwarding before releasing this slot. Cancellation may skip
      // deletion; the already forwarded message is not requeued by this parser.
      snprintf(at,sizeof(at),"AT+CMGD=%u",pending[0].index);
      command(at,reply,sizeof(reply));
      for(size_t i=1;i<pendingCount;++i) pending[i-1]=pending[i];
      --pendingCount;
    }
  }
  privateReply = false;
}
void request_cancel() { cancelRequested = true; }
bool finish_cancel() {
  if (sending) return false;
  if (!voiceConnected) return true;
  sending = true;
  char reply[128];
  transactionStarted=millis(); transactionLimit=5000;
  const bool ok=command("AT+CHUP",reply,sizeof(reply));
  sending=false;
  if(ok) voiceConnected=false;
  return ok;
}

// Query exact CLCC fields rather than treating command acceptance as an answer.
static int voiceState(char *reply, size_t capacity) {
  if (!command("AT+CLCC", reply, capacity)) return -1;
  int state = 0;
  for (const char *p = reply; (p = strstr(p, "+CLCC:")); p += 6) {
    int id, direction, status, mode, multiparty;
    if (sscanf(p, "+CLCC: %d,%d,%d,%d,%d", &id, &direction, &status,
               &mode, &multiparty) != 5) return -1;
    if (status != 6 && state < 1) state = 1;
    if (direction == 0 && status == 0 && mode == 0) state = 2;
  }
  return state;
}

const char *call(const char *number, void (*service)()) {
  if (!cellular::valid_call_number(number)) return "BAD_NUMBER";
  if (transferUncertain) return "AUDIO_TRANSFER_UNCERTAIN";
  if (transmitting_busy() || voiceConnected) return "BUSY";
  if (!ENABLE_VOICE_CALLS) return "DRY_RUN";
  Reservation reservation;
  cancelRequested=false;
  if(!finishBackgroundReceive(service)) return cancelRequested?"CANCELLED":"BUSY";
  struct VoiceGuard {
    VoiceGuard() { sending = calling = true; }
    ~VoiceGuard() { sending = calling = false; }
  } guard;
  cancelRequested = false;
  servicePrimary = service;
  transactionStarted = millis();
  transactionLimit = 150000;
  char reply[768];
  while (millis() - modemStarted < MODEM_BOOT_WAIT_MS && !transactionExpired())
    serviceDelay(20);
  if (!command("AT", reply, sizeof(reply))) return "MODEM_UNAVAILABLE";
  if (!command("ATE0", reply, sizeof(reply)) ||
      !command("AT+CMGF=0", reply, sizeof(reply))) return "MODEM_SETUP";
  if (voiceState(reply, sizeof(reply)) != 0) return "CALL_BUSY_OR_UNKNOWN";
  if (!audioReady) {
    if (!command("AT+CCMXPLAYWAV=?", reply, sizeof(reply)) ||
        !command("AT+FSCD=E:/", reply, sizeof(reply))) return "AUDIO_SETUP";
    char at[96];
    snprintf(at, sizeof(at), "AT+FSDEL=%s", AUDIO_PATH + 3);
    command(at, reply, sizeof(reply)); // Missing generated file is normal.
    snprintf(at, sizeof(at), "AT+CFTRANRX=\"%s\",%u", AUDIO_PATH,
             static_cast<unsigned>(sizeof(AUDIO_WAV)));
    drain();
    modem.printf("%s\r\n", at);
    // From this point a timeout may leave the modem in binary receive mode.
    transferUncertain = true;
    if (!waitReply(reply, sizeof(reply), ">", 10000)) return "AUDIO_TRANSFER_UNCERTAIN";
    for (size_t offset = 0; offset < sizeof(AUDIO_WAV); offset += 256) {
      if (transactionExpired()) return "AUDIO_TRANSFER_UNCERTAIN";
      const size_t remaining = sizeof(AUDIO_WAV) - offset;
      const size_t count = remaining < 256 ? remaining : 256;
      if (modem.write(AUDIO_WAV + offset, count) != count)
        return "AUDIO_TRANSFER_UNCERTAIN";
      modem.flush();
      serviceDelay(5);
    }
    if (!waitReply(reply, sizeof(reply), "\r\nOK\r\n", 15000))
      return "AUDIO_TRANSFER_UNCERTAIN";
    transferUncertain = false;
    audioReady = true;
  }
  auto endAttempt = [&](const char *reason) -> const char * {
    // Allow cleanup its own response window even after the transaction deadline.
    calling = false;
    transactionStarted = millis();
    transactionLimit = 5000;
    if (!command("AT+CHUP", reply, sizeof(reply))) {
      voiceConnected = true; // Block further sends until disconnect is observed.
      return "HANGUP_UNCONFIRMED";
    }
    voiceConnected = false;
    return reason;
  };
  if (cancelRequested) return "CANCELLED";
  voiceEnded = wavDone = wavError = false;
  drain();
  Serial.println("Dialing voice call (recipient redacted).");
  modem.printf("ATD%s;\r\n", number);
  if (!waitReply(reply, sizeof(reply), "\r\nOK\r\n", 15000))
    return endAttempt(cancelRequested?"CANCELLED":"CALL_DIAL_FAILED");
  const uint32_t dialed = millis();
  bool answered = false;
  while (!voiceEnded && !transactionExpired() &&
         millis() - dialed < VOICE_ANSWER_TIMEOUT_MS) {
    voiceInbox();
    if (cancelRequested) return endAttempt("CANCELLED");
    const int state = voiceState(reply, sizeof(reply));
    if (state == 2 && !voiceEnded) { answered = true; break; }
    if (state == 0) break;
    serviceDelay(500);
  }
  if (cancelRequested) return endAttempt("CANCELLED");
  if (!answered) return endAttempt("CALL_NOT_ANSWERED");
  voiceConnected = true;
  serviceDelay(1000);
  drain();
  if (cancelRequested) return endAttempt("CANCELLED");
  if (voiceEnded) return "CALL_DISCONNECTED";
  char play[96];
  snprintf(play, sizeof(play), "AT+CCMXPLAYWAV=\"%s\",1", AUDIO_PATH);
  if (!command(play, reply, sizeof(reply))) return endAttempt(cancelRequested?"CANCELLED":"PLAYBACK_FAILED");
  const uint32_t started = millis();
  while (!wavDone && !wavError && !voiceEnded && !transactionExpired() &&
         millis() - started < AUDIO_DURATION_MS + 10000) {
    can_horn::poll();
    if (servicePrimary) servicePrimary();
    voiceInbox();
    if (cancelRequested) return endAttempt("CANCELLED");
    drain(); // Same parser retains +CMTI notifications throughout the call.
    delay(2);
  }
  if (voiceEnded) return "CALL_DISCONNECTED";
  if (wavDone && !wavError) return nullptr; // Recipient ends successful call.
  return endAttempt(cancelRequested?"CANCELLED":"PLAYBACK_FAILED");
}

} // namespace sms_service
