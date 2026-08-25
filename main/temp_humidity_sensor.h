#pragma once

// Initializes the AM2302/DHT22 data pin (GPIO 4 by default).
void temp_humidity_sensor_init();

// Reads one sample from the AM2302/DHT22.
// On success, writes Fahrenheit and relative-humidity values and returns true.
// On failure, leaves the supplied values unchanged and returns false.
bool temp_humidity_sensor_read(float *temperature_f, float *humidity_percent);
