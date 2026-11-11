#include <cassert>
#include <cstdio>
#include "../main/driver_power.cpp"

struct Slept {};
static int64_t now_us = 0;
static auto wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static BeaconStatus driver{true, true, true, -60};
static bool stop_ok = true;
static bool sms_pending = false;
static bool sms_during_flush = false;
static int boot_level = 1, setup_level = 1, stopped = 0, announced = 0, reinits = 0;
static uint64_t timer_us = 0;
static esp_err_t wake_error = ESP_OK;
int64_t esp_timer_get_time() { return now_us; }
void test_log(const char *, ...) {}
int gpio_get_level(int gpio) { return gpio == 0 ? boot_level : setup_level; }
int gpio_set_level(int, int level) { assert(level == 0); return ESP_OK; }
esp_err_t rtc_gpio_deinit(int gpio) { assert(gpio == 0); return ESP_OK; }
esp_err_t rtc_gpio_pullup_en(int gpio) { assert(gpio == 0); return ESP_OK; }
esp_err_t rtc_gpio_pulldown_dis(int gpio) { assert(gpio == 0); return ESP_OK; }
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return wake_cause; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { timer_us = us; return wake_error; }
esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level) { assert(gpio == 0 && level == 0); return wake_error; }
void esp_deep_sleep_start() { throw Slept{}; }
BeaconStatus beacon_tracker_status() { return driver; }
bool beacon_tracker_driver_near() { return driver.driver_present; }
bool beacon_tracker_stop() { ++stopped; return stop_ok; }
bool beacon_tracker_init(bool) { ++reinits; return true; }
void usb_telemetry_send_driver_status(const BeaconStatus &status, TelemetryPowerState power) {
    assert(status.driver_present);
    if (power == TelemetryPowerState::SleepPending) {
        assert(status.available); ++announced;
    } else assert(power == TelemetryPowerState::Active);
}
void usb_telemetry_flush() { if (sms_during_flush) sms_pending = true; }
void usb_telemetry_poll_commands() {}
bool usb_telemetry_sms_pending() { return sms_pending; }

int main() {
    assert(!driver_power_init() && !driver_power_timer_wakeup());
    driver_power_sleep_if_present(false);
    assert(stopped == 0); // USB enumeration grace.
    now_us = 2000000;
    driver_power_sleep_if_present(true);
    driver.available = false; driver_power_sleep_if_present(false);
    driver.available = true; driver.driver_present = false; driver_power_sleep_if_present(false);
    driver.driver_present = true; boot_level = 0; driver_power_sleep_if_present(false);
    boot_level = 1; setup_level = 0; driver_power_sleep_if_present(false);
    setup_level = 1;
    driver.checking = true; driver_power_sleep_if_present(false);
    driver.checking = false; sms_pending = true; driver_power_sleep_if_present(false);
    sms_pending = false;
    assert(stopped == 0 && announced == 0); // Busy, BLE failure, AWAY and buttons defer sleep.
    sms_during_flush = true;
    driver_power_sleep_if_present(false);
    assert(stopped == 0 && announced == 1 && reinits == 0); // A late SMS cancels sleep before BLE shutdown.
    sms_during_flush = sms_pending = false;
    stopped = announced = reinits = 0;
    wake_error = ESP_FAIL;
    driver_power_sleep_if_present(false);
    assert(stopped == 0 && announced == 0);
    wake_error = ESP_OK; now_us += 30000001;
    try { driver_power_sleep_if_present(false); assert(false); } catch (Slept &) {}
    assert(timer_us == 8000000 && stopped == 1 && announced == 2); // BLE before shutdown, USB after.
    wake_cause = ESP_SLEEP_WAKEUP_TIMER; now_us = 0;
    assert(!driver_power_init() && driver_power_timer_wakeup());
    assert(s_sleep_cookie == 0); // Consume only our own retained sleep marker.
    s_sleep_cookie = SLEEP_COOKIE;
    wake_cause = ESP_SLEEP_WAKEUP_EXT0;
    assert(driver_power_init() && !driver_power_timer_wakeup());
    now_us = 29999999;
    driver_power_sleep_if_present(false);
    assert(stopped == 1); // BOOT wake grants 30 seconds for the horn/setup.
    now_us = 30000001; stop_ok = false;
    driver_power_sleep_if_present(false);
    assert(stopped == 2 && reinits == 1); // Shutdown failure cannot sleep.
    driver_power_sleep_if_present(false);
    assert(stopped == 2); // Bounded retry delay.
    wake_cause = ESP_SLEEP_WAKEUP_TIMER; now_us = 0;
    assert(!driver_power_init() && !driver_power_timer_wakeup()); // Unrelated wake.
    puts("PASS: driver sleep gating, wake prerequisites, 8-second timer, RTC marker, BOOT grace and shutdown failure");
}
