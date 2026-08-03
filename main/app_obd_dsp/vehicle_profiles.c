#include "vehicle_profiles.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <string.h>

#define TAG "vehicle_profile"

// Predefined vehicle profiles
static const vehicle_profile_t s_profiles[] = {
    {
        // Generic OBD2 standard profile (SAE J1979 / ISO 15031-5)
        // Uses only standard PIDs, no manufacturer-specific protocols:
        //   RPM 010C, Speed 010D, Coolant 0105, Oil Temp 015C, Intake 010F, Load 0104, TPS 0111, Voltage 0142
        // Gear ratios are common 6MT placeholders (only affects gear detection accuracy, not data reading).
        .name = "OBD2 Generic",
        .final_drive_ratio = 3.500f,       // Generic placeholder
        .tire_rolling_radius_m = 0.315f,   // Common for 205/55R16
        .gear_count = 6,
        .gear_ratios = {0, 3.500f, 2.000f, 1.400f, 1.100f, 0.900f, 0.750f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // Standard OBD2 oil temp PID (°C = A - 40)
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = false,                // Generic defaults to NA; turbo cars can still use standard 010B (manual enable)
    },
    {
        // BRZ ZC6 CAN (2013-2020, FA20 NA, Gen1)
        // ATMA monitor: 0x140 (100Hz RPM+TPS), 0x360 (20Hz oil+coolant temp)
        // Remaining channels use standard OBD PID
        // Ref: https://github.com/timurrrr/ft86/blob/main/can_bus/gen1.md
        .name = "ZC/N6",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_TOYOTA_21_01,
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .can_broadcast_mode = true,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZD8 CAN (2022+, FA24 NA, Gen2)
        // ATMA monitor: 0x40 (100Hz RPM+TPS), 0x345 (10Hz oil+coolant temp)
        // Remaining channels use standard OBD PID
        // Ref: https://github.com/timurrrr/ft86/blob/main/can_bus/gen2.md
        .name = "ZD8 CAN",
        .final_drive_ratio = 3.700f,       // ZD8 final drive ratio
        .tire_rolling_radius_m = 0.318f,   // 225/40R18
        .gear_count = 6,
        .gear_ratios = {0, 3.765f, 2.476f, 1.633f, 1.190f, 0.932f, 0.751f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ZD8 uses standard PID 5C
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .can_broadcast_mode = true,
        .poll_gap_ms = 1,
    },
    {
        // BRZ ZD8 (标准 OBD 兜底, 不走 CAN ATMA 监听)
        // 部分廉价克隆 ELM327 适配器不支持/该车总线不出 ATMA 监听帧, 导致 "ZD8 CAN" 拿不到
        // 0x040/0x345 数据(转速尚可靠标准 01 0C 兜底刷新, 但油温水温完全依赖 0x345, 会一直读不到)。
        // 这个变体转速/水温/油温全部走标准 OBD PID(01 0C / 01 05 / 01 5C), 牺牲 100Hz 转速刷新率
        // 换稳定性; ATMA 监听没问题的话优先用上面的 "ZD8 CAN"。
        .name = "ZD8",
        .final_drive_ratio = 3.700f,
        .tire_rolling_radius_m = 0.318f,   // 225/40R18
        .gear_count = 6,
        .gear_ratios = {0, 3.765f, 2.476f, 1.633f, 1.190f, 0.932f, 0.751f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // ZD8 支持标准 PID 5C
            .secondary = OIL_TEMP_MODE_NONE,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .forced_protocol = 6,
        .poll_gap_ms = 1,
    },
    {
        .name = "MX-5 ND",
        .final_drive_ratio = 2.866f,       // ND 6MT (all manuals identical; auto is 3.583)
        .tire_rolling_radius_m = 0.300f,   // 195/50R16
        .gear_count = 6,
        .gear_ratios = {0, 5.087f, 2.991f, 2.035f, 1.594f, 1.286f, 1.000f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // Try PID 1310 (double byte) first, fallback to 111F (single byte) on consecutive failures
            .primary = OIL_TEMP_MODE_MAZDA_22_1310,
            .secondary = OIL_TEMP_MODE_MAZDA_22_111F,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        // .has_boost defaults to false (NA)
        .obd_timeout = 0x0A,  // 40ms timeout; Mazda CAN typically responds in 5-15ms, reduces NO DATA waits
        .poll_gap_ms = 50,    // 50ms slot gap: fast enough for responsive UI, safe for Bluetooth ELM327 adapters
    },
    {
        // BMW G-series (G20/G21/G22, B48/B58 turbo, ZF 8HP)
        // Standard OBD only responds to 7DF functional addressing, 7E0 physical gets no response; hence obd_functional_addr=true.
        // During Mode 22 vendor PID polling, code temporarily switches to ATSH7E0 in Slot6, then restores ATSH7DF.
        // Oil temp: try PID 4402 (double byte) first, then PID D002 (oil pan backup), finally fallback to PID 03F3.
        .name = "BMW F/G",
        .final_drive_ratio = 2.813f,       // G20 330i final drive ratio
        .tire_rolling_radius_m = 0.330f,   // 225/45R18
        .gear_count = 8,                   // ZF 8HP 8-speed
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP75
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // Standard OBD2 PID 01 5C (reliable via OBDII)
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,
        .forced_protocol = 6,
        .obd_functional_addr = true,
        .obd_timeout = 0x0F,
    },
    {
        // BMW G-series CAN (G20/G21/G22/G80/G82, B48/B58 turbo, ZF 8HP)
        // PT-CAN 500kbps, ATMA monitoring replaces OBD polling for key channels:
        //   0x0A5 (100Hz): RPM byte5-6 LE (raw×4)
        //   0x254 (50Hz):  wheel speed byte4-5 LE (raw×0.015625−511.98 km/h, front-left)
        //   0x3F9 (1Hz):   coolant byte4 (raw−48), oil byte5 (raw−48), gear byte6 nibble (raw−4)
        // Ref: racechrono-canbus decoder_bmwg8x.cpp, thesecretingredient.neocities.org/bmw/can/g29/
        .name = "BMW G CAN",
        .final_drive_ratio = 2.813f,       // G20 330i final drive ratio
        .tire_rolling_radius_m = 0.330f,   // 225/45R18
        .gear_count = 8,                   // ZF 8HP 8-speed
        .gear_ratios = {0, 5.250f, 3.360f, 2.172f, 1.720f, 1.316f, 1.000f, 0.822f, 0.640f},  // ZF 8HP75
        .gear_tolerance = 0.09f,
        .oil_temp_strategy = {
            .primary = OIL_TEMP_MODE_PID_5C,        // CAN 0x3F9 provides oil temp directly; OBD 01 5C as fallback
            .secondary = OIL_TEMP_MODE_BMW_22_4402, // Mode 22 PID 4402 backup
            .tertiary = OIL_TEMP_MODE_NONE,
            .quaternary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // B48/B58 turbo
        .forced_protocol = 7,              // PT-CAN uses 29-bit extended frames (protocol 7)
        .obd_functional_addr = true,       // 7DF functional addressing
        .obd_timeout = 0x0A,               // 40ms; BMW CAN responds quickly
        .can_broadcast_mode = true,        // ATMA monitoring for 0x0A5/0x254/0x3F9
        .poll_gap_ms = 1,
    },
    {
        // MINI John Cooper Works F56 (BMW B48 2.0T, FWD transverse)
        .name = "JCW F56",
        .final_drive_ratio = 3.824f,       // F56 JCW 6MT final drive ratio
        .tire_rolling_radius_m = 0.308f,   // Front wheels (FWD drive wheels) 205/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.923f, 2.136f, 1.276f, 0.921f, 0.756f, 0.628f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // Boost pressure uses standard PID 010B (has_boost), no extra adaptation needed.
            // Oil temp: MINI/BMW enhanced Mode 22 PID 5822, °C = A-60 (community verified, same monitor as N18/N16/B48);
            // Fallback to standard 01 5C if unavailable.
            .primary = OIL_TEMP_MODE_MINI_22_5822,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
        },
        .has_boost = true,                 // B48 turbo, boost pressure via standard 010B
    },
    {
        // Porsche Gen2: 987.2/997.2 (2009-2012, DFI 9A1; NA; oil temp x-60)
        .name = "POS 997.2",
        .final_drive_ratio = 3.44f,        // 997.2 PDK final drive ratio (user measured)
        .tire_rolling_radius_m = 0.325f,   // User measured rolling radius
        .gear_count = 7,                   // PDK 7-speed
        .gear_ratios = {0, 3.91f, 2.29f, 1.65f, 1.30f, 1.08f, 0.88f, 0.62f},
        .gear_tolerance = 0.15f,
        .oil_temp_strategy = {
            // Oil temp/pressure via CAN broadcast 0x441 (byte5=oil temp, byte6=oil pressure×5/127), read via ELM327 ATMA
            .primary = OIL_TEMP_MODE_PORSCHE_CAN_441,
            .secondary = OIL_TEMP_MODE_PID_5C,
            .tertiary = OIL_TEMP_MODE_NONE,
            .can_num = 1, .can_den = 1, .can_off = -60,   // °C = x - 60
        },
        .has_boost = false,                // Naturally aspirated
        .forced_protocol = 6,              // Broadcast 0x441 is 11bit/500k, lock protocol 6 for ATMA monitoring
    },
    {
        // Porsche Gen1: 987.1/997.1 (2005-2008, M96/M97; oil temp x*3/4-48)
        // Note: Gear ratios use Gen2 placeholders (987.1 differs slightly); main difference is oil temp formula.
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
            .can_num = 3, .can_den = 4, .can_off = -48,   // °C = x*3/4 - 48
        },
        .has_boost = false,
        .forced_protocol = 6,              // Broadcast 0x441 is 11bit/500k, lock protocol 6 for ATMA monitoring
    },
};

#define PROFILE_COUNT (sizeof(s_profiles) / sizeof(s_profiles[0]))

// Cached gear ranges for the active profile
static gear_ratio_range_t s_gear_ranges[VEHICLE_MAX_GEARS];
static uint8_t s_gear_range_count = 0;
static uint8_t s_active_idx = 0;
static bool s_ranges_dirty = true;

// Recalculate gear ratio ranges from profile
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

    // Save to NVS
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.vehicle_profile_idx = index;
    nvs_cfg_set(&cfg);

    ESP_LOGI(TAG, "Active profile set to [%d] '%s'", index, s_profiles[index].name);
}

float vehicle_profile_calc_constant(const vehicle_profile_t *p)
{
    if (!p) return 0;
    float denom = p->final_drive_ratio * 0.377f * p->tire_rolling_radius_m;
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

const vehicle_override_t *vehicle_profile_get_override(void)
{
    const vehicle_profile_t *p = vehicle_profile_get_active();
    if (!p) return NULL;
    return vehicle_find_override(p->name);
}
