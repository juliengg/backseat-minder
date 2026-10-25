#pragma once
using gpio_num_t = int;
constexpr gpio_num_t GPIO_NUM_0 = 0, GPIO_NUM_2 = 2, GPIO_NUM_38 = 38, GPIO_NUM_48 = 48;
int gpio_get_level(gpio_num_t);
int gpio_set_level(gpio_num_t, int);
