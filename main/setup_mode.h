#pragma once

void setup_mode_init();        // Call once in app_main before the loop
bool setup_mode_button_pressed(); // Returns true if button is held (debounced)
void enter_setup_mode();       // Blocks until user confirms in portal