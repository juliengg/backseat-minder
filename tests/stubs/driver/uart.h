#pragma once
#include <cstddef>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
using uart_port_t = int;
constexpr int UART_NUM_2 = 2;
constexpr int UART_DATA_8_BITS = 8;
constexpr int UART_PARITY_DISABLE = 0;
constexpr int UART_STOP_BITS_1 = 1;
constexpr int UART_HW_FLOWCTRL_DISABLE = 0;
constexpr int UART_SCLK_DEFAULT = 0;
constexpr int UART_PIN_NO_CHANGE = -1;
struct uart_config_t { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; };
inline int uart_driver_install(int, int, int, int, void *, int) { return ESP_OK; }
inline int uart_driver_delete(int) { return ESP_OK; }
inline int uart_param_config(int, const uart_config_t *) { return ESP_OK; }
inline int uart_set_pin(int, int, int, int, int) { return ESP_OK; }
int uart_flush_input(int);
int uart_write_bytes(int, const void *, size_t);
int uart_read_bytes(int, void *, size_t, TickType_t);
