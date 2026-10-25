#include "driver_power.h"
#include "beacon_tracker.h"
#include "usb_telemetry.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

namespace {
const char *TAG = "driver_power";
constexpr int64_t BUTTON_AWAKE_US = 30000000;
constexpr uint32_t SLEEP_COOKIE = 0x42534d50;
RTC_DATA_ATTR uint32_t s_sleep_cookie = 0;
bool s_timer_wakeup = false;
int64_t s_sleep_after = 0;

bool buttons_held()
{
    return gpio_get_level(GPIO_NUM_0) == 0 || gpio_get_level(GPIO_NUM_38) == 0;
}
} // namespace

bool driver_power_init()
{
    const auto cause = esp_sleep_get_wakeup_cause();
    const bool own_wakeup = s_sleep_cookie == SLEEP_COOKIE;
    s_sleep_cookie = 0;
    s_timer_wakeup = own_wakeup && cause == ESP_SLEEP_WAKEUP_TIMER;
    const bool button_wakeup = own_wakeup && cause == ESP_SLEEP_WAKEUP_EXT0;
    // GPIO0 was routed to RTC IO for ext0 wakeup; normal button code uses GPIO.
    rtc_gpio_deinit(GPIO_NUM_0);
    // Allow USB enumeration and status delivery even on short timer probes.
    s_sleep_after = esp_timer_get_time() + (button_wakeup ? BUTTON_AWAKE_US : 1500000);
    return button_wakeup;
}

bool driver_power_timer_wakeup() { return s_timer_wakeup; }

void driver_power_sleep_if_present(bool action_busy)
{
    usb_telemetry_poll_commands();
    if (action_busy || usb_telemetry_sms_pending() ||
        esp_timer_get_time() < s_sleep_after || buttons_held()) return;
    const auto status = beacon_tracker_status();
    if (!status.available || !status.driver_present || status.checking) return;

    // BLE cannot run in deep sleep. Restart into a BLE-only probe after 8 s.
    esp_err_t err = esp_sleep_enable_timer_wakeup(
        static_cast<uint64_t>(DRIVER_SLEEP_INTERVAL_MS) * 1000);
    if (err == ESP_OK) err = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (err == ESP_OK) err = rtc_gpio_pullup_en(GPIO_NUM_0);
    if (err == ESP_OK) err = rtc_gpio_pulldown_dis(GPIO_NUM_0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wake configuration failed: %s; staying active", esp_err_to_name(err));
        s_sleep_after = esp_timer_get_time() + BUTTON_AWAKE_US;
        return;
    }
    usb_telemetry_poll_commands();
    if (usb_telemetry_sms_pending() || buttons_held() || !beacon_tracker_driver_near()) return;
    if (!beacon_tracker_stop()) {
        ESP_LOGE(TAG, "BLE shutdown failed; staying active");
        beacon_tracker_init();
        s_sleep_after = esp_timer_get_time() + BUTTON_AWAKE_US;
        return;
    }
    // Announce only after BLE shutdown succeeds. The viewer retains this report
    // while native USB is off and while subsequent timer probes are checking.
    usb_telemetry_send_driver_status(status, TelemetryPowerState::SleepPending);
    usb_telemetry_flush();
    usb_telemetry_poll_commands();
    if (usb_telemetry_sms_pending() || buttons_held()) {
        beacon_tracker_init(true);
        usb_telemetry_send_driver_status(beacon_tracker_status(), TelemetryPowerState::Active);
        return;
    }
    gpio_set_level(GPIO_NUM_2, 0);
    gpio_set_level(GPIO_NUM_48, 0);
    s_sleep_cookie = SLEEP_COOKIE;
    ESP_LOGI(TAG, "Driver present: deep sleep; beacon recheck in %u seconds",
             static_cast<unsigned>(DRIVER_SLEEP_INTERVAL_MS / 1000));
    esp_deep_sleep_start();
}
