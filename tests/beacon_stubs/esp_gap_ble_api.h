#pragma once
#include <cstdint>
#include "esp_err.h"
using esp_bd_addr_t = uint8_t[6];
enum esp_gap_ble_cb_event_t {
    ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,
    ESP_GAP_BLE_SCAN_RESULT_EVT, ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT
};
constexpr int ESP_BT_STATUS_SUCCESS = 0;
constexpr int ESP_GAP_SEARCH_INQ_RES_EVT = 0;
constexpr int ESP_GAP_SEARCH_INQ_CMPL_EVT = 1;
constexpr int BLE_SCAN_TYPE_PASSIVE = 0;
constexpr int BLE_ADDR_TYPE_PUBLIC = 0;
constexpr int BLE_SCAN_FILTER_ALLOW_ALL = 0;
constexpr int BLE_SCAN_DUPLICATE_DISABLE = 0;
struct esp_ble_gap_cb_param_t {
    struct { int status; } scan_param_cmpl{}, scan_start_cmpl{};
    struct { int search_evt; esp_bd_addr_t bda; int rssi; } scan_rst{};
};
struct esp_ble_scan_params_t {
    int scan_type, own_addr_type, scan_filter_policy, scan_interval, scan_window, scan_duplicate;
};
using esp_gap_ble_cb_t = void (*)(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t *);
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t);
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *);
esp_err_t esp_ble_gap_start_scanning(uint32_t);
