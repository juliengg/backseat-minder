#pragma once
void test_log(const char *, ...);
#define ESP_LOGI(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
#define ESP_LOGE(tag, ...) ((void)(tag), test_log(__VA_ARGS__))
