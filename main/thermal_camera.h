#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t THERMAL_CAMERA_WIDTH = 32;
constexpr size_t THERMAL_CAMERA_HEIGHT = 24;
constexpr size_t THERMAL_CAMERA_PIXELS =
    THERMAL_CAMERA_WIDTH * THERMAL_CAMERA_HEIGHT;

// Initializes the MLX90640 on I2C0 (SDA GPIO 21, SCL GPIO 47). A missing
// sensor is reported but does not prevent the rest of the device from running.
bool thermal_camera_init();

// Acquires a complete two-subpage frame and formats all 768 Celsius readings
// as one comma-separated row. The returned pointer remains valid until the
// next call. Returns nullptr when the camera is unavailable or a read fails.
const char *thermal_camera_read_csv(size_t *csv_length);

// Returns the debounced detection of a connected region within the configured
// skin-temperature range and above the frame background. Not person recognition.
bool thermal_camera_heat_trace_detected();

// Returns the latest detected blob bounds in MLX90640 pixel coordinates.
// Returns false unless a debounced heat trace has qualifying current-frame bounds.
bool thermal_camera_get_heat_trace_bounds(uint8_t *x, uint8_t *y,
                                          uint8_t *width, uint8_t *height);
