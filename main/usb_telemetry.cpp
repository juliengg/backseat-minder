#include "usb_telemetry.h"

#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

const char *TAG = "usb_telemetry";
bool s_ready = false;
SemaphoreHandle_t s_write_mutex = nullptr;

bool write_bytes(const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const size_t chunk_length = (length - sent > 256) ? 256 : length - sent;
        const int written = usb_serial_jtag_write_bytes(data + sent, chunk_length,
                                                         pdMS_TO_TICKS(20));
        if (written <= 0) {
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

bool send_packet(const char magic[4], const uint8_t *payload, size_t payload_length)
{
    const uint8_t header[] = {
        static_cast<uint8_t>(magic[0]), static_cast<uint8_t>(magic[1]),
        static_cast<uint8_t>(magic[2]), static_cast<uint8_t>(magic[3]),
        static_cast<uint8_t>((payload_length >> 24) & 0xFF),
        static_cast<uint8_t>((payload_length >> 16) & 0xFF),
        static_cast<uint8_t>((payload_length >> 8) & 0xFF),
        static_cast<uint8_t>(payload_length & 0xFF),
    };
    return write_bytes(header, sizeof(header)) && write_bytes(payload, payload_length);
}

} // namespace

void usb_telemetry_init()
{
    usb_serial_jtag_driver_config_t config = {};
    config.tx_buffer_size = 512;
    config.rx_buffer_size = 128;

    const esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err == ESP_OK) {
        s_write_mutex = xSemaphoreCreateMutex();
        if (!s_write_mutex) {
            ESP_LOGW(TAG, "Native USB telemetry mutex could not be created");
            return;
        }
        s_ready = true;
        ESP_LOGI(TAG, "Native USB telemetry is ready");
    } else {
        // USB is optional: do not prevent standalone operation if it is unavailable.
        ESP_LOGW(TAG, "Native USB telemetry unavailable: %s", esp_err_to_name(err));
    }
}

bool usb_telemetry_host_connected()
{
    // The USB Serial/JTAG connection monitor is not reliable on every board
    // routing of the native USB pins. A write with a short timeout is the
    // authoritative connectivity test, so do not suppress output here.
    return s_ready;
}

void usb_telemetry_send(float temperature_f, float humidity_percent,
                        bool temperature_humidity_valid, bool face_detected)
{
    if (!usb_telemetry_host_connected() ||
        xSemaphoreTake(s_write_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    char message[192];
    const int length = snprintf(
        message, sizeof(message),
        "{\"uptime_ms\":%lld,\"face_detected\":%s,"
        "\"temperature_humidity_valid\":%s,\"temperature_f\":%.1f,"
        "\"humidity_percent\":%.1f}\n",
        esp_timer_get_time() / 1000,
        face_detected ? "true" : "false",
        temperature_humidity_valid ? "true" : "false",
        temperature_f, humidity_percent);

    if (length > 0 && length < static_cast<int>(sizeof(message))) {
        send_packet("BSMT", reinterpret_cast<const uint8_t *>(message),
                    static_cast<size_t>(length));
    }
    xSemaphoreGive(s_write_mutex);
}

bool usb_telemetry_send_camera_frame(const uint8_t *jpeg, size_t jpeg_length)
{
    if (!jpeg || jpeg_length == 0 || !usb_telemetry_host_connected() ||
        xSemaphoreTake(s_write_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    // Packet type BSMF: JPEG payload length is a big-endian uint32.
    const bool sent = send_packet("BSMF", jpeg, jpeg_length);
    xSemaphoreGive(s_write_mutex);
    return sent;
}
