#include "temp_humidity_sensor.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace {

// GPIO 4 is used by the camera's SIOD control bus; GPIO 2 drives the status LED.
constexpr gpio_num_t SENSOR_GPIO = GPIO_NUM_1;
constexpr int RESPONSE_TIMEOUT_US = 120;
constexpr int BIT_TIMEOUT_US = 100;
constexpr int BIT_ONE_THRESHOLD_US = 50;

const char *TAG = "temp_humidity";

bool wait_for_level(int level, int timeout_us)
{
    const int64_t deadline = esp_timer_get_time() + timeout_us;
    while (gpio_get_level(SENSOR_GPIO) != level) {
        if (esp_timer_get_time() >= deadline) {
            return false;
        }
    }
    return true;
}

bool read_dht22(uint8_t data[5])
{
    // DHT22/AM2302 start signal: pull the single-wire bus low for at least 1 ms.
    gpio_set_direction(SENSOR_GPIO, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(SENSOR_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(SENSOR_GPIO, 0);
    esp_rom_delay_us(1200);
    gpio_set_level(SENSOR_GPIO, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT);

    // Sensor acknowledgement: approximately 80 us low, then 80 us high.
    if (!wait_for_level(0, RESPONSE_TIMEOUT_US) ||
        !wait_for_level(1, RESPONSE_TIMEOUT_US) ||
        !wait_for_level(0, RESPONSE_TIMEOUT_US)) {
        return false;
    }

    for (int bit_index = 0; bit_index < 40; ++bit_index) {
        // Each data bit starts low, followed by a high pulse. A longer high pulse is 1.
        if (!wait_for_level(1, BIT_TIMEOUT_US)) {
            return false;
        }

        const int64_t pulse_started = esp_timer_get_time();
        if (!wait_for_level(0, BIT_TIMEOUT_US)) {
            return false;
        }

        const int high_pulse_us = static_cast<int>(esp_timer_get_time() - pulse_started);
        data[bit_index / 8] <<= 1;
        if (high_pulse_us > BIT_ONE_THRESHOLD_US) {
            data[bit_index / 8] |= 1;
        }
    }

    return true;
}

} // namespace

void temp_humidity_sensor_init()
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << SENSOR_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(TAG, "AM2302/DHT22 initialized on GPIO %d", SENSOR_GPIO);
}

bool temp_humidity_sensor_read(float *temperature_f, float *humidity_percent)
{
    if (!temperature_f || !humidity_percent) {
        return false;
    }

    uint8_t data[5] = {};
    if (!read_dht22(data)) {
        ESP_LOGW(TAG, "AM2302/DHT22 did not respond");
        return false;
    }

    const uint8_t checksum = static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]);
    if (data[4] != checksum) {
        ESP_LOGW(TAG, "AM2302/DHT22 checksum mismatch");
        return false;
    }

    const uint16_t raw_humidity = static_cast<uint16_t>((data[0] << 8) | data[1]);
    const uint16_t raw_temperature = static_cast<uint16_t>(((data[2] & 0x7F) << 8) | data[3]);

    float temperature_c = raw_temperature / 10.0f;
    if (data[2] & 0x80) {
        temperature_c = -temperature_c;
    }

    *temperature_f = (temperature_c * 9.0f / 5.0f) + 32.0f;
    *humidity_percent = raw_humidity / 10.0f;
    return true;
}
