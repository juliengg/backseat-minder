#include <cassert>
#include <cstdio>
#include <cstring>
#include "beacon_tracker.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "freertos/task.h"

static unsigned telemetry_starts=0,telemetry_stops=0;
bool ble_telemetry_start(){++telemetry_starts;return true;}
bool ble_telemetry_stop(){++telemetry_stops;return true;}
void ble_telemetry_gap_event(esp_gap_ble_cb_event_t,esp_ble_gap_cb_param_t*){}
static TickType_t ticks = 0;
static esp_gap_ble_cb_t callback = nullptr;
static auto controller = ESP_BT_CONTROLLER_STATUS_IDLE;
static auto host = ESP_BLUEDROID_STATUS_UNINITIALIZED;
static int fail_step = 0, step = 0, scan_calls = 0;
static bool scan_fails = false;
void test_log(const char *, ...) {}
TickType_t xTaskGetTickCount() { return ticks; }
static esp_err_t result() { return ++step == fail_step ? ESP_FAIL : ESP_OK; }
esp_bt_controller_status_t esp_bt_controller_get_status() { return controller; }
esp_bluedroid_status_t esp_bluedroid_get_status() { return host; }
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *) {
    const auto err = result(); if (!err) controller = ESP_BT_CONTROLLER_STATUS_INITED; return err;
}
esp_err_t esp_bt_controller_enable(int mode) {
    assert(mode == ESP_BT_MODE_BLE);
    const auto err = result(); if (!err) controller = ESP_BT_CONTROLLER_STATUS_ENABLED; return err;
}
esp_err_t esp_bt_controller_disable() { controller = ESP_BT_CONTROLLER_STATUS_INITED; return ESP_OK; }
esp_err_t esp_bt_controller_deinit() { controller = ESP_BT_CONTROLLER_STATUS_IDLE; return ESP_OK; }
esp_err_t esp_bluedroid_init() {
    const auto err = result(); if (!err) host = ESP_BLUEDROID_STATUS_INITIALIZED; return err;
}
esp_err_t esp_bluedroid_enable() {
    const auto err = result(); if (!err) host = ESP_BLUEDROID_STATUS_ENABLED; return err;
}
esp_err_t esp_bluedroid_disable() {
    // Emulate a delayed stop event during deliberate shutdown: no restart.
    if (callback) { esp_ble_gap_cb_param_t param{}; callback(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &param); }
    host = ESP_BLUEDROID_STATUS_INITIALIZED; return ESP_OK;
}
esp_err_t esp_bluedroid_deinit() { host = ESP_BLUEDROID_STATUS_UNINITIALIZED; return ESP_OK; }
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t cb) { callback = cb; return result(); }
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *params) {
    assert(params->scan_type == BLE_SCAN_TYPE_PASSIVE);
    assert(params->scan_duplicate == BLE_SCAN_DUPLICATE_DISABLE);
    assert(params->scan_interval == 0x50 && params->scan_window == 0x30);
    return result();
}
esp_err_t esp_ble_gap_start_scanning(uint32_t duration) {
    assert(duration == 0); ++scan_calls; return scan_fails ? ESP_FAIL : ESP_OK;
}
static void event(esp_gap_ble_cb_event_t kind, int status = ESP_BT_STATUS_SUCCESS) {
    esp_ble_gap_cb_param_t param{};
    param.scan_param_cmpl.status = param.scan_start_cmpl.status = status;
    callback(kind, &param);
}
static void start(bool previous = false) {
    ticks = 0; fail_step = step = 0; scan_fails = false;
    assert(beacon_tracker_init(previous));
    assert(!beacon_tracker_status().available && !beacon_tracker_driver_near());
    event(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT);
    event(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT);
    assert(beacon_tracker_status().available);
}
static void sample(int rssi, bool correct_mac = true) {
    esp_ble_gap_cb_param_t param{};
    const esp_bd_addr_t mac = {0x78, 0x9f, 0x38, 0x0f, 0xf4, 0x43};
    memcpy(param.scan_rst.bda, mac, sizeof(mac));
    if (!correct_mac) param.scan_rst.bda[5] ^= 1;
    param.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
    param.scan_rst.rssi = rssi;
    callback(ESP_GAP_BLE_SCAN_RESULT_EVT, &param);
}
static void samples(int rssi, int n = 6) { for (int i = 0; i < n; ++i) sample(rssi); }

int main() {
    start();
    for (int i = 0; i < 6; ++i) sample(-40, false);
    assert(!beacon_tracker_driver_near() && !beacon_tracker_status().rssi_valid);
    sample(-74); sample(-76);
    assert(!beacon_tracker_driver_near()); // No optimistic boot decision.
    assert(beacon_tracker_status().checking);
    sample(-75);
    assert(beacon_tracker_driver_near());
    assert(!beacon_tracker_status().checking);
    assert(beacon_tracker_rssi() == -75); // Partial window uses its actual count.
    samples(-100, 1);
    assert(beacon_tracker_rssi() == -81); // Average includes the low reading.
    assert(!beacon_tracker_driver_near());
    samples(-75);
    assert(beacon_tracker_driver_near());
    sample(-100);
    assert(beacon_tracker_rssi() == -79);
    assert(beacon_tracker_driver_near()); // One noisy sample stays in the dead band.
    sample(-100);
    assert(beacon_tracker_rssi() == -83);
    assert(!beacon_tracker_driver_near());
    samples(-75);
    assert(beacon_tracker_rssi() == -75); // Old readings roll out of the window.
    assert(beacon_tracker_driver_near());
    samples(-80);
    assert(beacon_tracker_driver_near()); // Strict AWAY boundary is below -80.
    samples(-81);
    assert(!beacon_tracker_driver_near());
    samples(-78);
    assert(!beacon_tracker_driver_near()); // Dead band preserves AWAY.
    samples(-75);
    assert(beacon_tracker_driver_near());
    ticks = 5000;
    assert(beacon_tracker_driver_near());
    ticks = 5001;
    sample(-40, false);
    assert(!beacon_tracker_driver_near() && beacon_tracker_rssi() == 0);
    sample(-40);
    assert(!beacon_tracker_driver_near()); // Stale window discarded on return.
    assert(beacon_tracker_rssi() == -40);
    sample(-40); sample(-40);
    assert(beacon_tracker_driver_near());

    start(true);
    assert(!beacon_tracker_driver_near()); // RTC hysteresis alone cannot sleep.
    assert(beacon_tracker_status().driver_present && beacon_tracker_status().checking);
    sample(-78); sample(-78);
    assert(beacon_tracker_status().driver_present && beacon_tracker_status().checking);
    assert(!beacon_tracker_driver_near()); // Retained display state is not sleep permission.
    sample(-78);
    assert(beacon_tracker_driver_near()); // Preserve NEAR across timer probes.
    start(true);
    ticks = 1000; sample(-78);
    ticks = 4000; sample(-78);
    ticks = 6500;
    assert(beacon_tracker_status().driver_present && beacon_tracker_status().checking);
    ticks = 7000; sample(-78);
    assert(beacon_tracker_driver_near()); // Slow fob can finish a BLE-only probe.
    start(false); samples(-78, 3);
    assert(!beacon_tracker_driver_near());
    start(true); ticks = 5001;
    assert(!beacon_tracker_driver_near());
    assert(!beacon_tracker_status().checking && !beacon_tracker_status().driver_present);
    samples(-78, 3);
    assert(!beacon_tracker_driver_near());
    start(true); ticks = 5001; // The timeout also holds without a status poll.
    samples(-78, 3);
    assert(!beacon_tracker_driver_near());

    // Unsigned elapsed time remains correct across RTOS tick wrap.
    start(); ticks = UINT32_MAX - 100; samples(-60);
    ticks = 100; assert(beacon_tracker_driver_near());
    ticks = 5001; assert(!beacon_tracker_driver_near());
    start(); samples(-60);
    event(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, 1);
    assert(!beacon_tracker_status().available && !beacon_tracker_driver_near());
    start();
    event(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, 1);
    assert(!beacon_tracker_status().available && !beacon_tracker_driver_near());
    start(); scan_fails = true;
    event(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT);
    assert(!beacon_tracker_status().available && !beacon_tracker_driver_near());
    const int before = scan_calls;
    assert(beacon_tracker_stop());
    assert(scan_calls == before); // Deliberate shutdown never restarts scans.
    for (int failure = 1; failure <= 6; ++failure) {
        step = 0; fail_step = failure;
        assert(!beacon_tracker_init());
        assert(!beacon_tracker_status().available && !beacon_tracker_driver_near());
        assert(controller == ESP_BT_CONTROLLER_STATUS_IDLE);
        assert(host == ESP_BLUEDROID_STATUS_UNINITIALIZED);
    }
    puts("PASS: beacon MAC, moving average, thresholds, timeout, fresh return, timer hysteresis, wrap, BLE failures and cleanup");
}
