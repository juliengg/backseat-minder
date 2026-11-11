#pragma once
#include "esp_gap_ble_api.h"
// Forward from the single GAP callback owned by the beacon tracker.
void ble_telemetry_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
