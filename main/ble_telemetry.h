#pragma once

#include <stddef.h>
#include <stdint.h>

// Shares the beacon tracker's Bluedroid host and GAP callback. Stop before
// disabling that host; a viewer connection never prevents driver deep sleep.
bool ble_telemetry_start();
void ble_telemetry_stop();
bool ble_telemetry_connected();
bool ble_telemetry_take_sms_request();
void ble_telemetry_flush(); // Bounded best effort for the final driver report.

// Copies into bounded, latest-packet mailboxes; never waits for the radio.
bool ble_telemetry_send_packet(const char magic[4], const uint8_t *payload, size_t length);
