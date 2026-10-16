#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../main/usb_telemetry.cpp"

static std::string rx, tx;
static bool link_busy = false;
static unsigned reads = 0;
int64_t esp_timer_get_time() { return 9000000; }
void test_log(const char *, ...) {}
bool cellular_link_busy() { return link_busy; }
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *config) {
    assert(config->rx_buffer_size > 0);
    return ESP_OK;
}
int usb_serial_jtag_read_bytes(void *buffer, uint32_t capacity, TickType_t wait) {
    assert(wait == 0); // USB input cannot block the monitoring loop.
    ++reads;
    const auto count = std::min(rx.size(), static_cast<size_t>(capacity));
    memcpy(buffer, rx.data(), count);
    rx.erase(0, count);
    return static_cast<int>(count);
}
int usb_serial_jtag_write_bytes(const void *buffer, size_t size, TickType_t) {
    tx.append(static_cast<const char *>(buffer), size);
    return static_cast<int>(size);
}

int main() {
    assert(!usb_telemetry_sms_requested() && reads == 0);
    usb_telemetry_init();
    assert(!usb_telemetry_sms_requested());
    rx = "SEND_";
    assert(!usb_telemetry_sms_requested());
    rx = "SMS\n";
    assert(usb_telemetry_sms_requested());
    assert(!usb_telemetry_sms_requested()); // Consumed once.
    rx = "\nSEND_SMS\r\nSEND_SMS\n";
    assert(usb_telemetry_sms_requested() && s_sms_requests == 2); // Coalesce a burst.
    for (const auto &invalid : {std::string("SEND_SMS|+15551234567|Injected\n"),
                               std::string("SEND_SMS\0\n", 10),
                               std::string("SEND_SMS\rX\n"),
                               std::string(300, 'X') + "SEND_SMS\n"}) {
        rx = invalid;
        while (!rx.empty()) assert(!usb_telemetry_sms_requested());
    }
    rx = "SEND_SMS\n";
    assert(usb_telemetry_sms_requested() && s_sms_requests == 3);
    rx = std::string(500, 'X');
    const auto before = reads;
    assert(!usb_telemetry_sms_requested() && reads == before + 1 && rx.size() == 372);
    rx.clear();
    link_busy = true;
    usb_telemetry_send(70.0f, 50.0f, true, false, false, false, false);
    const auto marker = tx.find("BSMC");
    assert(marker != std::string::npos);
    const auto *header = reinterpret_cast<const unsigned char *>(tx.data() + marker + 4);
    const size_t size = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    const auto json = tx.substr(marker + 8, size);
    assert(json.size() == size && json.back() == '}');
    assert(json.find("\"sms_control\":true") != std::string::npos);
    assert(json.find("\"sms_busy\":true") != std::string::npos);
    assert(json.find("\"sms_requests\":3") != std::string::npos);
    delete s_write_mutex;
    puts("PASS: bounded nonblocking USB commands, recovery, coalescing and receipt telemetry");
}
