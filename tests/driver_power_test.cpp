#include <cassert>
#include <cstdio>
#include "../main/driver_power.cpp"

static auto wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static unsigned restores = 0;
esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return wake_cause; }
esp_err_t rtc_gpio_deinit(int gpio) { assert(gpio == 0); ++restores; return ESP_OK; }

int main() {
    assert(!driver_power_init());
    wake_cause = ESP_SLEEP_WAKEUP_TIMER;
    assert(!driver_power_init());
    wake_cause = ESP_SLEEP_WAKEUP_EXT0;
    assert(driver_power_init());
    assert(restores == 3);
    puts("PASS: continuous monitoring boot initialization and legacy RTC GPIO restoration");
}
