#pragma once

#include <stddef.h>

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
