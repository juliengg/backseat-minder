#pragma once

#include <stdint.h>

constexpr uint32_t DRIVER_SLEEP_INTERVAL_MS = 8000;

// Restore BOOT's digital GPIO after RTC wake. Returns true for BOOT wakeup.
bool driver_power_init();
bool driver_power_timer_wakeup();

// Sleeps only on a fresh, qualified NEAR decision, with no pending SMS/horn or
// held buttons. Returns if sleep is deferred or a prerequisite fails.
void driver_power_sleep_if_present(bool action_busy);
