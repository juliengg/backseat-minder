#pragma once
#include "esp_err.h"
enum esp_bt_controller_status_t { ESP_BT_CONTROLLER_STATUS_IDLE, ESP_BT_CONTROLLER_STATUS_INITED, ESP_BT_CONTROLLER_STATUS_ENABLED };
constexpr int ESP_BT_MODE_BLE = 1;
struct esp_bt_controller_config_t { int unused; };
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() esp_bt_controller_config_t{}
esp_bt_controller_status_t esp_bt_controller_get_status();
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *);
esp_err_t esp_bt_controller_enable(int);
esp_err_t esp_bt_controller_disable();
esp_err_t esp_bt_controller_deinit();
