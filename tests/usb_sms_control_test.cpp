#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../main/usb_telemetry.cpp"

static uint8_t wireless_pending=0;
void ble_telemetry_init(){}
bool ble_telemetry_connected(){return false;}
uint8_t ble_telemetry_take_test_requests(){uint8_t v=wireless_pending;wireless_pending=0;return v;}
bool ble_telemetry_send_packet(const char[4],const uint8_t*,size_t){return false;}
static std::string rx, tx;
static bool link_busy = false;
static unsigned reads = 0;
static int64_t now_us = 9000000;
int64_t esp_timer_get_time() { return now_us; }
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
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t) { return ESP_OK; }

int main() {
    using namespace telemetry_test;
    assert(!usb_telemetry_take_test_requests() && reads == 0);
    usb_telemetry_init();
    wireless_pending = SMS | HORN;
    assert(usb_telemetry_take_test_requests() == (SMS | HORN));
    assert(!usb_telemetry_take_test_requests());
    rx = "SEND_";
    assert(!usb_telemetry_take_test_requests());
    rx = "SMS\n";
    assert(usb_telemetry_take_test_requests() == SMS);
    assert(!usb_telemetry_take_test_requests()); // Consumed once.
    rx = "\nSEND_SMS\r\nSEND_SMS\nTEST_CALL\nTEST_HORN\r\n";
    assert(usb_telemetry_take_test_requests() == (SMS | CALL | HORN));
    assert(s_tests.sms_requests == 3); // Coalesce a burst.
    for (const auto &invalid : {std::string("SEND_SMS|+15551234567|Injected\n"),
                               std::string("TEST_CALL|933\nTEST_HORN_REPEAT\n"),
                               std::string("SEND_SMS\0\n", 10),
                               std::string("SEND_SMS\rX\n"),
                               std::string(300, 'X') + "SEND_SMS\n"}) {
        rx = invalid;
        while (!rx.empty()) assert(!usb_telemetry_take_test_requests());
    }
    rx = "SEND_SMS\n";
    assert(usb_telemetry_take_test_requests() == SMS && s_tests.sms_requests == 4);
    rx = std::string(500, 'X');
    const auto before = reads;
    assert(!usb_telemetry_take_test_requests() && reads == before + 1 && rx.size() == 372);
    rx = "\nVIEWER_"; // Leading delimiter recovers from a prior partial/oversized line.
    assert(!usb_telemetry_take_test_requests());
    rx = "ALIVE\n";
    assert(!usb_telemetry_take_test_requests());
    usb_telemetry_send(70.0f, 50.0f, true, false, false, false, false,
                       {true, false, true, -81});
    assert(tx.find("\"driver_present\":false") != std::string::npos);
    assert(tx.find("\"beacon_rssi_dbm\":-81") != std::string::npos);
    usb_telemetry_send_driver_status({true, true, true, -65}, TelemetryPowerState::SleepPending);
    assert(tx.find("BSMP") != std::string::npos);
    assert(tx.find("\"driver_present\":true") != std::string::npos);
    assert(tx.find("\"deep_sleep_pending\":true") != std::string::npos);
    assert(tx.find("\"beacon_recheck_ms\":0") != std::string::npos);
    assert(tx.find("\"power_state\":\"sleep_pending\"") != std::string::npos);
    assert(tx.find("telemetry_active") == std::string::npos);
    tx.clear();
    usb_telemetry_send_driver_status({true, true, false, 0, true}, TelemetryPowerState::BeaconProbe);
    assert(tx.find("\"driver_present\":true") != std::string::npos);
    assert(tx.find("\"driver_checking\":true") != std::string::npos);
    assert(tx.find("\"power_state\":\"beacon_probe\"") != std::string::npos);
    usb_telemetry_set_alert("active", 123, false, 0, 0);
    usb_telemetry_send(70.0f, 50.0f, true, false, false, false, false,
                       {true, true, true, -65}, true);
    usb_telemetry_flush();
    assert(tx.find("\"power_state\":\"active\"") != std::string::npos);
    const auto marker = tx.find("BSMC");
    assert(marker != std::string::npos);
    const auto *header = reinterpret_cast<const unsigned char *>(tx.data() + marker + 4);
    const size_t size = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    const auto json = tx.substr(marker + 8, size);
    assert(json.size() == size && json.back() == '}');
    assert(json.find("\"test_control\":true") != std::string::npos);
    assert(json.find("\"sms_control\":true") != std::string::npos);
    assert(json.find("\"sms_busy\":true") != std::string::npos);
    assert(json.find("\"alert_active\":true") != std::string::npos);
    assert(json.find("\"sms_requests\":4") != std::string::npos);
    assert(json.find("\"call_requests\":1") != std::string::npos);
    assert(json.find("\"horn_requests\":2") != std::string::npos);
    delete s_write_mutex;
    puts("PASS: bounded test commands over USB and BLE, recovery, coalescing and receipt telemetry");
}
