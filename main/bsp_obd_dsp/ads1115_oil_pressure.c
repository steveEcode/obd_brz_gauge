#include "ads1115_oil_pressure.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "app_obd_dsp/obd_data_cache.h"

#define TAG "oil_press"

// ADS1115 I2C 地址（ADDR 引脚接 GND 时为 0x48）
#define ADS1115_ADDR            0x48
#define ADS1115_REG_CONV        0x00
#define ADS1115_REG_CONFIG      0x01

#define OIL_PRESS_POLL_MS       100

// 标定参数：3.3V 供电传感器，输出 0.5~2.5V 对应 0.0~10.0 bar
// ADS1115 使用 AIN0 通道（传感器信号线接 AIN0，GND 接 GND）
#define OIL_PRESS_ADC_MIN_MV    500
#define OIL_PRESS_ADC_MAX_MV    2500
#define OIL_PRESS_MIN_BAR_X10   0
#define OIL_PRESS_MAX_BAR_X10   100

static bool s_started = false;

// 触发单次转换并读取 AIN0 电压（单位 mV）
static esp_err_t ads1115_read_ain0_mv(int32_t *out_mv)
{
    if (out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // OS=1（启动单次转换）, MUX=AIN0/GND(100), PGA=±4.096V(001),
    // MODE=single-shot(1), DR=860SPS(111), COMP 禁用(11)
    // Config = 0x8000|0x4000|0x0200|0x0100|0x00E0|0x0003 = 0xC3E3
    uint16_t cfg = 0xC3E3u;
    uint8_t cfg_bytes[2] = {(uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF)};

    esp_err_t err = I2C_Write(ADS1115_ADDR, ADS1115_REG_CONFIG, cfg_bytes, sizeof(cfg_bytes));
    if (err != ESP_OK) {
        return err;
    }

    // 860 SPS 转换时间约 1.2ms，给 3ms 余量
    vTaskDelay(pdMS_TO_TICKS(3));

    uint8_t conv[2] = {0};
    err = I2C_Read(ADS1115_ADDR, ADS1115_REG_CONV, conv, sizeof(conv));
    if (err != ESP_OK) {
        return err;
    }

    int16_t raw = (int16_t)(((uint16_t)conv[0] << 8) | conv[1]);

    // PGA=±4.096V 时，LSB = 125µV = 0.125mV → mv = raw / 8
    int32_t mv = (int32_t)raw / 8;
    if (mv < 0) {
        mv = 0;
    }

    *out_mv = mv;
    return ESP_OK;
}

static int16_t mv_to_oil_pressure_x10(int32_t mv)
{
    int32_t mv_clamped = mv;
    if (mv_clamped < OIL_PRESS_ADC_MIN_MV) {
        mv_clamped = OIL_PRESS_ADC_MIN_MV;
    }
    if (mv_clamped > OIL_PRESS_ADC_MAX_MV) {
        mv_clamped = OIL_PRESS_ADC_MAX_MV;
    }

    const int32_t in_span  = (OIL_PRESS_ADC_MAX_MV - OIL_PRESS_ADC_MIN_MV);
    const int32_t out_span = (OIL_PRESS_MAX_BAR_X10 - OIL_PRESS_MIN_BAR_X10);
    if (in_span <= 0 || out_span <= 0) {
        return OIL_PRESS_MIN_BAR_X10;
    }

    int32_t x10 = OIL_PRESS_MIN_BAR_X10 +
                  ((mv_clamped - OIL_PRESS_ADC_MIN_MV) * out_span) / in_span;

    if (x10 < OIL_PRESS_MIN_BAR_X10) {
        x10 = OIL_PRESS_MIN_BAR_X10;
    }
    if (x10 > OIL_PRESS_MAX_BAR_X10) {
        x10 = OIL_PRESS_MAX_BAR_X10;
    }

    return (int16_t)x10;
}

static void oil_pressure_task(void *arg)
{
    (void)arg;

    while (1) {
        int32_t mv = 0;
        esp_err_t err = ads1115_read_ain0_mv(&mv);
        if (err == ESP_OK) {
            int16_t pressure_x10 = mv_to_oil_pressure_x10(mv);
            obd_data_set_oil_pressure_x10(pressure_x10);
        } else {
            obd_data_set_oil_pressure_x10(-1);
        }

        vTaskDelay(pdMS_TO_TICKS(OIL_PRESS_POLL_MS));
    }
}

void oil_pressure_start(void)
{
    if (s_started) {
        return;
    }

    s_started = true;
    xTaskCreate(oil_pressure_task, "oil_press", 3072, NULL, 4, NULL);
}
