#include "driver_presence.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

namespace {
constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_0;
constexpr int64_t DEBOUNCE_US = 50000;
constexpr const char *TAG = "driver_presence";
int candidate_level = 1;
int stable_level = 1;
int64_t candidate_since = 0;
bool armed = false;
bool press_pending = false;
bool driver_present = false;
} // namespace

void driver_presence_init()
{
    // EXT0 leaves its pad configured as RTC IO after waking.
    ESP_ERROR_CHECK(rtc_gpio_deinit(BOOT_GPIO));
    ESP_ERROR_CHECK(gpio_reset_pin(BOOT_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY));
    candidate_level = stable_level = gpio_get_level(BOOT_GPIO);
    candidate_since = esp_timer_get_time();
    armed = false;
    press_pending = false;
    driver_present = false;
    ESP_LOGI(TAG, "Driver absent; monitoring enabled%s",
             esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0
                 ? " (BOOT wake)" : "");
}

bool driver_presence_is_present()
{
    const int level = gpio_get_level(BOOT_GPIO);
    const int64_t now = esp_timer_get_time();
    if (level != candidate_level) {
        candidate_level = level;
        candidate_since = now;
    }
    if (now - candidate_since < DEBOUNCE_US) {
        return driver_present;
    }
    // Ignore the wake-up press (including a held button) until released.
    if (!armed) {
        stable_level = candidate_level;
        armed = stable_level == 1;
        return driver_present;
    }
    if (candidate_level != stable_level) {
        stable_level = candidate_level;
        if (stable_level == 0) {
            press_pending = true;
        } else if (press_pending) {
            press_pending = false;
            driver_present = true;
        }
    }
    return driver_present;
}

[[noreturn]] void driver_presence_sleep()
{
    // Sleep after release so the same press cannot immediately wake us.
    // EXT0 keeps RTC peripherals powered, including this internal pull-up.
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_GPIO, 0));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_GPIO));
    ESP_LOGI(TAG, "Driver present; entering deep sleep. Press BOOT to resume monitoring.");
    esp_deep_sleep_start();
}
