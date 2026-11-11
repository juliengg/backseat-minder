#include "beacon_tracker.h"
#include "ble_telemetry.h"
#include "ble_telemetry_gap.h"

#include <cstring>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
const char *TAG = "beacon_tracker";
// The supplied fob must advertise this fixed address (no address rotation).
const esp_bd_addr_t TARGET_MAC = {0x78, 0x9f, 0x38, 0x0f, 0xf4, 0x43};
constexpr int RSSI_THRESHOLD = -75;
constexpr int RSSI_HYSTERESIS = 5;
constexpr int WIN_SIZE = 6;
constexpr int MIN_SAMPLES = WIN_SIZE / 2;
constexpr uint32_t BEACON_TIMEOUT_MS = 5000;

portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
int s_rssi[WIN_SIZE] = {};
int s_count = 0;
int s_index = 0;
TickType_t s_last_seen = 0;
TickType_t s_init_tick = 0;
bool s_near = false;
bool s_checking = true;
bool s_running = false;
bool s_stopping = true;

// Caller holds s_lock and s_count is nonzero. Average the latest WIN_SIZE
// advertisements (or all available samples while the window fills).
int average_rssi()
{
    int sum = 0;
    for (int i = 0; i < s_count; ++i) sum += s_rssi[i];
    return sum / s_count;
}

void mark_unavailable()
{
    portENTER_CRITICAL(&s_lock);
    s_running = false;
    s_near = false;
    s_checking = false;
    s_count = s_index = 0;
    portEXIT_CRITICAL(&s_lock);
}

void start_scan()
{
    const esp_err_t err = esp_ble_gap_start_scanning(0);
    if (err != ESP_OK) {
        mark_unavailable();
        ESP_LOGE(TAG, "Start BLE scan failed: %s; monitoring stays active", esp_err_to_name(err));
    }
}

void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    portENTER_CRITICAL(&s_lock);
    const bool stopping = s_stopping;
    portEXIT_CRITICAL(&s_lock);
    ble_telemetry_gap_event(event,param);
    if (stopping) return;

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) start_scan();
        else {
            mark_unavailable();
            ESP_LOGE(TAG, "BLE scan parameters rejected; monitoring stays active");
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            portENTER_CRITICAL(&s_lock);
            s_running = true;
            portEXIT_CRITICAL(&s_lock);
        } else {
            mark_unavailable();
            ESP_LOGE(TAG, "BLE scan failed asynchronously; monitoring stays active");
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT &&
            memcmp(param->scan_rst.bda, TARGET_MAC, sizeof(TARGET_MAC)) == 0) {
            const TickType_t now = xTaskGetTickCount();
            portENTER_CRITICAL(&s_lock);
            if (!s_count) s_checking = true;
            if (!s_count && now - s_init_tick > pdMS_TO_TICKS(BEACON_TIMEOUT_MS))
                s_near = false;
            if (s_count && now - s_last_seen > pdMS_TO_TICKS(BEACON_TIMEOUT_MS)) {
                // Never let RSSI retained from before a timeout qualify a return.
                s_count = s_index = 0;
                s_near = false;
                s_checking = true;
            }
            s_rssi[s_index] = param->scan_rst.rssi;
            s_index = (s_index + 1) % WIN_SIZE;
            if (s_count < WIN_SIZE) ++s_count;
            s_last_seen = now;
            portEXIT_CRITICAL(&s_lock);
        } else if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            mark_unavailable();
            start_scan();
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        mark_unavailable();
        start_scan(); // Unexpected stop only; deliberate shutdown sets s_stopping.
        break;
    default:
        break;
    }
}

bool check(esp_err_t err, const char *operation)
{
    if (err == ESP_OK) return true;
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(err));
    return false;
}
} // namespace

bool beacon_tracker_stop()
{
    portENTER_CRITICAL(&s_lock);
    s_stopping = true;
    portEXIT_CRITICAL(&s_lock);
    mark_unavailable();
    if(!ble_telemetry_stop()) return false;
    bool ok = true;
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
        ok = check(esp_bluedroid_disable(), "Disable Bluedroid") && ok;
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        ok = check(esp_bluedroid_deinit(), "Deinit Bluedroid") && ok;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
        ok = check(esp_bt_controller_disable(), "Disable BLE controller") && ok;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        ok = check(esp_bt_controller_deinit(), "Deinit BLE controller") && ok;
    return ok;
}

bool beacon_tracker_init(bool previously_near)
{
    if (!beacon_tracker_stop()) return false;
    portENTER_CRITICAL(&s_lock);
    s_near = previously_near;
    s_checking = true;
    s_init_tick = xTaskGetTickCount();
    s_stopping = false;
    portEXIT_CRITICAL(&s_lock);

    // S3 supports BLE only. Do not release BLE memory: probes need it again.
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (!check(esp_bt_controller_init(&cfg), "Init BLE controller") ||
        !check(esp_bt_controller_enable(ESP_BT_MODE_BLE), "Enable BLE controller") ||
        !check(esp_bluedroid_init(), "Init Bluedroid") ||
        !check(esp_bluedroid_enable(), "Enable Bluedroid") ||
        !check(esp_ble_gap_register_callback(gap_callback), "Register GAP callback")) {
        beacon_tracker_stop();
        return false;
    }
    if(!ble_telemetry_start()) ESP_LOGW(TAG,"Optional BLE telemetry unavailable; beacon scanning continues");
    static esp_ble_scan_params_t params = {};
    params.scan_type = BLE_SCAN_TYPE_PASSIVE;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL;
    params.scan_interval = 0x50;
    params.scan_window = 0x30;
    params.scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE;
    if (!check(esp_ble_gap_set_scan_params(&params), "Set scan parameters")) {
        beacon_tracker_stop();
        return false;
    }
    ESP_LOGI(TAG, "Scanning for driver fob 78:9f:38:0f:f4:43");
    return true;
}

BeaconStatus beacon_tracker_status()
{
    portENTER_CRITICAL(&s_lock);
    // Read time inside the lock so it cannot precede a just-arrived packet.
    const TickType_t now = xTaskGetTickCount();
    const bool fresh = s_count && now - s_last_seen <= pdMS_TO_TICKS(BEACON_TIMEOUT_MS);
    const int average = fresh ? average_rssi() : 0;
    if (s_running && !fresh &&
        (s_count || now - s_init_tick > pdMS_TO_TICKS(BEACON_TIMEOUT_MS))) {
        s_near = false;
        s_checking = false;
    }
    else if (s_running && fresh && s_count >= MIN_SAMPLES) {
        if (average >= RSSI_THRESHOLD) s_near = true;
        else if (average < RSSI_THRESHOLD - RSSI_HYSTERESIS) s_near = false;
        s_checking = false;
    }
    const BeaconStatus status = {s_running,
        s_near, s_running && fresh, average, s_checking};
    portEXIT_CRITICAL(&s_lock);
    return status;
}

bool beacon_tracker_driver_near()
{
    const auto status = beacon_tracker_status();
    return status.available && status.driver_present && !status.checking;
}
int beacon_tracker_rssi()
{
    const auto status = beacon_tracker_status();
    return status.rssi_valid ? status.rssi_dbm : 0;
}
