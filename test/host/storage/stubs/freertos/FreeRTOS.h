#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define portTICK_PERIOD_MS 10u
#define tskIDLE_PRIORITY 0
/* Deterministic 100 Hz ticks; not an ESP32 RTOS simulation. */
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) / 10u))
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
