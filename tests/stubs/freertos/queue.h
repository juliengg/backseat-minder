#pragma once
#include <cstring>
#include <vector>
#include "FreeRTOS.h"
struct TestQueue { std::vector<char> value; bool full = false; };
using QueueHandle_t = TestQueue *;
inline QueueHandle_t xQueueCreate(int, size_t size) { return new TestQueue{std::vector<char>(size), false}; }
inline void vQueueDelete(QueueHandle_t queue) { delete queue; }
inline int xQueueSend(QueueHandle_t queue, const void *item, TickType_t) {
    if (queue->full) return 0;
    memcpy(queue->value.data(), item, queue->value.size());
    queue->full = true;
    return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t queue, void *item, TickType_t) {
    if (!queue->full) return 0;
    memcpy(item, queue->value.data(), queue->value.size());
    queue->full = false;
    return pdTRUE;
}
