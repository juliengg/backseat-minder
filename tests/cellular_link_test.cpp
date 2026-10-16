#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "../main/cellular_link.cpp"

static int64_t nowUs = 0;
static std::string rx, tx, logs;
static std::vector<std::pair<int64_t, std::string>> responses;
static size_t nextResponse = 0;
static unsigned writes = 0;

int64_t esp_timer_get_time() { return nowUs; }
void test_log(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logs += buffer; logs += '\n';
}
int uart_flush_input(int) { rx.clear(); return ESP_OK; }
int uart_write_bytes(int, const void *data, size_t length) {
    tx.append(static_cast<const char *>(data), length);
    ++writes;
    return static_cast<int>(length);
}
int uart_read_bytes(int, void *data, size_t capacity, TickType_t wait) {
    nowUs += wait * 1000;
    while (nextResponse < responses.size() && responses[nextResponse].first <= nowUs)
        rx += responses[nextResponse++].second;
    const size_t count = std::min(capacity, rx.size());
    memcpy(data, rx.data(), count); rx.erase(0, count);
    return static_cast<int>(count);
}

static void reset() {
    nowUs = 0; writes = 0; nextResponse = 0;
    rx.clear(); tx.clear(); logs.clear(); responses.clear();
    busy.store(false);
    incoming = cellular::ReceiveLineBuffer{};
    if (requests) vQueueDelete(requests);
    requests = nullptr;
    assert(cellular_link_init() == ESP_OK);
}

static void run() {
    cellular::Request request;
    char number[] = "+15551234567";
    char message[] = "Testing";
    assert(cellular_link_send_sms(number, message) == ESP_OK);
    number[1] = '9'; message[0] = 'X'; // Queue must own copies of the arguments.
    assert(nowUs == 0 && tx.empty()); // Caller never waits for UART or modem.
    assert(cellular_link_send_sms("+15551234567", "Duplicate") == ESP_ERR_INVALID_STATE);
    assert(logs.find("BUSY") != std::string::npos);
    assert(xQueueReceive(requests, &request, 0) == pdTRUE);
    transact(request);
    busy.store(false); // Same completion action as the worker task.
    assert(tx == "SEND_SMS|+15551234567|Testing\n" && writes == 1);
}

static void run_horn() {
    cellular::Request request;
    assert(cellular_link_send_horn() == ESP_OK);
    assert(cellular_link_busy() && tx.empty() && nowUs == 0);
    assert(cellular_link_send_sms("+15551234567", "Testing") == ESP_ERR_INVALID_STATE);
    assert(cellular_link_send_horn() == ESP_ERR_INVALID_STATE);
    assert(xQueueReceive(requests, &request, 0) == pdTRUE);
    assert(request.action == cellular::Action::Horn);
    transact(request);
    busy.store(false);
    assert(tx == "HORN_CHIRP\n" && writes == 1 && !cellular_link_busy());
}

int main() {
    reset();
    assert(cellular_link_send_sms("+15551234567\nAT", "Testing") == ESP_ERR_INVALID_ARG);
    assert(!busy.load() && !requests->full);
    responses = {{20000, "ACCEPTED\nRESULT|OK\n"}};
    run();
    assert(nowUs == 20000 && logs.find("Secondary: RESULT|OK") != std::string::npos);
    char diagnostics[1024];
    assert(cellular_diagnostics_json(diagnostics, sizeof(diagnostics)) > 0);
    assert(std::string(diagnostics).find("\"status\":\"ACCEPTED\"") != std::string::npos);
    assert(std::string(diagnostics).find("\"status\":\"OK\"") != std::string::npos);
    assert(std::string(diagnostics).find("15551234567") == std::string::npos);
    assert(cellular_diagnostics_json(diagnostics, 10) == 0);

    reset();
    responses = {{20000, "ACCEPTED\nRESULT|ERROR|BUSY\n"}, {200000, "RESULT|OK\n"}};
    run();
    assert(nowUs == 200000 && logs.find("Secondary: RESULT|OK") != std::string::npos);

    reset();
    responses = {{20000, "RESULT|ERROR|SIM_NOT_READY\n"}};
    run();
    assert(logs.find("without acknowledgement") != std::string::npos);

    reset();
    responses = {{5000000, "RESULT|OK\n"}}; // Lost ACK, real completion later.
    run();
    assert(logs.find("ACCEPTED timeout") != std::string::npos);
    assert(nowUs == 5000000 && writes == 1);

    reset(); run(); // Secondary disconnected: bounded wait, never retransmit.
    assert(logs.find("ACCEPTED timeout") != std::string::npos);
    assert(logs.find("RESULT timeout") != std::string::npos);
    assert(nowUs == cellular::RESULT_TIMEOUT_MS * 1000LL);

    reset();
    responses = {{20000, "ACCEPTED\n"}};
    run();
    assert(logs.find("ACCEPTED timeout") == std::string::npos);
    assert(logs.find("RESULT timeout") != std::string::npos);

    reset();
    responses = {{20000, std::string(cellular::MAX_RECEIVED_FRAME + 1, 'x') + "\n"}, {100000, "ACCEP"},
                 {120000, "TED\r\n"}, {200000, "RESULT|OK\n"}};
    run();
    assert(logs.find("Discarded invalid") != std::string::npos);
    assert(logs.find("Secondary: ACCEPTED") != std::string::npos);
    assert(logs.find("Secondary: RESULT|OK") != std::string::npos);

    reset();
    responses = {{20000, "HORN_ACCEPTED\n"}, {260000, "HORN_RESULT|OK\n"}};
    run_horn();
    assert(nowUs == 260000 && logs.find("Secondary: HORN_RESULT|OK") != std::string::npos);
    assert(cellular_diagnostics_json(diagnostics, sizeof(diagnostics)) > 0);
    assert(std::string(diagnostics).find("HORN_OK") != std::string::npos);

    reset();
    responses = {{20000, "HORN_ACCEPTED\nRESULT|OK\nHORN_RESULT|ERROR|BUSY\n"},
                 {260000, "HORN_RESULT|ERROR|CAN_OFF_FAILED\n"}};
    run_horn();
    assert(nowUs == 260000 && logs.find("CAN_OFF_FAILED") != std::string::npos);

    reset();
    responses = {{20000, "RESULT|OK\n"}}; // SMS replies cannot complete a horn.
    run_horn();
    assert(nowUs == cellular::HORN_RESULT_TIMEOUT_MS * 1000LL);
    assert(logs.find("ACCEPTED timeout") != std::string::npos);
    assert(logs.find("RESULT timeout") != std::string::npos);

    reset();
    responses = {{20000, "HORN_RESULT|OK\nACCEPTED\n"}, {100000, "RESULT|OK\n"}};
    run(); // Horn replies cannot complete an SMS.
    assert(nowUs == 100000);

    reset();
    responses = {{20000, "HORN_RESULT|ERROR|CAN_UNAVAILABLE\n"}};
    run_horn();
    assert(nowUs == 20000 && logs.find("without acknowledgement") != std::string::npos);

    reset();
    assert(cellular_link_send_sms("+15551234567", "Testing") == ESP_OK);
    assert(cellular_link_send_horn() == ESP_ERR_INVALID_STATE);
    cellular::Request queued;
    assert(xQueueReceive(requests, &queued, 0) == pdTRUE);
    assert(queued.action == cellular::Action::Sms && !requests->full);
    // No horn was queued behind the active SMS.
    vQueueDelete(requests);
    requests = nullptr;
    for (int i = 0; i < 10; ++i) {
        char status[16];
        snprintf(status, sizeof(status), "EVENT_%d", i);
        cellular_diagnostics_record(status);
    }
    assert(cellular_diagnostics_json(diagnostics, sizeof(diagnostics)) > 0);
    assert(std::string(diagnostics).find("EVENT_1") == std::string::npos);
    assert(std::string(diagnostics).find("EVENT_2") != std::string::npos);
    assert(std::string(diagnostics).find("EVENT_9") != std::string::npos);
    cellular_diagnostics_record("bad\"json");
    assert(cellular_diagnostics_json(diagnostics, sizeof(diagnostics)) > 0);
    assert(std::string(diagnostics).find("UNKNOWN_ERROR") != std::string::npos);

    reset();
    rx = "RECEIVED_SMS|+15551234567|4F4B0A4552524F527C22F09F9880\n";
    poll_incoming(0); // Primary must listen even without a pending request.
    char received_json[cellular::DIAGNOSTICS_JSON_CAPACITY];
    assert(cellular_diagnostics_json(received_json, sizeof(received_json)) > 0);
    assert(std::string(received_json).find("\"sender\":\"+15551234567\"") != std::string::npos);
    assert(std::string(received_json).find("OK\\nERROR|\\\"\xf0\x9f\x98\x80") != std::string::npos);

    reset();
    responses = {{20000, "ACCEPTED\nRESULT|OK\nRECEIVED_SMS|+15551234567|4869\n"}};
    run();
    assert(cellular_diagnostics_json(received_json, sizeof(received_json)) > 0);
    assert(std::string(received_json).find("\"message\":\"Hi\"") != std::string::npos);

    reset();
    rx = "RECEIVED_SMS|+15551234567|4";
    poll_incoming(0);
    rx = "1\nSMS_RX_STATUS|RX_READY\n";
    poll_incoming(0);
    assert(cellular_diagnostics_json(received_json, sizeof(received_json)) > 0);
    assert(std::string(received_json).find("\"message\":\"A\"") != std::string::npos);
    assert(std::string(received_json).find("RX_READY") != std::string::npos);
    reset();
    responses = {{20000, "ACCEPTED\nRECEIVED_SMS|+15551234567|4869\n"},
                 {200000, "RESULT|OK\n"}};
    run(); // Receiving a text cannot finish the active send transaction.
    assert(nowUs == 200000 && logs.find("Unexpected secondary response") == std::string::npos);
    // Eight maximum-size escaped SMS entries still fit the USB packet budget.
    for (int i = 0; i < 8; ++i)
        cellular_diagnostics_received("+15551234567", std::string(480, '"').c_str());
    assert(cellular_diagnostics_json(received_json, sizeof(received_json)) > 0);
    puts("PASS: primary async queue, BUSY, UART responses and timeout/no-retry policy");
}
