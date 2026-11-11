#pragma once
#include <stddef.h>
#include <stdint.h>
// Optional transport. Failure never blocks sensors, alerts, or beacon scanning.
void ble_telemetry_init();
bool ble_telemetry_start(); // Existing Bluedroid host must already be enabled.
bool ble_telemetry_stop(); // Quiesce before that shared host is disabled.
bool ble_telemetry_connected();
// Includes pairing/subscription so an attached viewer can finish connecting.
bool ble_telemetry_client_connected();
bool ble_telemetry_send_packet(const char magic[4], const uint8_t *data, size_t size);
bool ble_telemetry_take_sms_request();
