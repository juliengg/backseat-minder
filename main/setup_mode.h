#pragma once

#include <stddef.h>

void setup_mode_init();        // Call once in app_main before the loop
bool setup_mode_button_pressed(); // Returns true if button is held (debounced)
void enter_setup_mode();       // Blocks until user confirms in portal
// Reads only the saved primary number. Removes common display formatting;
// returns false (and clears the output) if missing, malformed or too long.
bool setup_mode_get_phone_number(char *buffer, size_t capacity);
// Reads the saved name; false if missing, empty, or the buffer is too small.
bool setup_mode_get_name(char *buffer, size_t capacity);
