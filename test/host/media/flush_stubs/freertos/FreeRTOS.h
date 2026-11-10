#pragma once
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
void flush_test_yield_from_isr(void);
#define portYIELD_FROM_ISR() flush_test_yield_from_isr()
