#include "temp_humidity_sensor.h"

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace {

// GPIO 4 is used by the camera; GPIO 2 drives the status LED.
constexpr gpio_num_t SENSOR_GPIO = GPIO_NUM_1;
const char *TAG = "temp_humidity";
rmt_channel_handle_t rx_channel = nullptr;
QueueHandle_t receive_queue = nullptr;
// Reserve two hardware blocks below so the full response fits before the
// driver's half-buffer interrupt threshold (48 symbols).
rmt_symbol_word_t symbols[48];

bool receive_done(rmt_channel_handle_t, const rmt_rx_done_event_data_t *event, void *)
{
    BaseType_t awakened = pdFALSE;
    xQueueSendFromISR(receive_queue, &event->num_symbols, &awakened);
    return awakened == pdTRUE;
}

bool decode_response(size_t count, uint8_t data[5])
{
    // Capture can begin partway through the host's low start pulse.
    struct Pulse { unsigned level; unsigned duration; } pulses[96];
    size_t length = 0;
    for (size_t i = 0; i < count; ++i) {
        if (symbols[i].duration0 == 0) break;
        pulses[length++] = {symbols[i].level0, symbols[i].duration0};
        if (symbols[i].duration1 == 0) break;
        pulses[length++] = {symbols[i].level1, symbols[i].duration1};
    }
    size_t start = 0;
    bool acknowledged = false;
    // Find the 80 us low/high acknowledgement in the preamble only.
    for (size_t i = 0; i + 1 < length && i < 4; ++i) {
        if (pulses[i].level == 0 && pulses[i].duration >= 60 &&
            pulses[i].duration <= 110 && pulses[i + 1].level == 1 &&
            pulses[i + 1].duration >= 60 && pulses[i + 1].duration <= 110) {
            start = i + 2;
            acknowledged = true;
            break;
        }
    }
    if (!acknowledged) {
        ESP_LOGW(TAG, "AM2302/DHT22 acknowledgement missing; check wiring/pull-up");
        return false;
    }
    if (length < start + 80) {
        ESP_LOGW(TAG, "AM2302/DHT22 incomplete response: %u of 40 bits",
                 static_cast<unsigned>((length - start) / 2));
        return false;
    }
    for (size_t bit = 0; bit < 40; ++bit) {
        const auto &low = pulses[start + bit * 2];
        const auto &high = pulses[start + bit * 2 + 1];
        if (low.level != 0 || high.level != 1 || low.duration < 30 ||
            low.duration > 75 || high.duration < 15 || high.duration > 100) {
            ESP_LOGW(TAG, "AM2302/DHT22 malformed pulse at bit %u",
                     static_cast<unsigned>(bit));
            return false;
        }
        data[bit / 8] = static_cast<uint8_t>((data[bit / 8] << 1) |
                                            (high.duration > 50));
    }
    return true;
}

bool read_dht22(uint8_t data[5])
{
    size_t count = 0;
    while (xQueueReceive(receive_queue, &count, 0) == pdTRUE) {}
    // Input remains enabled while driving open-drain, preserving RMT routing.
    gpio_set_level(SENSOR_GPIO, 0);
    esp_rom_delay_us(1200);
    rmt_receive_config_t receive_config = {};
    receive_config.signal_range_min_ns = 2000;
    receive_config.signal_range_max_ns = 10000000; // 10 ms idle ends capture
    const esp_err_t error = rmt_receive(rx_channel, symbols, sizeof(symbols), &receive_config);
    gpio_set_level(SENSOR_GPIO, 1);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not start sensor capture: %s", esp_err_to_name(error));
        return false;
    }
    if (xQueueReceive(receive_queue, &count, pdMS_TO_TICKS(100)) != pdTRUE) {
        // Stop a stuck receive before the static buffer is reused next time.
        ESP_ERROR_CHECK(rmt_disable(rx_channel));
        ESP_ERROR_CHECK(rmt_enable(rx_channel));
        ESP_LOGW(TAG, "AM2302/DHT22 capture timed out");
        return false;
    }
    if (count > 48) {
        ESP_LOGW(TAG, "AM2302/DHT22 capture overflow");
        return false;
    }
    return decode_response(count, data);
}

} // namespace

void temp_humidity_sensor_init()
{
    receive_queue = xQueueCreate(1, sizeof(size_t));
    ESP_ERROR_CHECK(receive_queue ? ESP_OK : ESP_ERR_NO_MEM);
    rmt_rx_channel_config_t rx_config = {};
    rx_config.gpio_num = SENSOR_GPIO;
    rx_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_config.resolution_hz = 1000000; // One hardware tick per microsecond.
    rx_config.mem_block_symbols = 96;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rx_channel));
    ESP_ERROR_CHECK(gpio_set_level(SENSOR_GPIO, 1));
    ESP_ERROR_CHECK(gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT_OUTPUT_OD));
    // An external pull-up is required; do not rely on the weak internal one.
    ESP_ERROR_CHECK(gpio_set_pull_mode(SENSOR_GPIO, GPIO_FLOATING));
    rmt_rx_event_callbacks_t callbacks = {};
    callbacks.on_recv_done = receive_done;
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &callbacks, nullptr));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_LOGI(TAG, "AM2302/DHT22 RMT capture initialized on GPIO %d", SENSOR_GPIO);
}

bool temp_humidity_sensor_read(float *temperature_f, float *humidity_percent)
{
    if (!temperature_f || !humidity_percent || !rx_channel) return false;
    uint8_t data[5] = {};
    if (!read_dht22(data)) return false;
    const uint8_t checksum = static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]);
    if (data[4] != checksum) {
        ESP_LOGW(TAG, "AM2302/DHT22 checksum mismatch");
        return false;
    }
    const uint16_t raw_humidity = static_cast<uint16_t>((data[0] << 8) | data[1]);
    const uint16_t raw_temperature = static_cast<uint16_t>(((data[2] & 0x7F) << 8) | data[3]);
    float temperature_c = raw_temperature / 10.0f;
    if (data[2] & 0x80) temperature_c = -temperature_c;
    if (raw_humidity > 1000 || temperature_c < -40 || temperature_c > 80) {
        ESP_LOGW(TAG, "AM2302/DHT22 sample outside sensor range");
        return false;
    }
    *temperature_f = (temperature_c * 9.0f / 5.0f) + 32.0f;
    *humidity_percent = raw_humidity / 10.0f;
    return true;
}
