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
    GEAR_7,   // 保时捷 997.2 PDK 等 7 速
    GEAR_8,   // BMW ZF 8HP 等 8 速
} enGear;

typedef enum {
    BRAKE_RS485_IDLE = 0,
    BRAKE_RS485_PROBE,
    BRAKE_RS485_OK,
    BRAKE_RS485_TIMEOUT,
    BRAKE_RS485_PARSE_FAIL,
} brake_rs485_status_t;

void obd_data_set_rpm(uint16_t rpm);
void obd_data_set_speed(uint8_t kmh);
void obd_data_set_coolant_temp(int16_t temp);
void obd_data_set_oil_temp(int16_t temp);   // 真实机油温度 °C (SSM 22 10 17, A-40)
void obd_data_set_intake_temp(int16_t temp);
void obd_data_set_load_pct(int16_t pct);    // 发动机负荷 0~100%
void obd_data_set_tps(int16_t pct);         // 节气门开度 0~100%
void obd_data_set_bat_mv(int32_t mv);        // 电池电压 mV (e.g. 12000 = 12.0V)
void obd_data_set_oil_pressure_x10(int16_t pressure_x10); // 油压, 0.1bar, -1=无效
void obd_data_set_boost_x10(int16_t boost_x10); // 涡轮表压, 0.1bar(可为负), -32768=无效
void obd_data_set_brake_temp_x10(int16_t temp_x10); // 刹车温度, 0.1°C
void obd_data_set_gear(int8_t gear);               // 直接档位: -1=R, 0=N, 1+=前进挡, 127=无效
void obd_data_set_brake_rs485_status(brake_rs485_status_t status);
uint16_t obd_data_get_rpm(void);
uint8_t  obd_data_get_speed(void);
int16_t  obd_data_get_coolant_temp(void);
int16_t  obd_data_get_oil_temp(void);       // -100 = 无效
int16_t  obd_data_get_intake_temp(void);
int16_t  obd_data_get_load_pct(void);       // -1 = 无效
int16_t  obd_data_get_tps(void);            // -1 = 无效
int32_t  obd_data_get_bat_mv(void);         // -1 = 无效
int16_t  obd_data_get_oil_pressure_x10(void); // -1 = 无效
int16_t  obd_data_get_boost_x10(void); // -32768 = 无效
int16_t  obd_data_get_brake_temp_x10(void); // -1000 = 无效
int8_t   obd_data_get_gear(void);            // 127 = 无效 (用计算档位回退)
brake_rs485_status_t obd_data_get_brake_rs485_status(void);
enGear calculate_gear(float rpm, float speed);
void vMileageDataStatisticTask(void);

// ---- Dirty flags: UI 按需刷新, 跳过未变化的通道 ----
#define OBD_DIRTY_RPM          (1u << 0)
#define OBD_DIRTY_SPEED        (1u << 1)
#define OBD_DIRTY_COOLANT      (1u << 2)
#define OBD_DIRTY_OIL_TEMP     (1u << 3)
#define OBD_DIRTY_INTAKE       (1u << 4)
#define OBD_DIRTY_LOAD         (1u << 5)
#define OBD_DIRTY_TPS          (1u << 6)
#define OBD_DIRTY_BAT          (1u << 7)
#define OBD_DIRTY_OIL_PRESS    (1u << 8)
#define OBD_DIRTY_BOOST        (1u << 9)
#define OBD_DIRTY_BRAKE_TEMP   (1u << 10)
#define OBD_DIRTY_GEAR         (1u << 11)
#define OBD_DIRTY_ALL          0xFFFFu

// 读取并清除 dirty flags (原子操作)
uint32_t obd_data_take_dirty(void);
// 标记全部 dirty (初始/重连时)
void obd_data_mark_all_dirty(void);
#ifdef __cplusplus
}
#endif
