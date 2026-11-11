#pragma once
#include "esp_gap_ble_api.h"

// Forward the shared beacon/telemetry GAP events after checking shutdown state.
void ble_telemetry_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
