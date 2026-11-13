#include "driver_power.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

bool driver_power_init()
{
    // Restore digital GPIO if updating a board previously using RTC wakeup.
    const bool button_wakeup = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    rtc_gpio_deinit(GPIO_NUM_0);
    return button_wakeup;
}
