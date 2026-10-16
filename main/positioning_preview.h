#pragma once

#include <stddef.h>
#include <stdint.h>

// Initialize before starting the camera consumer task. Failure disables preview.
void positioning_preview_init();
// Only setup mode may request frames. Turning it off discards the cached JPEG.
void positioning_preview_set_active(bool active);
// Camera task: claims at most one conversion attempt per second, on HTTP demand.
bool positioning_preview_frame_due(int64_t now_us);
// Copies a JPEG from the existing camera pipeline; never keeps a camera buffer.
void positioning_preview_publish(const uint8_t *jpeg, size_t length);
// HTTP task: renews demand and copies a recent cached JPEG without waiting for
// capture. Caller frees the result. nullptr means unavailable; *length is zero.
uint8_t *positioning_preview_snapshot(size_t *length);
