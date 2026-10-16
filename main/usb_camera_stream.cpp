#include "usb_camera_stream.h"

#include <stdlib.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "positioning_preview.h"
#include "usb_telemetry.h"

namespace {

constexpr int64_t FRAME_INTERVAL_US = 1000000 / 3; // Target three preview FPS.
const char *TAG = "usb_camera";
QueueHandle_t s_frame_queue = nullptr;

void camera_stream_task(void *)
{
    int64_t last_frame_sent = 0;

    while (true) {
        camera_fb_t *frame = nullptr;
        if (xQueueReceive(s_frame_queue, &frame, portMAX_DELAY) != pdTRUE || !frame) {
            continue;
        }

        const int64_t now = esp_timer_get_time();
        const bool usb_due = usb_telemetry_host_connected() &&
                             now - last_frame_sent >= FRAME_INTERVAL_US;
        const bool positioning_due = positioning_preview_frame_due(now);
        if (usb_due || positioning_due) {
            // Rate-limit attempts too: an unplugged development cable should
            // not make JPEG conversion run for every captured camera frame.
            if (usb_due) last_frame_sent = now;
            uint8_t *jpeg = nullptr;
            size_t jpeg_length = 0;
            if (frame2jpg(frame, 50, &jpeg, &jpeg_length)) {
                if (positioning_due) positioning_preview_publish(jpeg, jpeg_length);
                if (usb_due) usb_telemetry_send_camera_frame(jpeg, jpeg_length);
                free(jpeg);
            } else {
                ESP_LOGW(TAG, "JPEG conversion failed");
            }
        }

        // The face-detection stage handed ownership of this camera buffer to us.
        esp_camera_fb_return(frame);
    }
}

} // namespace

void usb_camera_stream_start(QueueHandle_t frame_queue)
{
    positioning_preview_init();
    s_frame_queue = frame_queue;
    if (!s_frame_queue) {
        ESP_LOGE(TAG, "Camera stream queue is not available");
        return;
    }

    xTaskCreatePinnedToCore(camera_stream_task, "usb_camera", 8192, nullptr, 4, nullptr, 1);
}
