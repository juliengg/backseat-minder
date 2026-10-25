#pragma once

// wakeup_press preserves a BOOT press that woke the chip from deep sleep.
void boot_button_init(bool wakeup_press = false);
// One event after both press and release have been stable for 50 ms.
bool boot_button_released();
