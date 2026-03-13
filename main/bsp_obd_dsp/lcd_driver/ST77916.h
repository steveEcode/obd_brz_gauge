#pragma once
#include "esp_err.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "esp_intr_alloc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "lvgl.h"
#include "driver/ledc.h"

#include "bsp_obd_dsp/lcd_driver/esp_lcd_st77916/esp_lcd_st77916.h"
#include "bsp_obd_dsp/touch_driver/CST816.h"
#include "bsp_obd_dsp/exio/TCA9554PWR.h"


#define EXAMPLE_LCD_WIDTH                   (360)
#define EXAMPLE_LCD_HEIGHT                  (360)
#define EXAMPLE_LCD_COLOR_BITS              (16)

#define ESP_PANEL_HOST_SPI_ID_DEFAULT       (SPI2_HOST)
#define ESP_PANEL_LCD_SPI_MODE              (0)                   // 0/1/2/3, typically set to 0
#define ESP_PANEL_LCD_SPI_CLK_HZ            (80 * 1000 * 1000)    // Should be an integer divisor of 80M, typically set to 40M
#define ESP_PANEL_LCD_SPI_TRANS_QUEUE_SZ    (10)                  // Typically set to 10
#define ESP_PANEL_LCD_SPI_CMD_BITS          (32)                  // Typically set to 32
#define ESP_PANEL_LCD_SPI_PARAM_BITS        (8)                   // Typically set to 8

#define ESP_PANEL_LCD_SPI_IO_TE             (18)
#define ESP_PANEL_LCD_SPI_IO_SCK            (40)
#define ESP_PANEL_LCD_SPI_IO_DATA0          (46)
#define ESP_PANEL_LCD_SPI_IO_DATA1          (45)
#define ESP_PANEL_LCD_SPI_IO_DATA2          (42)
#define ESP_PANEL_LCD_SPI_IO_DATA3          (41)
#define ESP_PANEL_LCD_SPI_IO_CS             (21)
#define EXAMPLE_LCD_PIN_NUM_RST             (-1)    // EXIO2
#define EXAMPLE_LCD_PIN_NUM_BK_LIGHT        (5)

#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL       (1)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

#define ESP_PANEL_HOST_SPI_MAX_TRANSFER_SIZE   (2048)

#define LEDC_HS_TIMER          LEDC_TIMER_0
#define LEDC_LS_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_HS_CH0_GPIO       EXAMPLE_LCD_PIN_NUM_BK_LIGHT
#define LEDC_HS_CH0_CHANNEL    LEDC_CHANNEL_0
#define LEDC_TEST_DUTY         (4000)
#define LEDC_ResolutionRatio   LEDC_TIMER_13_BIT
#define LEDC_MAX_Duty          ((1 << LEDC_ResolutionRatio) - 1)
#define Backlight_MAX   100      

extern esp_lcd_panel_handle_t panel_handle;
extern uint8_t LCD_Backlight;

/**
 * @brief 设置 LVGL flush 完成回调 (在 LCD_Init 之前调用)
 * @param cb   回调函数指针，DMA 传输完成时调用
 * @param ctx  用户上下文 (通常为 &disp_drv)
 */
void LCD_SetFlushCallback(bool (*cb)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *), void *ctx);

void ST77916_Init();

void LCD_Init(void);                     // Call this function to initialize the screen (must be called in the main function) !!!!!
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint16_t* color);

void Backlight_Init(void);                             // Initialize the LCD backlight, which has been called in the LCD_Init function, ignore it                                                         
void Set_Backlight(uint8_t Light);                   // Call this function to adjust the brightness of the backlight. The value of the parameter Light ranges from 0 to 100
