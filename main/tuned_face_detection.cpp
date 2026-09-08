#include "tuned_face_detection.h"

#include <atomic>
#include <list>

#include "dl_detect_define.hpp"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"
#include "who_ai_utils.hpp"

namespace {

// Slightly lower confidence cutoffs admit more marginal face detections.
constexpr float STAGE_ONE_SCORE_THRESHOLD = 0.18F;
constexpr float STAGE_ONE_NMS_THRESHOLD = 0.30F;
constexpr int STAGE_ONE_TOP_K = 10;
constexpr float STAGE_ONE_RESIZE_SCALE = 0.50F;

constexpr float STAGE_TWO_SCORE_THRESHOLD = 0.28F;
constexpr float STAGE_TWO_NMS_THRESHOLD = 0.30F;
constexpr int STAGE_TWO_TOP_K = 10;

const char *TAG = "tuned_face_detection";
QueueHandle_t s_frame_input = nullptr;
QueueHandle_t s_frame_output = nullptr;
bool s_return_camera_frame = false;
std::atomic<bool> s_face_detected{false};

void face_detection_task(void *)
{
    HumanFaceDetectMSR01 stage_one(STAGE_ONE_SCORE_THRESHOLD,
                                   STAGE_ONE_NMS_THRESHOLD,
                                   STAGE_ONE_TOP_K,
                                   STAGE_ONE_RESIZE_SCALE);
    HumanFaceDetectMNP01 stage_two(STAGE_TWO_SCORE_THRESHOLD,
                                   STAGE_TWO_NMS_THRESHOLD,
                                   STAGE_TWO_TOP_K);

    while (true) {
        camera_fb_t *frame = nullptr;
        if (xQueueReceive(s_frame_input, &frame, portMAX_DELAY) != pdTRUE ||
            frame == nullptr) {
            continue;
        }

        std::list<dl::detect::result_t> &candidates = stage_one.infer(
            reinterpret_cast<uint16_t *>(frame->buf),
            {static_cast<int>(frame->height), static_cast<int>(frame->width), 3});
        std::list<dl::detect::result_t> &results = stage_two.infer(
            reinterpret_cast<uint16_t *>(frame->buf),
            {static_cast<int>(frame->height), static_cast<int>(frame->width), 3},
            candidates);

        if (!results.empty()) {
            draw_detection_result(reinterpret_cast<uint16_t *>(frame->buf),
                                  frame->height, frame->width, results);
            print_detection_result(results);
            s_face_detected.store(true, std::memory_order_release);
        }

        if (s_frame_output) {
            xQueueSend(s_frame_output, &frame, portMAX_DELAY);
        } else if (s_return_camera_frame) {
            esp_camera_fb_return(frame);
        } else {
            free(frame);
        }
    }
}

} // namespace

void register_tuned_face_detection(QueueHandle_t frame_i,
                                   QueueHandle_t frame_o,
                                   bool camera_fb_return)
{
    s_frame_input = frame_i;
    s_frame_output = frame_o;
    s_return_camera_frame = camera_fb_return;
    ESP_LOGI(TAG, "Thresholds: stage1 %.2f, stage2 %.2f; resize scale %.2f",
             STAGE_ONE_SCORE_THRESHOLD, STAGE_TWO_SCORE_THRESHOLD,
             STAGE_ONE_RESIZE_SCALE);
    xTaskCreatePinnedToCore(face_detection_task, TAG, 4 * 1024, nullptr, 5,
                            nullptr, 0);
}

bool get_tuned_face_detected()
{
    return s_face_detected.exchange(false, std::memory_order_acq_rel);
}
