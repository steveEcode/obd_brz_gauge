#pragma once

#include <stdint.h>
#include "obd_data_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define VEHICLE_MAX_GEARS 9   // 容纳 index0 + 最多 8 个前进挡 (如 ZF 8HP)

// 油温查询模式优先级
typedef enum {
    OIL_TEMP_MODE_NONE = 0xFF,
    OIL_TEMP_MODE_PID_5C = 0,       // 标准 PID 01 5C
    OIL_TEMP_MODE_UDS_22_10_17 = 1, // UDS Mode 22 10 17
    OIL_TEMP_MODE_TOYOTA_21_01 = 2, // Toyota Mode 21 01
    OIL_TEMP_MODE_MAZDA_22_111F = 3, // Mazda Skyactiv Mode 22 PID 111F, 单字节 A-50
    OIL_TEMP_MODE_MAZDA_22_1310 = 4, // Mazda Skyactiv Mode 22 PID 1310, 双字节 (A*256+B)/100-40
    OIL_TEMP_MODE_PORSCHE_CAN_441 = 5, // 保时捷 997.2/987.2 CAN 广播帧 0x441 (byte5油温 x-60, byte6油压)
    OIL_TEMP_MODE_MINI_22_5822 = 6,    // MINI/BMW Mode 22 PID 5822, 单字节 °C = A-60 (°F=A*9/5-76)
    OIL_TEMP_MODE_BMW_22_4402 = 7,     // BMW F系(F48等, B38/B48) Mode 22 PID 4402, 第二字节 °C = B-64
    OIL_TEMP_MODE_BMW_22_03F3 = 8,     // BMW G系 Mode 22 PID 03F3, 单字节 °C = A-40
    OIL_TEMP_MODE_BMW_G_22_4402 = 9,   // BMW G系 Mode 22 PID 4402, 双字节 °C = (A*256+B)*191.25/255-48
    OIL_TEMP_MODE_BMW_22_D002 = 10,    // BMW G系 Mode 22 PID D002, 双字节 °C = (A*256+B)*191.25/255-48
    OIL_TEMP_MODE_BMW_22_111F = 11,    // BMW Mode 22 PID 111F (Header 7E0), 单字节 °C = A-50
} oil_temp_query_mode_t;

// 车辆档位传动比范围 (用于档位识别)
typedef struct {
    float min_ratio;
    float max_ratio;
    enGear gear;
} gear_ratio_range_t;

// 油温查询策略
typedef struct {
    oil_temp_query_mode_t primary;     // 首选查询模式
    oil_temp_query_mode_t secondary;   // 备用1
    oil_temp_query_mode_t tertiary;    // 备用2
    oil_temp_query_mode_t quaternary;  // 备用3（最终兜底）
    uint16_t offset_c;                 // 温度偏移量，单位 0.1°C（有符号）
    // CAN-441(保时捷)油温线性公式: °C = raw * can_num / can_den + can_off
    // 仅 OIL_TEMP_MODE_PORSCHE_CAN_441 模式使用; can_den=0 时回退内置默认(x-60)。
    // 不同代只改这三个系数即可: 997.2/987.2 = 1/1/-60; 997.1/987.1 = 3/4/-48。
    int16_t can_num;
    int16_t can_den;
    int16_t can_off;
} oil_temp_strategy_t;

// 车辆参数配置
typedef struct {
    const char *name;                    // 显示名称 (e.g. "BRZ ZC6")
    float final_drive_ratio;             // 主减速比
    float tire_rolling_radius_m;         // 轮胎滚动半径 (m)
    uint8_t gear_count;                  // 前进挡数量 (5 or 6)
    float gear_ratios[VEHICLE_MAX_GEARS]; // 各挡传动比, index 0 unused, 1~gear_count 有效
    float gear_tolerance;                // 档位识别容差 (e.g. 0.15 = ±15%)
    oil_temp_strategy_t oil_temp_strategy; // 油温查询策略
    bool has_boost;                      // 是否有涡轮增压(决定是否查询/显示涡轮压力)
    uint8_t forced_protocol;             // 强制 ELM327 协议号(ATSP), 0=自动探测; BMW 等自动探测不稳的车锁 6
    bool obd_functional_addr;            // true=标准PID用功能寻址(ATSH 7DF, 同手机APP); false=物理寻址(ATSH 7E0, 斯巴鲁等)
    float speed_scale;                   // 车速校正系数(读取值×此系数), 0 或未设=1.0(不校正)
    uint8_t obd_timeout;                 // ATST 超时值(ELM327 单位, 0=默认 0x19); BMW G CAN 快速响应可设 0x0F 减少 NO DATA 等待
    uint8_t poll_gap_ms;                 // 轮询槽间隔(ms), 0=使用默认 OBD_POLL_SLOT_GAP_MS(30ms)
    bool can_broadcast_mode;             // true=通过 ATMA 监听 CAN 广播帧读取数据(FT86 0x140/360/D1/141), 替代标准 OBD PID 轮询
} vehicle_profile_t;

// 获取所有预定义的车辆配置
const vehicle_profile_t *vehicle_profile_get_all(uint8_t *count);

// 获取指定索引的车辆配置
const vehicle_profile_t *vehicle_profile_get(uint8_t index);

// 获取当前激活的车辆配置
const vehicle_profile_t *vehicle_profile_get_active(void);

// 设置激活的车辆配置 (同时保存到 NVS)
void vehicle_profile_set_active(uint8_t index);

// 计算总传动比常数: 1 / (0.377 * tire_radius)
float vehicle_profile_calc_constant(const vehicle_profile_t *p);

// 根据当前激活的车辆配置生成档位范围数组
// 返回范围数组指针, count 输出有效元素数量
const gear_ratio_range_t *vehicle_profile_get_gear_ranges(uint8_t *count);

// 获取当前激活车型的油温查询策略
const oil_temp_strategy_t *vehicle_profile_get_oil_temp_strategy(void);

#ifdef __cplusplus
}
#endif
