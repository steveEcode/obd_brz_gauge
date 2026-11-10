#pragma once
#include "platform.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
void vTaskDelete(TaskHandle_t task);
