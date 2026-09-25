#pragma once

// Placeholder driver detector. Cold boot and BOOT wake start driver-absent.
void driver_presence_init();

// Poll from the main task. A debounced BOOT press/release marks driver-present.
// Replace this input with the real detector when it is available.
bool driver_presence_is_present();

// Wake on the next BOOT press. A future detector also needs a hardware wake
// source: software cannot run while the ESP32 is in deep sleep.
[[noreturn]] void driver_presence_sleep();
