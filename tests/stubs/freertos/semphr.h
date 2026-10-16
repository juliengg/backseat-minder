#pragma once
#include "FreeRTOS.h"
using SemaphoreHandle_t = std::mutex *;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) { mutex->lock(); return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
