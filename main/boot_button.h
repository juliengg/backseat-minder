#pragma once

void boot_button_init();
// One event after both press and release have been stable for 50 ms.
bool boot_button_released();
