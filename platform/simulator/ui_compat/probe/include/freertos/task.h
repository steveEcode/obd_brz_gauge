#pragma once

#include "FreeRTOS.h"

/* Desktop simulator compatibility type. */
typedef void *TaskHandle_t;

static inline void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}
