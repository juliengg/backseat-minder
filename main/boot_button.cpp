#include "boot_button.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"

namespace {
constexpr gpio_num_t BOOT_GPIO = GPIO_NUM_0;
constexpr int64_t DEBOUNCE_US = 50000;
int candidate_level = 1;
int stable_level = 1;
int64_t candidate_since = 0;
bool armed = false;
bool press_pending = false;
}

void boot_button_init()
{
    ESP_ERROR_CHECK(gpio_reset_pin(BOOT_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY));
    candidate_level = stable_level = gpio_get_level(BOOT_GPIO);
    candidate_since = esp_timer_get_time();
    armed = press_pending = false;
}

bool boot_button_released()
{
    const int level = gpio_get_level(BOOT_GPIO);
    const int64_t now = esp_timer_get_time();
    if (level != candidate_level) {
        candidate_level = level;
        candidate_since = now;
    }
    if (now - candidate_since < DEBOUNCE_US) return false;
    // A button held at startup must be released before it can trigger an SMS.
    if (!armed) {
        stable_level = candidate_level;
        armed = stable_level == 1;
        return false;
    }
    if (candidate_level != stable_level) {
        stable_level = candidate_level;
        if (stable_level == 0) {
            press_pending = true;
        } else if (press_pending) {
            press_pending = false;
            return true;
        }
    }
    return false;
}
