#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Starts a two-stage ESP-WHO face detector tuned to favor recall over the
// stock example's false-positive rate.
void register_tuned_face_detection(QueueHandle_t frame_i,
                                   QueueHandle_t frame_o = nullptr,
                                   bool camera_fb_return = false);

// Returns whether a face was detected since the previous call.
bool get_tuned_face_detected();
