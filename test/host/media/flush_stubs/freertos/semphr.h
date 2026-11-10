#pragma once
#include <pthread.h>
#include "FreeRTOS.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t event;
    pthread_cond_t blocked_event;
    int token;
    int blocked;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken);
