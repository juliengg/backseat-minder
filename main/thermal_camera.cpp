#include "thermal_camera.h"

#include <stdio.h>

extern "C" {
#include "MLX90640_API.h"
}
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr i2c_port_num_t I2C_PORT = I2C_NUM_0;
constexpr gpio_num_t I2C_SDA = GPIO_NUM_21;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_47;
constexpr uint32_t I2C_FREQUENCY_HZ = 1000000;
constexpr uint8_t MLX90640_ADDRESS = 0x33;
constexpr uint8_t MLX90640_18_BIT = 2;
constexpr uint8_t MLX90640_8_HZ = 4;
constexpr float EMISSIVITY = 0.95f;
constexpr size_t CSV_CAPACITY = THERMAL_CAMERA_PIXELS * 10;

const char *TAG = "thermal_camera";
paramsMLX90640 s_parameters = {};
uint16_t s_sensor_data[834] = {};
float s_temperatures[THERMAL_CAMERA_PIXELS] = {};
char s_csv[CSV_CAPACITY] = {};
bool s_ready = false;
i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_i2c_device = nullptr;
TaskHandle_t s_initialization_caller = nullptr;

} // namespace

extern "C" int MLX90640_I2CRead(uint8_t slave_address, uint16_t start_address,
                                  uint16_t word_count, uint16_t *data)
{
    uint8_t address[] = {
        static_cast<uint8_t>(start_address >> 8),
        static_cast<uint8_t>(start_address & 0xff),
    };
    const size_t byte_count = static_cast<size_t>(word_count) * 2;
    uint8_t *bytes = reinterpret_cast<uint8_t *>(data);
    if (!s_i2c_device || slave_address != MLX90640_ADDRESS) {
        return -MLX90640_I2C_NACK_ERROR;
    }
    const esp_err_t error = i2c_master_transmit_receive(
        s_i2c_device, address, sizeof(address), bytes, byte_count, 1000);
    if (error != ESP_OK) {
        return -MLX90640_I2C_NACK_ERROR;
    }

    // The sensor transmits words most-significant byte first.
    for (uint16_t index = 0; index < word_count; ++index) {
        data[index] = static_cast<uint16_t>(bytes[index * 2] << 8) |
                      bytes[index * 2 + 1];
    }
    return MLX90640_NO_ERROR;
}

extern "C" int MLX90640_I2CWrite(uint8_t slave_address, uint16_t address,
                                   uint16_t value)
{
    const uint8_t bytes[] = {
        static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value),
    };
    if (!s_i2c_device || slave_address != MLX90640_ADDRESS) {
        return -MLX90640_I2C_WRITE_ERROR;
    }
    const esp_err_t error = i2c_master_transmit(
        s_i2c_device, bytes, sizeof(bytes), 1000);
    return error == ESP_OK ? MLX90640_NO_ERROR : -MLX90640_I2C_WRITE_ERROR;
}

extern "C" void MLX90640_I2CInit(void) {}
extern "C" void MLX90640_I2CFreqSet(int) {}
extern "C" int MLX90640_I2CGeneralReset(void) { return MLX90640_NO_ERROR; }

namespace {

bool initialize_sensor()
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_PORT;
    bus_config.sda_io_num = I2C_SDA;
    bus_config.scl_io_num = I2C_SCL;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t error = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (error == ESP_OK) {
        i2c_device_config_t device_config = {};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = MLX90640_ADDRESS;
        device_config.scl_speed_hz = I2C_FREQUENCY_HZ;
        error = i2c_master_bus_add_device(s_i2c_bus, &device_config,
                                          &s_i2c_device);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Could not initialize I2C: %s", esp_err_to_name(error));
        return false;
    }

    int result = MLX90640_DumpEE(MLX90640_ADDRESS, s_sensor_data);
    if (result == MLX90640_NO_ERROR) {
        result = MLX90640_ExtractParameters(s_sensor_data, &s_parameters);
    }
    if (result == MLX90640_NO_ERROR) {
        result = MLX90640_SetResolution(MLX90640_ADDRESS, MLX90640_18_BIT);
    }
    if (result == MLX90640_NO_ERROR) {
        result = MLX90640_SetRefreshRate(MLX90640_ADDRESS, MLX90640_8_HZ);
    }
    if (result != MLX90640_NO_ERROR) {
        ESP_LOGW(TAG, "MLX90640 initialization failed (%d); continuing without it", result);
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "MLX90640 ready on SDA GPIO %d / SCL GPIO %d", I2C_SDA, I2C_SCL);
    return true;
}

void initialization_task(void *)
{
    initialize_sensor();
    xTaskNotifyGive(s_initialization_caller);
    vTaskDelete(nullptr);
}

} // namespace

bool thermal_camera_init()
{
    // Melexis parameter extraction temporarily allocates a 768-float work
    // array. Run it on a short-lived task rather than overflowing ESP-IDF's
    // 3584-byte main task stack.
    s_initialization_caller = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(initialization_task, "mlx90640_init", 8192, nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Could not create MLX90640 initialization task");
        return false;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_initialization_caller = nullptr;
    return s_ready;
}

const char *thermal_camera_read_csv(size_t *csv_length)
{
    if (csv_length) {
        *csv_length = 0;
    }
    if (!s_ready) {
        return nullptr;
    }

    bool subpage_seen[2] = {};
    for (int attempt = 0; attempt < 4 && (!subpage_seen[0] || !subpage_seen[1]); ++attempt) {
        const int subpage = MLX90640_GetFrameData(MLX90640_ADDRESS, s_sensor_data);
        if (subpage < 0 || subpage > 1) {
            ESP_LOGW(TAG, "MLX90640 frame read failed (%d)", subpage);
            return nullptr;
        }
        const float ambient = MLX90640_GetTa(s_sensor_data, &s_parameters);
        const float reflected = ambient - 8.0f;
        MLX90640_CalculateTo(s_sensor_data, &s_parameters, EMISSIVITY, reflected,
                            s_temperatures);
        MLX90640_BadPixelsCorrection(s_parameters.brokenPixels, s_temperatures,
                                     1, &s_parameters);
        MLX90640_BadPixelsCorrection(s_parameters.outlierPixels, s_temperatures,
                                     1, &s_parameters);
        subpage_seen[subpage] = true;
    }
    if (!subpage_seen[0] || !subpage_seen[1]) {
        ESP_LOGW(TAG, "MLX90640 did not provide both subpages");
        return nullptr;
    }

    size_t used = 0;
    for (size_t index = 0; index < THERMAL_CAMERA_PIXELS; ++index) {
        const int written = snprintf(s_csv + used, sizeof(s_csv) - used,
                                     index ? ",%.2f" : "%.2f", s_temperatures[index]);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(s_csv) - used) {
            ESP_LOGW(TAG, "Thermal CSV buffer is too small");
            return nullptr;
        }
        used += static_cast<size_t>(written);
    }
    if (csv_length) {
        *csv_length = used;
    }
    return s_csv;
}
