#pragma once
#include "FreeRTOS.h"
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return pdPASS; }
