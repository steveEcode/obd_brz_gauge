#include "vehicle_profiles.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include <string.h>

#define TAG "vehicle_profile"

// 预定义车辆配置
static const vehicle_profile_t s_profiles[] = {
    {
        .name = "BRZ ZC6",
        .final_drive_ratio = 4.100f,
        .tire_rolling_radius_m = 0.314f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.626f, 2.188f, 1.541f, 1.213f, 1.000f, 0.767f},
        .gear_tolerance = 0.15f,
    },
    {
        .name = "LANCER V3",
        .final_drive_ratio = 4.052f,
        .tire_rolling_radius_m = 0.298f,   // 195/55R15
        .gear_count = 5,
        .gear_ratios = {0, 3.545f, 2.095f, 1.452f, 1.107f, 0.882f},
        .gear_tolerance = 0.15f,
    },
    {
        .name = "CIVIC FD2",
        .final_drive_ratio = 4.764f,
        .tire_rolling_radius_m = 0.307f,   // 215/45R17
        .gear_count = 6,
        .gear_ratios = {0, 3.267f, 2.130f, 1.517f, 1.147f, 0.921f, 0.738f},
        .gear_tolerance = 0.15f,
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
