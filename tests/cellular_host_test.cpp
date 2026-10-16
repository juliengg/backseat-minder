#include <cassert>
#include <cstdio>
#include <string>
#include "cellular_protocol.h"
#include "boot_button.h"
#include "../secondary_firmware/src/main.cpp"
#include "../secondary_firmware/src/services/sms_service.cpp"
#include "../secondary_firmware/src/services/can_horn.cpp"
#include "../secondary_firmware/src/services/primary_link.cpp"

using sms_service::modem;
using primary_link::primary;
using primary_link::input;
using primary_link::busy;
using primary_link::frameReceivedWhileBusy;

HardwareSerial Serial(0);
int test_button_level = 1;
static uint32_t now = 0;
static int64_t buttonUs = 0;
enum class ModemCase { Ready, Absent, NoSim, NoNetwork, NoPrompt, Reject, NoResult, Slow };
static ModemCase modemCase = ModemCase::Ready;
static std::string atLine;
static bool enteringSms = false;
static unsigned submissions = 0;
static bool injectBusyCommand = false;
static std::string delayedReply;
static uint32_t replyAt = 0;
static bool injectHornDuringSms = false;
static bool injectDuringHorn = false;
static bool injectIncoming = false;
static std::string incomingPdu;

uint32_t millis() { return now; }
int64_t esp_timer_get_time() { return buttonUs; }
void delay(uint32_t ms) {
    now += ms;
    if (injectDuringHorn && now >= 10) {
        primary.rx += "SEND_SMS|+15551234567|Second\nHORN_CHIRP\n";
        injectDuringHorn = false;
    }
    if (!delayedReply.empty() && now >= replyAt) {
        modem.rx += delayedReply;
        delayedReply.clear();
    }
}

void test_on_write(HardwareSerial &serial, char c) {
    if (serial.id != 1) return;
    if (c == 0x1B) { enteringSms = false; atLine.clear(); return; }
    if (enteringSms) {
        if (c == 0x1A) {
            ++submissions;
            enteringSms = false;
            if (modemCase == ModemCase::Reject) modem.rx += "\r\n+CMS ERROR: 500\r\n";
            else if (modemCase != ModemCase::NoResult && modemCase != ModemCase::Slow)
                modem.rx += "\r\n+CMGS: 42\r\n\r\nOK\r\n";
        }
        return;
    }
    atLine += c;
    if (c != '\n') return;
    const std::string commandLine = atLine;
    atLine.clear();
    if (modemCase == ModemCase::Absent) return;
    std::string response = "\r\nOK\r\n";
    if (commandLine == "AT+CPIN?\r\n")
        response = modemCase == ModemCase::NoSim ? "\r\n+CME ERROR: 10\r\n" : "\r\n+CPIN: READY\r\nOK\r\n";
    else if (commandLine == "AT+CREG?\r\n")
        response = modemCase == ModemCase::NoNetwork ? "\r\n+CREG: 0,2\r\nOK\r\n" : "\r\n+CREG: 0,1\r\nOK\r\n";
    else if (commandLine == "AT+CEREG?\r\n")
        response = modemCase == ModemCase::NoNetwork ? "\r\n+CEREG: 0,2\r\nOK\r\n" : "\r\n+CEREG: 0,5\r\nOK\r\n";
    else if (commandLine.find("AT+CMGR=") == 0)
        response = "\r\n+CMGR: 0,,23\r\n" + incomingPdu + "\r\n\r\nOK\r\n";
    else if (commandLine.find("AT+CMGS=") == 0) {
        enteringSms = true;
        response = modemCase == ModemCase::NoPrompt ? "" : "\r\n> ";
    }
    if (injectBusyCommand) {
        primary.rx += "SEND_SMS|+15551234567|Second\n";
        injectBusyCommand = false;
    }
    if (injectHornDuringSms) {
        primary.rx += "HORN_CHIRP\n";
        injectHornDuringSms = false;
    }
    if (injectIncoming) {
        response = "\r\n+CMTI: \"SM\",7\r\n" + response;
        injectIncoming = false;
    }
    if (modemCase == ModemCase::Slow) {
        delayedReply = response;
        replyAt = now + 2900;
    } else modem.rx += response;
}

static void reset(ModemCase scenario = ModemCase::Ready) {
    input = cellular::LineBuffer{};
    busy = frameReceivedWhileBusy = false;
    now = 0;
    injectHornDuringSms = injectDuringHorn = false;
    injectIncoming = false;
    MCP2515::resetResult = MCP2515::bitrateResult = MCP2515::modeResult = MCP2515::ERROR_OK;
    MCP2515::frames.clear(); MCP2515::times.clear(); MCP2515::results.clear();
    modemCase = scenario;
    enteringSms = injectBusyCommand = false;
    submissions = 0;
    atLine.clear(); delayedReply.clear();
    modem.rx.clear(); modem.tx.clear();
    primary.rx.clear(); primary.tx.clear();
    Serial.tx.clear();
    setup();
}

static void request(const std::string &frame = "SEND_SMS|+15551234567|Testing\n") {
    primary.rx += frame;
    do { loop(); } while (primary.available());
}

static void settle_receiver() {
    // Advance fake time through a missing-command timeout and a setup retry.
    for (unsigned i = 0; i < 15000; ++i) {
        loop();
        if (sms_service::receiveReady && !sms_service::receiving_busy() &&
            !sms_service::pendingCount && !modem.available()) return;
    }
    assert(false && "receiver did not settle");
}

static void test_protocol() {
    cellular::Sms sms{};
    char valid[] = "SEND_SMS|+15551234567|Testing";
    assert(!cellular::parse_sms(valid, sms));
    assert(std::string(sms.message) == "Testing");
    for (const char *number : {"", "+", "+0123456789", "123", "1234567890123456", "12;AT+CFUN=0", "+1 5551234567"})
        assert(!cellular::valid_number(number));
    assert(cellular::valid_number("5551234567"));
    assert(cellular::valid_message(std::string(160, 'a').c_str()));
    assert(!cellular::valid_message(std::string(161, 'a').c_str()));
    assert(cellular::valid_message(std::string(80, '^').c_str()));
    assert(!cellular::valid_message(std::string(81, '^').c_str()));
    for (const char *message : {"", "a|b", "line\nAT", "\x1a", "\x1b", "`", "\xc3\xa9"})
        assert(!cellular::valid_message(message));

    cellular::LineBuffer line;
    for (size_t i = 0; i < cellular::MAX_FRAME; ++i) assert(line.push('a') == cellular::FrameEvent::None);
    assert(line.push('\n') == cellular::FrameEvent::Ready);
    for (size_t i = 0; i <= cellular::MAX_FRAME; ++i) line.push('a');
    assert(line.push('\n') == cellular::FrameEvent::Oversized);
    line.push('O'); line.push('K'); line.push('\r');
    assert(line.push('\n') == cellular::FrameEvent::Ready);
    assert(std::string(line.data()) == "OK");
    line.push('\0');
    assert(line.push('\n') == cellular::FrameEvent::Invalid);
}

static void test_secondary() {
    reset();
    request("\n\r\n");
    assert(primary.tx.empty() && modem.tx.empty());
    request("AT+CFUN=0\nSEND_SMS|123|Testing\nSEND_SMS|+15551234567|a|b\n");
    request(std::string(257, 'a') + "\n");
    request(std::string("SEND_SMS|+15551234567|bad") + '\0' + "tail\n");
    assert(modem.tx.empty());
    assert(primary.tx.find("UNKNOWN_COMMAND") != std::string::npos);
    assert(primary.tx.find("BAD_NUMBER") != std::string::npos);
    assert(primary.tx.find("BAD_MESSAGE") != std::string::npos);
    assert(primary.tx.find("FRAME_TOO_LONG") != std::string::npos);
    assert(primary.tx.find("BAD_FRAME") != std::string::npos);
    primary.tx.clear();
    request("SEND_SMS|+15551234567|Tes");
    assert(primary.tx.empty());
    request("ting\r\n");
    const std::string final = SEND_REAL_SMS ? "RESULT|OK\n" : "RESULT|ERROR|DRY_RUN\n";
    assert(primary.tx == "ACCEPTED\n" + final);
    assert(submissions == (SEND_REAL_SMS ? 1u : 0u));
    if (SEND_REAL_SMS) assert(modem.tx.find("Testing\x1a") != std::string::npos);

    reset(); injectBusyCommand = true; request();
    assert(primary.tx == "ACCEPTED\nRESULT|ERROR|BUSY\n" + final);
    assert(submissions <= 1);
    for (const auto scenario : {ModemCase::Absent, ModemCase::NoSim, ModemCase::NoNetwork}) {
        reset(scenario); request();
        assert(primary.tx.find("ACCEPTED\nRESULT|ERROR|") == 0);
        assert(submissions == 0 && !busy && now <= cellular::MODEM_TIMEOUT_MS + 4);
        modemCase = ModemCase::Ready;
        settle_receiver();
        primary.tx.clear(); request(); // Recover after the fault without resetting.
        assert(primary.tx == "ACCEPTED\n" + final);
    }
    if (SEND_REAL_SMS) {
        for (const auto scenario : {ModemCase::NoPrompt, ModemCase::Reject, ModemCase::NoResult, ModemCase::Slow}) {
            reset(scenario); request();
            assert(primary.tx.find("RESULT|OK") == std::string::npos);
            assert(primary.tx.find("RESULT|ERROR|") != std::string::npos);
            assert(!busy && now <= cellular::MODEM_TIMEOUT_MS + 4);
            assert(submissions <= 1); // Never automatically resubmit after timeout.
            if (scenario == ModemCase::NoResult || scenario == ModemCase::Slow) {
                assert(primary.tx.find("SMS_OUTCOME_UNKNOWN") != std::string::npos);
                assert(now >= cellular::MODEM_TIMEOUT_MS);
            }
        }
    }
}

static std::string pdu(const std::string &body, unsigned dcs = 8, unsigned flags = 0) {
    // SMSC omitted; SMS-DELIVER from +15551234567; PID/DCS; seven timestamp bytes.
    char hex[8];
    snprintf(hex, sizeof(hex), "%02X", flags);
    std::string result = "00" + std::string(hex) + "0B915155214365F700";
    snprintf(hex, sizeof(hex), "%02X", dcs);
    result += hex;
    result += "00000000000000";
    snprintf(hex, sizeof(hex), "%02X", unsigned(body.size() / 2));
    return result + hex + body;
}

static std::string gsm_pdu(const std::vector<unsigned> &septets, bool withHeader = false) {
    std::vector<unsigned char> packed(withHeader ? 6 : 0, 0);
    if (withHeader) { packed[0] = 5; packed[2] = 3; packed[3] = 1; packed[4] = 2; packed[5] = 1; }
    const size_t skipped = withHeader ? 7 : 0; // Six UDH octets + one fill bit.
    packed.resize(((skipped + septets.size()) * 7 + 7) / 8);
    for (size_t i = 0; i < septets.size(); ++i) {
        const size_t bit = (skipped + i) * 7;
        packed[bit / 8] |= septets[i] << (bit % 8);
        if (bit % 8 > 1 && bit / 8 + 1 < packed.size())
            packed[bit / 8 + 1] |= septets[i] >> (8 - bit % 8);
    }
    std::string body;
    char hex[3];
    for (auto c : packed) { snprintf(hex, sizeof(hex), "%02X", c); body += hex; }
    auto result = pdu(body, 0, withHeader ? 0x40 : 0);
    snprintf(hex, sizeof(hex), "%02X", unsigned(skipped + septets.size()));
    result.replace(result.size() - body.size() - 2, 2, hex);
    return result;
}

static void test_incoming() {
    received_sms::Message message;
    assert(received_sms::decode(pdu("004F004B000A004500520052004F0052007C0022D83DDE00").c_str(), message));
    assert(std::string(message.sender) == "+15551234567");
    assert(std::string(message.text) == "OK\nERROR|\"\xf0\x9f\x98\x80");
    // Known GSM-7 packed 'hello' (UDL is septets, not octets).
    assert(received_sms::decode(pdu("E8329BFD06", 0).c_str(), message));
    assert(std::string(message.text) == "hello");
    assert(received_sms::decode(gsm_pdu({'A', 27, 64, 27, 101, 10, 'B'}, true).c_str(), message));
    assert(std::string(message.text) == "A|\xe2\x82\xac\nB");
    assert(received_sms::decode(gsm_pdu(std::vector<unsigned>(160, 'x')).c_str(), message));
    assert(std::string(message.text) == std::string(160, 'x'));
    assert(!received_sms::decode(pdu("0041", 4).c_str(), message)); // binary
    assert(!received_sms::decode("00ZZ", message));
    auto incomplete = pdu("0041"); incomplete.pop_back();
    assert(!received_sms::decode(incomplete.c_str(), message));
    // A UCS2 concatenation header is skipped; each part is displayed independently.
    assert(received_sms::decode(pdu("0500030102010041", 8, 0x40).c_str(), message));
    assert(std::string(message.text) == "A");

    reset();
    incomingPdu = pdu("004F004B000A004500520052004F0052007C0022D83DDE00");
    settle_receiver(); // Reception starts without ever sending an SMS.
    assert(submissions == 0 && primary.tx.find("SMS_RX_STATUS|RX_READY\n") != std::string::npos);
    primary.tx.clear(); modem.tx.clear();
    modem.rx = "+CM"; loop();
    modem.rx += "TI: \"SM\",7\r\n+CMTI: \"SM\",7\r\n";
    settle_receiver();
    assert(primary.tx == "RECEIVED_SMS|+15551234567|4F4B0A4552524F527C22F09F9880\n");
    assert(modem.tx.find("AT+CMGR=7\r\n") != std::string::npos);
    assert(modem.tx.find("AT+CMGD=7\r\n") != std::string::npos);
    assert(submissions == 0);

    reset(); injectIncoming = true; request();
    assert(sms_service::pendingCount == 1); // Notification survives an outgoing wait.
    settle_receiver();
    assert(primary.tx.find("RECEIVED_SMS|+15551234567|") != std::string::npos);
    assert(submissions == (SEND_REAL_SMS ? 1u : 0u));

    reset(); settle_receiver();
    incomingPdu = "00ZZ";
    primary.tx.clear(); modem.tx.clear();
    modem.rx = "+CMTI: \"ME\",9\r\n";
    settle_receiver();
    assert(primary.tx == "SMS_RX_STATUS|RX_ERROR\n");
    assert(modem.tx.find("AT+CMGD=") == std::string::npos); // Preserve undecodable SMS.

    reset(); now = MODEM_BOOT_WAIT_MS; loop();
    assert(sms_service::receiving_busy());
    request();
    assert(primary.tx.find("RESULT|ERROR|BUSY\n") != std::string::npos);
    assert(submissions == 0); // A send cannot interleave the receiver's AT commands.

    reset(); settle_receiver(); primary.tx.clear();
    modem.rx = "+CMTI: \"SM\",65536\r\n+CMTI: \"XX\",2\r\n+CMTI: \"SM\",2AT\r\n";
    loop();
    assert(sms_service::pendingCount == 0 && primary.tx.empty());
    modemCase = ModemCase::Absent;
    modem.rx = "+CMTI: \"SM\",12\r\n";
    loop();
    assert(sms_service::receiving_busy());
    now += 5000; loop();
    assert(!sms_service::receiving_busy() && primary.tx == "SMS_RX_STATUS|RX_ERROR\n");
    const auto before = modem.tx;
    modem.rx = "\r\nOK\r\n"; loop(); // Late reply must not start a new read/delete.
    assert(modem.tx == before && !sms_service::receiveReady);
    modemCase = ModemCase::Ready;
    settle_receiver();
    assert(submissions == 0 && primary.tx.find("RX_READY") != std::string::npos);
}

static bool sample(int level, int64_t advance) {
    test_button_level = level;
    buttonUs += advance;
    return boot_button_released();
}

static void test_button() {
    test_button_level = 0; buttonUs = 0; boot_button_init();
    assert(!sample(0, 1000000)); // Held at startup.
    assert(!sample(1, 0)); assert(!sample(1, 50000));
    assert(!sample(0, 10000)); assert(!sample(1, 10000));
    assert(!sample(1, 50000)); // Short bounce is not a press.
    for (int i = 0; i < 2; ++i) {
        assert(!sample(0, 0)); assert(!sample(0, 50000));
        assert(!sample(0, 1000000)); // Held press never triggers repeatedly.
        assert(!sample(1, 0)); assert(!sample(0, 10000));
        assert(!sample(1, 10000)); assert(!sample(1, 49999));
        assert(sample(1, 1)); assert(!sample(1, 50000));
    }
}

static void test_horn() {
    reset();
    assert(MCP2515::frames.empty()); // No startup honk, even in real-SMS mode.
    assert(SPI.sck == 25 && SPI.mosi == 23 && SPI.miso == 22 && SPI.cs == 21);
    assert(MCP2515::cs == 21 && MCP2515::clock == 1000000 && MCP2515::bus == &SPI);
    request("HORN_");
    assert(primary.tx.empty() && MCP2515::frames.empty());
    request("CHIRP\r\n");
    assert(primary.tx == "HORN_ACCEPTED\nHORN_RESULT|OK\n");
    assert(modem.tx.empty() && submissions == 0);
    assert(MCP2515::times == std::vector<uint32_t>({2, 22, 122, 222, 242}));
    for (size_t i = 0; i < 5; ++i) {
        const auto &frame = MCP2515::frames[i];
        assert(frame.can_id == 0x358 && frame.can_dlc == 8);
        assert(frame.data[0] == (i < 4 ? 0x42 : 0x02) && frame.data[1] == 0x02);
        for (size_t j = 2; j < 8; ++j) assert(frame.data[j] == 0);
    }
    assert(!busy);

    reset(); injectDuringHorn = true; request("HORN_CHIRP\n");
    assert(primary.tx == "HORN_ACCEPTED\nRESULT|ERROR|BUSY\nHORN_RESULT|ERROR|BUSY\nHORN_RESULT|OK\n");
    assert(MCP2515::frames.size() == 5 && modem.tx.empty());
    request(); // SMS still works after the chirp.
    assert(primary.tx.find(SEND_REAL_SMS ? "RESULT|OK\n" : "RESULT|ERROR|DRY_RUN\n") != std::string::npos);

    reset(); injectHornDuringSms = true; request();
    assert(primary.tx.find("HORN_RESULT|ERROR|BUSY\n") != std::string::npos);
    assert(MCP2515::frames.empty()); // No delayed honk after SMS completes.

    for (int failure = 0; failure < 3; ++failure) {
        reset();
        if (failure == 0) MCP2515::resetResult = MCP2515::ERROR_FAIL;
        if (failure == 1) MCP2515::bitrateResult = MCP2515::ERROR_FAIL;
        if (failure == 2) MCP2515::modeResult = MCP2515::ERROR_FAIL;
        assert(!can_horn::begin());
        request("HORN_CHIRP\n");
        assert(primary.tx == "HORN_ACCEPTED\nHORN_RESULT|ERROR|CAN_UNAVAILABLE\n");
        assert(MCP2515::frames.empty());
        request();
        assert(primary.tx.find(SEND_REAL_SMS ? "RESULT|OK\n" : "RESULT|ERROR|DRY_RUN\n") != std::string::npos);
    }
    for (size_t failedFrame : {size_t(0), size_t(4)}) {
        reset();
        MCP2515::results.assign(5, MCP2515::ERROR_OK);
        MCP2515::results[failedFrame] = MCP2515::ERROR_FAILTX;
        const auto resetsBefore = MCP2515::resets;
        request("HORN_CHIRP\n");
        assert(MCP2515::frames.size() == 5 && MCP2515::frames.back().data[0] == 0x02);
        assert(MCP2515::times.back() == 240); // OFF is attempted despite ON failure.
        assert(primary.tx.find(failedFrame == 4 ? "CAN_OFF_FAILED" : "CAN_SEND_FAILED") != std::string::npos);
        assert(MCP2515::resets == resetsBefore + 1);
        request("HORN_CHIRP\n");
        assert(MCP2515::frames.size() == 5); // Failed controller stays disabled.
    }
    reset();
    request("HORN_CHIRP|payload\nHORN_CHIRP_EXTRA\n");
    assert(MCP2515::frames.empty() && primary.tx.find("UNKNOWN_COMMAND") != std::string::npos);
}

int main() {
    test_protocol(); test_secondary(); test_button(); test_horn(); test_incoming();
    printf("PASS: protocol, secondary SMS (%s), horn frames/timing/failures/shared BUSY, and BOOT debounce\n", SEND_REAL_SMS ? "real SMS simulation" : "dry run");
}
