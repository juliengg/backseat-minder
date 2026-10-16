#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Starts the shared JPEG consumer of face-detection output. Native USB previews
// run at up to 3 FPS; setup positioning snapshots are supplied on demand at 1 FPS.
void usb_camera_stream_start(QueueHandle_t frame_queue);
