#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Gear enum
typedef enum {
    GEAR_NEUTRAL, // Neutral or unrecognized
    GEAR_1,
    GEAR_2,
    GEAR_3,
    GEAR_4,
    GEAR_5,
    GEAR_6,
    GEAR_7,   // 7-speed such as Porsche 997.2 PDK
    GEAR_8,   // 8-speed such as BMW ZF 8HP
} enGear;

typedef enum {
    BRAKE_RS485_IDLE = 0,
    BRAKE_RS485_PROBE,
    BRAKE_RS485_OK,
    BRAKE_RS485_TIMEOUT,
    BRAKE_RS485_PARSE_FAIL,
} brake_rs485_status_t;

void obd_data_set_rpm(uint16_t rpm);
// RPM override layer: for multi-gauge linkage tests. When enabled, get_rpm returns val; disabling restores the real value.
void obd_data_rpm_override_set(bool en, uint16_t val);
void obd_data_set_speed(uint8_t kmh);
void obd_data_set_coolant_temp(int16_t temp);
void obd_data_set_oil_temp(int16_t temp);   // actual oil temp °C (SSM 22 10 17, A-40)
void obd_data_set_intake_temp(int16_t temp);
void obd_data_set_load_pct(int16_t pct);    // engine load 0~100%
void obd_data_set_tps(int16_t pct);         // throttle opening 0~100%
void obd_data_set_bat_mv(int32_t mv);        // battery voltage mV (e.g. 12000 = 12.0V)
void obd_data_set_oil_pressure_x10(int16_t pressure_x10); // oil pressure, 0.1bar, -1=invalid
void obd_data_set_boost_x10(int16_t boost_x10); // boost gauge pressure, 0.1bar (can be negative), -32768=invalid
void obd_data_set_brake_temp_x10(int16_t temp_x10); // brake temp, 0.1°C
void obd_data_set_gear(int8_t gear);               // direct gear value: -1=R, 0=N, 1+=forward gear, 127=invalid
void obd_data_set_brake_rs485_status(brake_rs485_status_t status);
void obd_data_set_afr_x100(int16_t afr_x100);      // air-fuel ratio AFR, ×100 (1470=14.7:1), -1=invalid
uint16_t obd_data_get_rpm(void);
uint8_t  obd_data_get_speed(void);
int16_t  obd_data_get_coolant_temp(void);
int16_t  obd_data_get_oil_temp(void);       // -100 = invalid
int16_t  obd_data_get_intake_temp(void);
int16_t  obd_data_get_load_pct(void);       // -1 = invalid
int16_t  obd_data_get_tps(void);            // -1 = invalid
int32_t  obd_data_get_bat_mv(void);         // -1 = invalid
int16_t  obd_data_get_oil_pressure_x10(void); // -1 = invalid
int16_t  obd_data_get_boost_x10(void); // -32768 = invalid
int16_t  obd_data_get_brake_temp_x10(void); // -1000 = invalid
int8_t   obd_data_get_gear(void);            // 127 = invalid (falls back to the computed gear)
int16_t  obd_data_get_afr_x100(void);        // -1 = invalid
enGear calculate_gear(float rpm, float speed);
void vMileageDataStatisticTask(void);

#ifdef __cplusplus
}
#endif
