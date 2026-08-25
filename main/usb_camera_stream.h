#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Starts a task that converts face-detection output frames to JPEG and sends
// at most one development preview frame per second over native USB.
void usb_camera_stream_start(QueueHandle_t frame_queue);
