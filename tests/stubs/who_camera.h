#pragma once
#include "freertos/queue.h"
struct camera_fb_t;
constexpr int PIXFORMAT_RGB565=0, FRAMESIZE_QVGA=0;
void register_camera(int,int,int,QueueHandle_t);
