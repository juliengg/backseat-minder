#include "who_camera.h"
#include "who_human_face_detection.hpp"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO GPIO_NUM_2

static QueueHandle_t xQueueAIFrame = NULL;

extern "C" void app_main()
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 1, xQueueAIFrame);
    register_human_face_detection(xQueueAIFrame, NULL, NULL, NULL, true);

    while (true)
    {
        if (get_face_detected())
            gpio_set_level(LED_GPIO, 1);
        else
            gpio_set_level(LED_GPIO, 0);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}