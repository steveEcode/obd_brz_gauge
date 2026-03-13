#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 档位枚举
typedef enum {
    GEAR_NEUTRAL, // 空档或无法识别
    GEAR_1,
    GEAR_2,
    GEAR_3, 
    GEAR_4,
    GEAR_5,
    GEAR_6,
} enGear;

void obd_data_set_rpm(uint16_t rpm);
void obd_data_set_speed(uint8_t kmh);
void obd_data_set_coolant_temp(int16_t temp);
void obd_data_set_oil_temp(int16_t temp);   // 真实机油温度 °C (SSM 22 10 17, A-40)
void obd_data_set_intake_temp(int16_t temp);
void obd_data_set_load_pct(int16_t pct);    // 发动机负荷 0~100%
void obd_data_set_tps(int16_t pct);         // 节气门开度 0~100%
void obd_data_set_bat_mv(int32_t mv);        // 电池电压 mV (e.g. 12000 = 12.0V)
uint16_t obd_data_get_rpm(void);
uint8_t  obd_data_get_speed(void);
int16_t  obd_data_get_coolant_temp(void);
int16_t  obd_data_get_oil_temp(void);       // -100 = 无效
int16_t  obd_data_get_intake_temp(void);
int16_t  obd_data_get_load_pct(void);       // -1 = 无效
int16_t  obd_data_get_tps(void);            // -1 = 无效
int32_t  obd_data_get_bat_mv(void);         // -1 = 无效
enGear calculate_gear(float rpm, float speed);
void vMileageDataStatisticTask(void);
#ifdef __cplusplus
}
#endif
