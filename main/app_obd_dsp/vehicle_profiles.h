#pragma once

#include <stdint.h>
#include "obd_data_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VEHICLE_MAX_GEARS 8

// 车辆档位传动比范围 (用于档位识别)
typedef struct {
    float min_ratio;
    float max_ratio;
    enGear gear;
} gear_ratio_range_t;

// 车辆参数配置
typedef struct {
    const char *name;                    // 显示名称 (e.g. "BRZ ZC6")
    float final_drive_ratio;             // 主减速比
    float tire_rolling_radius_m;         // 轮胎滚动半径 (m)
    uint8_t gear_count;                  // 前进挡数量 (5 or 6)
    float gear_ratios[VEHICLE_MAX_GEARS]; // 各挡传动比, index 0 unused, 1~gear_count 有效
    float gear_tolerance;                // 档位识别容差 (e.g. 0.15 = ±15%)
} vehicle_profile_t;

// 获取所有预定义的车辆配置
const vehicle_profile_t *vehicle_profile_get_all(uint8_t *count);

// 获取指定索引的车辆配置
const vehicle_profile_t *vehicle_profile_get(uint8_t index);

// 获取当前激活的车辆配置
const vehicle_profile_t *vehicle_profile_get_active(void);

// 设置激活的车辆配置 (同时保存到 NVS)
void vehicle_profile_set_active(uint8_t index);

// 计算速度常数: 1 / (final_drive * 0.377 * tire_radius)
float vehicle_profile_calc_constant(const vehicle_profile_t *p);

// 根据当前激活的车辆配置生成档位范围数组
// 返回范围数组指针, count 输出有效元素数量
const gear_ratio_range_t *vehicle_profile_get_gear_ranges(uint8_t *count);

#ifdef __cplusplus
}
#endif
