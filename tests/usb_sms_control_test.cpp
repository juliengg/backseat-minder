#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "../main/usb_telemetry.cpp"

static bool wireless_pending=false;
void ble_telemetry_init(){}
bool ble_telemetry_connected(){return false;}
bool ble_telemetry_take_sms_request(){bool v=wireless_pending;wireless_pending=false;return v;}
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
    assert(!usb_telemetry_sms_requested() && reads == 0);
    usb_telemetry_init();
    wireless_pending=true;
    assert(usb_telemetry_sms_requested());
    assert(!usb_telemetry_sms_requested());
    s_sms_requests=0;
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
    assert(!usb_telemetry_sms_pending());
    rx = "\nVIEWER_"; // Leading delimiter recovers from a prior partial/oversized line.
    usb_telemetry_poll_commands();
    assert(!usb_telemetry_sms_pending());
    rx = "ALIVE\n";
    assert(!usb_telemetry_sms_requested()); // A heartbeat never triggers SMS.
    assert(!usb_telemetry_sms_pending()); // Legacy viewer heartbeats cannot inhibit sleep.
    rx = "\nVIEWER_ALIVE\nSEND_SMS\n";
    usb_telemetry_poll_commands(); // Simulate reading commands during a wake probe.
    assert(usb_telemetry_sms_pending() && s_sms_requests == 4);
    assert(usb_telemetry_sms_requested()); // The probe preserves the command for the main loop.
    assert(!usb_telemetry_sms_requested() && !usb_telemetry_sms_pending());
    rx = "VIEWER_ALIVE|invalid\n";
    usb_telemetry_poll_commands();
    assert(!usb_telemetry_sms_pending());
    link_busy = true;
    usb_telemetry_send(70.0f, 50.0f, true, false, false, false, false,
                       {true, false, true, -81});
    assert(tx.find("\"driver_present\":false") != std::string::npos);
    assert(tx.find("\"beacon_rssi_dbm\":-81") != std::string::npos);
    usb_telemetry_send_driver_status({true, true, true, -65}, TelemetryPowerState::SleepPending);
    assert(tx.find("BSMP") != std::string::npos);
    assert(tx.find("\"driver_present\":true") != std::string::npos);
    assert(tx.find("\"deep_sleep_pending\":true") != std::string::npos);
    assert(tx.find("\"beacon_recheck_ms\":8000") != std::string::npos);
    assert(tx.find("\"power_state\":\"sleep_pending\"") != std::string::npos);
    assert(tx.find("telemetry_active") == std::string::npos);
    tx.clear();
    usb_telemetry_send_driver_status({true, true, false, 0, true}, TelemetryPowerState::BeaconProbe);
    assert(tx.find("\"driver_present\":true") != std::string::npos);
    assert(tx.find("\"driver_checking\":true") != std::string::npos);
    assert(tx.find("\"power_state\":\"beacon_probe\"") != std::string::npos);
    usb_telemetry_send(70.0f, 50.0f, true, false, false, false, false,
                       {true, true, true, -65});
    usb_telemetry_flush();
    assert(tx.find("\"power_state\":\"active\"") != std::string::npos);
    const auto marker = tx.find("BSMC");
    assert(marker != std::string::npos);
    const auto *header = reinterpret_cast<const unsigned char *>(tx.data() + marker + 4);
    const size_t size = (header[0] << 24) | (header[1] << 16) | (header[2] << 8) | header[3];
    const auto json = tx.substr(marker + 8, size);
    assert(json.size() == size && json.back() == '}');
    assert(json.find("\"sms_control\":true") != std::string::npos);
    assert(json.find("\"sms_busy\":true") != std::string::npos);
    assert(json.find("\"sms_requests\":4") != std::string::npos);
    delete s_write_mutex;
    puts("PASS: bounded nonblocking USB commands, recovery, coalescing and receipt telemetry");
}
