#pragma once
#include "FreeRTOS.h"
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return pdPASS; }

TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);
void xTaskDelayUntil(TickType_t *, TickType_t);
inline int xTaskCreatePinnedToCore(void (*)(void *),const char *,int,void *,int,void *,int) {return pdPASS;}
inline unsigned uxTaskGetStackHighWaterMark(void *) {return 8192;}
