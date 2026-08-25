// TO UPLOAD TO ESP32 RUN BELOW COMMANDS IN IDF POWERSHELL:

// 1. idf.py build
// 2. idf.py -p COM4 flash monitor


// TO SAVE TO GITHUB RUN COMMANDS IN TERMINAL:
// git add .
// git commit -m "working version"
// git push

// TO REVERT TO OTHER VERSION:
// git log --oneline
// git checkout (COMMIT ID)



#include "who_camera.h"
#include "who_human_face_detection.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "setup_mode.h"
#include "temp_humidity_sensor.h"
#include "usb_camera_stream.h"
#include "usb_telemetry.h"

#define LED_GPIO        GPIO_NUM_2
// The ESP32-S3 has a WS2812 RGB NeoPixel on GPIO 48.
// We hold it LOW so floating/spurious signals don't light it up.
#define NEOPIXEL_GPIO   GPIO_NUM_48

static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueUSBFrame = NULL;

extern "C" void app_main()
{
    setup_mode_init();  // handles NVS, button GPIO

    // Suppress the onboard NeoPixel (GPIO 48) by driving it low.
    // Without this the camera driver leaves the data line floating,
    // which can latch a bright white colour into the LED.
    gpio_reset_pin(NEOPIXEL_GPIO);
    gpio_set_direction(NEOPIXEL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NEOPIXEL_GPIO, 0);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    temp_humidity_sensor_init();
    usb_telemetry_init();

    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueUSBFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 1, xQueueAIFrame);
    register_human_face_detection(xQueueAIFrame, NULL, NULL, xQueueUSBFrame, false);
    usb_camera_stream_start(xQueueUSBFrame);

    float temperature_f = 0.0f;
    float humidity_percent = 0.0f;
    bool temperature_humidity_valid = false;
    TickType_t last_sensor_read = 0;

    while (true)
    {
        if (setup_mode_button_pressed())
            enter_setup_mode();  // blocks until confirmed

        const bool face_detected = get_face_detected();

        // AM2302/DHT22 measurements should be spaced by at least two seconds.
        if (xTaskGetTickCount() - last_sensor_read >= pdMS_TO_TICKS(3000)) {
            last_sensor_read = xTaskGetTickCount();
            temperature_humidity_valid =
                temp_humidity_sensor_read(&temperature_f, &humidity_percent);
            if (temperature_humidity_valid) {
                ESP_LOGI("app_main", "Temperature: %.1f F, Humidity: %.1f %%",
                         temperature_f, humidity_percent);
            }

            usb_telemetry_send(temperature_f, humidity_percent,
                               temperature_humidity_valid, face_detected);
        }

        gpio_set_level(LED_GPIO, face_detected ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
