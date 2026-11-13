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
#include "driver/gpio.h"
#include "boot_button.h"
#include "beacon_tracker.h"
#include "driver_power.h"
#include "alert_runtime.h"
#include "cellular_link.h"
#include "cellular_diagnostics.h"
#include "cellular_protocol.h"
#include <cstdio>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mmwave_sensor.h"
#include "setup_mode.h"
#include "temp_humidity_sensor.h"
#include "thermal_camera.h"
#include "tuned_face_detection.h"
#include "usb_camera_stream.h"
#include "usb_telemetry.h"

#define LED_GPIO        GPIO_NUM_2
// The ESP32-S3 has a WS2812 RGB NeoPixel on GPIO 48.
// We hold it LOW so floating/spurious signals don't light it up.
#define NEOPIXEL_GPIO   GPIO_NUM_48

static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueUSBFrame = NULL;

namespace {

constexpr TickType_t THERMAL_FRAME_INTERVAL = pdMS_TO_TICKS(250); // 4 FPS

void thermal_stream_task(void *)
{
    TickType_t next_frame_time = xTaskGetTickCount();
    while (true) {
        size_t csv_length = 0;
        const char *csv = thermal_camera_read_csv(&csv_length);
        if (csv) {
            usb_telemetry_send_thermal_frame(csv, csv_length);
            uint8_t x = 0;
            uint8_t y = 0;
            uint8_t width = 0;
            uint8_t height = 0;
            const bool detected = thermal_camera_get_heat_trace_bounds(
                &x, &y, &width, &height);
            usb_telemetry_send_heat_trace_bounds(detected, x, y, width, height);
        }
        xTaskDelayUntil(&next_frame_time, THERMAL_FRAME_INTERVAL);
    }
}

void thermal_stream_start()
{
    if (xTaskCreatePinnedToCore(thermal_stream_task, "thermal_stream", 6144,
                                nullptr, 3, nullptr, 0) != pdPASS) {
        ESP_LOGW("app_main", "Could not start thermal USB stream task");
    }
}

} // namespace

extern "C" void app_main()
{
    const bool boot_wakeup = driver_power_init();
    setup_mode_init();  // handles NVS, button GPIO
    boot_button_init(boot_wakeup);
    usb_telemetry_init();
    alert_runtime_init();
    const bool suppressed_boot = alert_runtime_suppressed();
    const bool beacon_ready = beacon_tracker_init();
    if(suppressed_boot) {
        cellular_link_init();
        cellular_link_cancel_alert();
    }
    if (!beacon_ready) ESP_LOGW("app_main", "Beacon unavailable; monitoring stays active");

    // Suppress the onboard NeoPixel (GPIO 48) by driving it low.
    // Without this the camera driver leaves the data line floating,
    // which can latch a bright white colour into the LED.
    gpio_reset_pin(NEOPIXEL_GPIO);
    gpio_set_direction(NEOPIXEL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(NEOPIXEL_GPIO, 0);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);

    bool setup_button_held = false;
    const esp_err_t cellular_status = cellular_link_init();
    if (cellular_status != ESP_OK) {
        ESP_LOGW("app_main", "Cellular link unavailable: %s", esp_err_to_name(cellular_status));
        cellular_diagnostics_record("LINK_UNAVAILABLE");
    }

    temp_humidity_sensor_init();
    mmwave_sensor_init();
    const bool thermal_camera_ready = thermal_camera_init();

    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueUSBFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 1, xQueueAIFrame);
    register_tuned_face_detection(xQueueAIFrame, xQueueUSBFrame, false);
    usb_camera_stream_start(xQueueUSBFrame);
    if (thermal_camera_ready) {
        thermal_stream_start();
    }

    float temperature_f = 0.0f;
    float humidity_percent = 0.0f;
    bool temperature_humidity_valid = false;
    bool face_detected_since_telemetry = false;
    TickType_t last_sensor_read = 0;
    TickType_t last_stack_report = xTaskGetTickCount();
    TickType_t last_driver_report = 0;

    while (true)
    {
        if (boot_button_released()) {
            cellular_link_send_horn();
        }

        if (usb_telemetry_sms_requested()) {
            char phone[32];
            if (setup_mode_get_phone_number(phone, sizeof(phone))) {
                char name[64];
                char message[cellular::MAX_MESSAGE + 1];
                if (!setup_mode_get_name(name, sizeof(name))) {
                    ESP_LOGW("app_main", "Save a valid name in setup before sending the dashboard test");
                    cellular_diagnostics_record("NO_NAME");
                } else {
                    const int length = snprintf(message, sizeof(message),
                        "Backseat Minder test for %s. This checks delivery of passenger alerts. No action is needed.",
                        name);
                    if (length < 0 || static_cast<size_t>(length) >= sizeof(message) ||
                        !cellular::valid_message(message)) {
                        ESP_LOGW("app_main", "Saved name cannot be used in the dashboard test; update it in setup");
                        cellular_diagnostics_record("INVALID_NAME");
                    } else {
                        cellular_link_send_sms(phone, message);
                    }
                }
            } else {
                ESP_LOGW("app_main", "No valid phone number configured; save a number in setup mode before sending SMS");
                cellular_diagnostics_record("NO_PHONE");
            }
        }

        const bool setup_pressed = setup_mode_button_pressed();
        if (setup_pressed && !setup_button_held) {
            if (beacon_tracker_driver_near()) {
                // A nearby driver can configure the device while still detected.
                // The cellular worker continues cancellation during setup.
                alert_runtime_cancel();
                ESP_LOGI("app_main", "Setup button pressed; driver nearby, alert cancelled for configuration");
                enter_setup_mode();
            } else {
                ESP_LOGW("app_main", "Setup requires confirmed driver presence; bring the beacon nearby and press again");
            }
        }
        setup_button_held = setup_pressed;

        const BeaconStatus driver = beacon_tracker_status();
        if (xTaskGetTickCount() - last_driver_report >= pdMS_TO_TICKS(500)) {
            usb_telemetry_send_driver_status(driver, TelemetryPowerState::Active);
            last_driver_report = xTaskGetTickCount();
        }

        // ESP-WHO exposes face detection as a one-shot flag. Preserve any
        // detection until the next telemetry sample instead of losing it
        // between the main loop's polls and telemetry's 3 second polls.
        face_detected_since_telemetry |= get_tuned_face_detected();
        const bool mmwave_presence_detected = mmwave_sensor_presence_detected();
        const bool thermal_heat_detected = thermal_camera_heat_trace_detected();
        const bool human_presence_detected = face_detected_since_telemetry ||
                                             mmwave_presence_detected ||
                                             thermal_heat_detected;
        alert_runtime_poll(human_presence_detected,driver);
        gpio_set_level(LED_GPIO, human_presence_detected ? 1 : 0);

        // AM2302/DHT22 measurements should be spaced by at least two seconds.
        if (xTaskGetTickCount() - last_sensor_read >= pdMS_TO_TICKS(3000)) {
            last_sensor_read = xTaskGetTickCount();
            temperature_humidity_valid =
                temp_humidity_sensor_read(&temperature_f, &humidity_percent);
            if (temperature_humidity_valid) {
                ESP_LOGI("app_main", "Temperature: %.1f F, Humidity: %.1f %%",
                         temperature_f, humidity_percent);
            }

            alert_runtime_sample(temperature_f,temperature_humidity_valid);
            alert_runtime_poll(human_presence_detected,driver);
            const auto &workflow=alert_runtime_workflow();
            const char *alert_state=workflow.state==alert::State::Active?"active":
                workflow.state==alert::State::Suppressed?"suppressed":"armed";
            usb_telemetry_set_alert(alert_state,
                workflow.elapsed(static_cast<uint64_t>(esp_timer_get_time()/1000)),
                workflow.forecast_valid,workflow.slope_f_per_min,workflow.forecast_f);
            usb_telemetry_send(temperature_f, humidity_percent,
                               temperature_humidity_valid,
                               face_detected_since_telemetry,
                               mmwave_presence_detected,
                               thermal_heat_detected,
                               human_presence_detected, driver);

            face_detected_since_telemetry = false;
        }

        if (xTaskGetTickCount() - last_stack_report >= pdMS_TO_TICKS(60000)) {
            // ESP-IDF reports the minimum unused task stack in bytes.
            ESP_LOGI("app_main", "Main task minimum free stack: %u bytes",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            last_stack_report = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // Keep USB commands and setup responsive.
    }
}
