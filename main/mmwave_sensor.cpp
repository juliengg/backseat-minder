#include "mmwave_sensor.h"

#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr uart_port_t SENSOR_UART = UART_NUM_1;
constexpr int SENSOR_RX_GPIO = 41;
constexpr int SENSOR_TX_GPIO = 42;
constexpr int SENSOR_BAUD_RATE = 9600;
constexpr size_t LINE_BUFFER_SIZE = 96;

const char *TAG = "mmwave_sensor";
char s_line_buffer[LINE_BUFFER_SIZE] = {};
size_t s_line_length = 0;
bool s_person_detected = false;

void process_packet()
{
    s_line_buffer[s_line_length] = '\0';

    constexpr char PREFIX[] = "$DFHPD,";
    if (std::strncmp(s_line_buffer, PREFIX, sizeof(PREFIX) - 1) == 0) {
        const char value = s_line_buffer[sizeof(PREFIX) - 1];
        if (value == '0' || value == '1') {
            const bool detected = value == '1';
            if (detected != s_person_detected) {
                ESP_LOGI(TAG, "Person %s", detected ? "detected" : "no longer detected");
            }
            s_person_detected = detected;
        }
    }

    s_line_length = 0;
}

void process_byte(char value)
{
    if (value == '*' || value == '\n') {
        if (s_line_length > 0) {
            process_packet();
        }
        return;
    }

    if (value == '\r') {
        return;
    }

    if (s_line_length < LINE_BUFFER_SIZE - 1) {
        s_line_buffer[s_line_length++] = value;
    } else {
        // Discard an oversized or malformed packet and resynchronize.
        s_line_length = 0;
    }
}

void drain_uart()
{
    uint8_t data[64];
    int bytes_read = 0;
    do {
        bytes_read = uart_read_bytes(SENSOR_UART, data, sizeof(data), 0);
        for (int index = 0; index < bytes_read; ++index) {
            process_byte(static_cast<char>(data[index]));
        }
    } while (bytes_read > 0);
}

void send_command(const char *command)
{
    uart_write_bytes(SENSOR_UART, command, std::strlen(command));
    uart_write_bytes(SENSOR_UART, "\r\n", 2);
    uart_wait_tx_done(SENSOR_UART, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(150));
    drain_uart();
}

void configure_sensor()
{
    ESP_LOGI(TAG, "Configuring C4001 presence sensor");
    send_command("sensorStop");
    send_command("setRange 0.6 1");
    send_command("setTrigRange 1");
    send_command("setLatency 0.05 1");
    send_command("setSensitivity 7 8");
    send_command("saveConfig");
    send_command("sensorStart");
    ESP_LOGI(TAG, "C4001 presence sensor configured");
}

} // namespace

void mmwave_sensor_init()
{
    uart_config_t config = {};
    config.baud_rate = SENSOR_BAUD_RATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(SENSOR_UART, 512, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(SENSOR_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(SENSOR_UART, SENSOR_TX_GPIO, SENSOR_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // The sensor requires at least one second after power-up before commands.
    vTaskDelay(pdMS_TO_TICKS(1200));
    configure_sensor();
}

bool mmwave_sensor_person_detected()
{
    drain_uart();
    return s_person_detected;
}
