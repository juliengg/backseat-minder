#pragma once

#include <stddef.h>
#include <stdint.h>

// Starts the ESP32-S3 native USB Serial/JTAG channel used for optional
// development telemetry. Failure to connect USB never affects device behavior.
void usb_telemetry_init();

// Sends one newline-delimited JSON sample to a connected USB host.
// Values remain processed locally; this is development-only observability.
void usb_telemetry_send(float temperature_f, float humidity_percent,
                        bool temperature_humidity_valid, bool face_detected,
                        bool mmwave_person_detected);

// Sends one JPEG frame with a binary BSMF header. This is used only by the
// development camera viewer connected to native USB.
bool usb_telemetry_send_camera_frame(const uint8_t *jpeg, size_t jpeg_length);

// Returns true only while a native USB host is connected.
bool usb_telemetry_host_connected();
