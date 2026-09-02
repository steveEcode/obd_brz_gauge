/**
 * @file
 * @brief ESP LCD touch: CST816S (I2C)
 */

#pragma once
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"

/**
 * @brief Create a new CST816S touch driver
 *
 * @note  The I2C bus must be initialized before use this function.
 *
 * @param i2c_bus I2C master bus handle
 * @param config Touch panel configuration
 * @param tp Touch panel handle
 * @return
 *      - ESP_OK: on success
 */
esp_err_t esp_lcd_touch_new_i2c_cst816(i2c_master_bus_handle_t i2c_bus, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *tp);

/**
 * @brief I2C address of the CST816S controller
 *
 */
#define ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS    (0x15)

// I2C settings
#if CONFIG_OBD_HW_VERSION_V2_NEW || CONFIG_OBD_HW_VERSION_V3_NEW
#define I2C_Touch_SDA_IO            7               /*!< GPIO number used for I2C master data  (new boards) */
#define I2C_Touch_SCL_IO            8               /*!< GPIO number used for I2C master clock (new boards) */
#define I2C_Touch_INT_IO            41              /*!< GPIO number used for I2C master data  */
#define I2C_Touch_RST_IO            40              /*!< GPIO number used for I2C master clock */
#else
#define I2C_Touch_SDA_IO            11               /*!< GPIO number used for I2C master data  */
#define I2C_Touch_SCL_IO            10               /*!< GPIO number used for I2C master clock */
#define I2C_Touch_INT_IO            4               /*!< GPIO number used for I2C master data  */
#define I2C_Touch_RST_IO            -1              /*!< GPIO number used for I2C master clock (EXIO1) */
#endif
#define I2C_Touch_MASTER_FREQ_HZ    400000          /*!< I2C master clock frequency */

extern esp_lcd_touch_handle_t tp;

void Touch_Init(void);
