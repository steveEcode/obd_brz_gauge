#pragma once

#include <stdint.h>

#ifndef EXAMPLE_PIN_NUM_BK_LIGHT
#define EXAMPLE_PIN_NUM_BK_LIGHT 5
#endif
#define Backlight_MAX 100

extern uint8_t LCD_Backlight;

void Backlight_Init(void);
void Set_Backlight(uint8_t light);
