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
    if (requests) vQueueDelete(requests);
    requests = nullptr;
    assert(cellular_link_init() == ESP_OK);
}

static void run() {
    cellular::Sms sms{};
    char number[] = "+15551234567";
    char message[] = "Testing";
    assert(cellular_link_send_sms(number, message) == ESP_OK);
    number[1] = '9'; message[0] = 'X'; // Queue must own copies of the arguments.
    assert(nowUs == 0 && tx.empty()); // Caller never waits for UART or modem.
    assert(cellular_link_send_sms("+15551234567", "Duplicate") == ESP_ERR_INVALID_STATE);
    assert(logs.find("BUSY") != std::string::npos);
    assert(xQueueReceive(requests, &sms, 0) == pdTRUE);
    transact(sms);
    busy.store(false); // Same completion action as the worker task.
    assert(tx == "SEND_SMS|+15551234567|Testing\n" && writes == 1);
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
    responses = {{20000, std::string(257, 'x') + "\n"}, {100000, "ACCEP"},
                 {120000, "TED\r\n"}, {200000, "RESULT|OK\n"}};
    run();
    assert(logs.find("Discarded invalid") != std::string::npos);
    assert(logs.find("Secondary: ACCEPTED") != std::string::npos);
    assert(logs.find("Secondary: RESULT|OK") != std::string::npos);
    vQueueDelete(requests);
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
    puts("PASS: primary async queue, BUSY, UART responses and timeout/no-retry policy");
}
