#pragma once

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore,
    TickType_t timeout
)
{
    (void)semaphore;
    (void)timeout;
    return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(
    SemaphoreHandle_t semaphore
)
{
    (void)semaphore;
    return pdTRUE;
}
