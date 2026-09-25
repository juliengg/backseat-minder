#pragma once
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_0 = 0;
constexpr int GPIO_MODE_INPUT = 0;
constexpr int GPIO_PULLUP_ONLY = 0;
extern int test_button_level;
inline int gpio_reset_pin(int) { return 0; }
inline int gpio_set_direction(int, int) { return 0; }
inline int gpio_set_pull_mode(int, int) { return 0; }
inline int gpio_get_level(int) { return test_button_level; }
