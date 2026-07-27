#include "vehicle_profiles.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <string.h>

#define TAG "vehicle_profile"

// 预定义车辆配置
static const vehicle_profile_t s_profiles[] = {
    {
        // 通用 OBD2 标准配置 (SAE J1979 / ISO 15031-5)
        // 仅使用标准 PID，不依赖任何厂商私有协议:
        //   转速 010C, 车速 010D, 水温 0105, 油温 015C, 进气 010F, 负荷 0104, TPS 0111, 电压 0142
        // 档位比为常见 6MT 占位(仅影响档位识别精度, 不影响数据读取)。
        .name = "OBD2 Generic",
        .final_drive_ratio = 3.500f,       // 通用占位
        .tire_rolling_radius_m = 0.315f,   // 205/55R16 常见
        .gear_count = 6,
        .gear_ratios = {0, 3.500f, 2.000f, 1.400f, 1.100f, 0.900f, 0.750f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // 标准 OBD2 油温 PID (°C = A - 40)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        .has_boost = false,                // 通用默认自吸; 涡轮车标准 010B 仍可读(需手动开启)
    },
    {
        // BRZ ZC6 CAN (2013-2020, FA20 自吸, Gen1)
        // ATMA 监听: 0x140(100Hz 转速+节气门), 0x360(20Hz 油温+水温)
        // 其余走标准 OBD PID
        // 参考: https://github.com/timurrrr/ft86/blob/main/can_bus/gen1.md
        .name = "ZC6 CAN",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_TOYOTA_21_01,
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        .forced_protocol = 6,
        .can_broadcast_mode = true,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZD8 CAN (2022+, FA24 自吸, Gen2)
        // ATMA 监听: 0x40(100Hz 转速+节气门), 0x345(10Hz 油温+水温)
        // 其余走标准 OBD PID
        // 参考: https://github.com/timurrrr/ft86/blob/main/can_bus/gen2.md
        .name = "ZD8 CAN",
        .final_drive_ratio = 3.700f,       // ZD8 主减速比
        .tire_rolling_radius_m = 0.318f,   // 225/40R18
        .gear_count = 6,
        .gear_ratios = {0, 3.765f, 2.476f, 1.633f, 1.190f, 0.932f, 0.751f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ZD8 用标准 PID 5C
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        .forced_protocol = 6,
        .can_broadcast_mode = true,
        .poll_gap_ms = 1,
    },
    {
        // Toyota GT86 / 86 ZN6 (2012-2021, FA20 自吸)
        // Toyota ECU 的 Mode 21 01 响应只有 31 字节，布局与 Subaru ECU(ZC6 d[33]) 不同：
        // 水温在 d[9]，油温在 d[27](≈86°C when coolant=91°C，实测稳定).
        .name = "GT86 ZN6",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17 (原厂同 BRZ ZC6)
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_TOYOTA_21_01,
            .secondary = OIL_TEMP_MODE_PID_5C,        // gt96 等适配器可能不支持 Mode 21，回退标准 PID
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
    },
    {
        .name = "MX-5 ND",
        .final_drive_ratio = 2.866f,       // ND 6MT（所有手动一致，自动为 3.583）
        .tire_rolling_radius_m = 0.300f,   // 195/50R16
        .gear_count = 6,
        .gear_ratios = {0, 5.087f, 2.991f, 2.035f, 1.594f, 1.286f, 1.000f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // 先试 PID 1310(双字节)，连续失败则回退到 111F(单字节)——两种已知 Mazda 油温 PID 都覆盖
            .primary = OIL_TEMP_MODE_MAZDA_22_1310,
            .secondary = OIL_TEMP_MODE_MAZDA_22_111F,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        // .has_boost 默认 false (自吸)
        .obd_timeout = 0x0A,  // 40ms 超时; Mazda CAN 通常 5-15ms 响应, 减少 NO DATA 等待
        .poll_gap_ms = 1,     // 槽间最小间隔 1ms(0=跳过 vTaskDelay, 1ms 让调度器有机会切换)
    },
    {
        // BMW G系 (G20/G21/G22等, B48/B58 涡轮, ZF 8HP)
        // 标准 OBD 只响应 7DF 功能寻址，7E0 物理寻址无响应; 故 obd_functional_addr=true。
        // Mode 22 厂商 PID 轮询时代码会在 Slot6 中临时切换到 ATSH7E0，发完再恢复 ATSH7DF。
        // 油温: 先试 PID 4402 (双字节公式), 再试 PID D002(油底壳备用通道), 最后回退 PID 03F3。
        .name = "BMW F/G",
        .final_drive_ratio = 2.813f,       // G20 330i 主减速比
        .tire_rolling_radius_m = 0.330f,   // 225/45R18
        .gear_count = 8,                   // ZF 8HP 8速
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP75
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_BMW_22_4402,    // F/G系 Mode 22 PID 4402
            .secondary = OIL_TEMP_MODE_PID_5C,       // 回退标准 01 5C
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        .has_boost = true,
        .forced_protocol = 6,
        .obd_functional_addr = true,
        .obd_timeout = 0x0F,
    },
    {
        // MINI John Cooper Works F56 (宝马 B48 2.0T, 前驱横置)
        .name = "JCW F56",
        .final_drive_ratio = 3.824f,       // F56 JCW 6MT 主减速比
        .tire_rolling_radius_m = 0.308f,   // 前轮(前驱驱动轮) 205/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.923f, 2.136f, 1.276f, 0.921f, 0.756f, 0.628f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // 涡轮压力走标准 PID 010B(has_boost)，无需额外适配。
            // 油温: MINI/BMW 增强 Mode 22 PID 5822, °C = A-60 (社区验证, N18/N16/B48 同款监视器)；
            // 读不到回退标准 01 5C。
            .primary = OIL_TEMP_MODE_MINI_22_5822,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
        },
        .has_boost = true,                 // B48 涡轮增压，涡轮压力走标准 010B
    },
    {
        // 保时捷 Gen2: 987.2/997.2 (2009-2012, DFI 9A1; 自吸; 油温 x-60)
        .name = "POS 997.2",
        .final_drive_ratio = 3.44f,        // 997.2 PDK 主减速比(用户实测)
        .tire_rolling_radius_m = 0.325f,   // 用户实测滚动半径
        .gear_count = 7,                   // PDK 7速
        .gear_ratios = {0, 3.91f, 2.29f, 1.65f, 1.30f, 1.08f, 0.88f, 0.62f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // 油温油压在 CAN 广播帧 0x441(byte5油温, byte6油压×5/127)，经 ELM327 监听读取
            .primary = OIL_TEMP_MODE_PORSCHE_CAN_441,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
            .can_num = 1, .can_den = 1, .can_off = -60,   // °C = x - 60
        },
        .has_boost = false,                // 自然吸气
        .forced_protocol = 6,              // 广播帧 0x441 是 11bit/500k, 锁协议6 才能 ATMA 监听到
    },
    {
        // 保时捷 Gen1: 987.1/997.1 (2005-2008, M96/M97; 油温 x*3/4-48)
        // 注: 档位比暂沿用 Gen2 占位(987.1 实际略有差异)，主要差异是油温公式。
        .name = "POS 997.1",
        .final_drive_ratio = 3.89f,
        .tire_rolling_radius_m = 0.335f,
        .gear_count = 6,
        .gear_ratios = {0, 3.67f, 2.05f, 1.46f, 1.13f, 0.97f, 0.84f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PORSCHE_CAN_441,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .offset_c = 0,
            .can_num = 3, .can_den = 4, .can_off = -48,   // °C = x*3/4 - 48
        },
        .has_boost = false,
        .forced_protocol = 6,              // 广播帧 0x441 是 11bit/500k, 锁协议6 才能 ATMA 监听到
    },
};

#define PROFILE_COUNT (sizeof(s_profiles) / sizeof(s_profiles[0]))

// 缓存当前激活配置的档位范围
static gear_ratio_range_t s_gear_ranges[VEHICLE_MAX_GEARS];
static uint8_t s_gear_range_count = 0;
static uint8_t s_active_idx = 0;
static bool s_ranges_dirty = true;

// 根据配置重新计算档位传动比范围
static void rebuild_gear_ranges(const vehicle_profile_t *p)
{
    s_gear_range_count = 0;
    for (uint8_t i = 1; i <= p->gear_count && i < VEHICLE_MAX_GEARS; i++) {
        float total = p->gear_ratios[i] * p->final_drive_ratio;
        s_gear_ranges[s_gear_range_count].min_ratio = total * (1.0f - p->gear_tolerance);
        s_gear_ranges[s_gear_range_count].max_ratio = total * (1.0f + p->gear_tolerance);
        s_gear_ranges[s_gear_range_count].gear = (enGear)i; // GEAR_1=1, GEAR_2=2, ...
        s_gear_range_count++;
    }
    s_ranges_dirty = false;
    ESP_LOGI(TAG, "Rebuilt gear ranges for '%s' (%d gears)", p->name, p->gear_count);
}

const vehicle_profile_t *vehicle_profile_get_all(uint8_t *count)
{
    if (count) *count = (uint8_t)PROFILE_COUNT;
    return s_profiles;
}

const vehicle_profile_t *vehicle_profile_get(uint8_t index)
{
    if (index >= PROFILE_COUNT) return &s_profiles[0];
    return &s_profiles[index];
}

const vehicle_profile_t *vehicle_profile_get_active(void)
{
    return vehicle_profile_get(s_active_idx);
}

void vehicle_profile_set_active(uint8_t index)
{
    if (index >= PROFILE_COUNT) index = 0;
    s_active_idx = index;
    s_ranges_dirty = true;

    // 保存到 NVS
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.vehicle_profile_idx = index;
    nvs_cfg_set(&cfg);

    ESP_LOGI(TAG, "Active profile set to [%d] '%s'", index, s_profiles[index].name);
}

float vehicle_profile_calc_constant(const vehicle_profile_t *p)
{
    if (!p) return 0;
    float denom = 0.377f * p->tire_rolling_radius_m;
    if (denom == 0) return 0;
    return 1.0f / denom;
}

const gear_ratio_range_t *vehicle_profile_get_gear_ranges(uint8_t *count)
{
    if (s_ranges_dirty) {
        rebuild_gear_ranges(vehicle_profile_get_active());
    }
    if (count) *count = s_gear_range_count;
    return s_gear_ranges;
}

const oil_temp_strategy_t *vehicle_profile_get_oil_temp_strategy(void)
{
    const vehicle_profile_t *p = vehicle_profile_get_active();
    if (!p) return NULL;
    return &p->oil_temp_strategy;
}
