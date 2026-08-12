// Common board-level configuration header
// Targets the Waveshare ESP32-S3-Touch-LCD-1.85 development board
#ifndef BSP_BOARD_H_
#define BSP_BOARD_H_

#include "driver/gpio.h"
#include "nvs_storage.h"

// LCD backlight GPIO and level definitions (avoid duplication with ST77916.h)
#ifndef EXAMPLE_LCD_PIN_NUM_BK_LIGHT
#define EXAMPLE_PIN_NUM_BK_LIGHT       GPIO_NUM_5
#else
#define EXAMPLE_PIN_NUM_BK_LIGHT       EXAMPLE_LCD_PIN_NUM_BK_LIGHT
#endif

#ifndef EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#endif

#ifndef EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL (!EXAMPLE_LCD_BK_LIGHT_ON_LEVEL)
#endif

#endif /* BSP_BOARD_H_ */
