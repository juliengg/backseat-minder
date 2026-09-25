#include <cassert>
#include <cstdio>
#include <string>
#include "cellular_protocol.h"
#include "boot_button.h"
#include "../cell/src/main.cpp"

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

uint32_t millis() { return now; }
int64_t esp_timer_get_time() { return buttonUs; }
void delay(uint32_t ms) {
    now += ms;
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
    else if (commandLine.find("AT+CMGS=") == 0) {
        enteringSms = true;
        response = modemCase == ModemCase::NoPrompt ? "" : "\r\n> ";
    }
    if (injectBusyCommand) {
        primary.rx += "SEND_SMS|+15551234567|Second\n";
        injectBusyCommand = false;
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

int main() {
    test_protocol(); test_secondary(); test_button();
    printf("PASS: protocol, secondary modem service (%s), and BOOT debounce\n", SEND_REAL_SMS ? "real SMS simulation" : "dry run");
}
