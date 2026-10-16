#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
struct usb_serial_jtag_driver_config_t { size_t tx_buffer_size; size_t rx_buffer_size; };
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *);
int usb_serial_jtag_read_bytes(void *, uint32_t, TickType_t);
int usb_serial_jtag_write_bytes(const void *, size_t, TickType_t);
