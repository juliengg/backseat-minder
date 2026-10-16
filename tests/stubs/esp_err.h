#pragma once
#include <cassert>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_INVALID_ARG = 0x102;
constexpr int ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_ERR_NO_MEM = 0x101;
inline const char *esp_err_to_name(esp_err_t) { return "TEST_ERROR"; }
#define ESP_ERROR_CHECK(expression) assert((expression) == ESP_OK)
