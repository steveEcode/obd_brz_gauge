#include "esp_system.h"

#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "simulator_role_runtime.h"

#include "freertos/semphr.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* BLE 扫描页面需要这个全局 LVGL 锁。 */
static int s_lvgl_mux_token;
SemaphoreHandle_t lvgl_mux = &s_lvgl_mux_token;

/* 模拟 LCD 背光状态。 */
uint8_t LCD_Backlight = 100;

/* 模拟 NVS：数据只保存在当前进程内存中。 */
static bool s_nvs_initialized;
static nvs_user_cfg_t s_user_cfg;
static nvs_stat_t s_stat;
static int16_t s_chart_alarm[256];
static uint8_t s_intro_enabled;
static uint8_t s_device_position;

static uint64_t s_speed_weighted_ms;
static uint64_t s_speed_total_ms;

static void simulator_nvs_initialize_defaults(void)
{
    if (s_nvs_initialized) {
        return;
    }

    memset(&s_user_cfg, 0, sizeof(s_user_cfg));
    memset(&s_stat, 0, sizeof(s_stat));

    s_user_cfg.protocol = 0;
    s_user_cfg.theme_cfg.theme = 1;
    snprintf(
        s_user_cfg.ble_device_name,
        sizeof(s_user_cfg.ble_device_name),
        "%s",
        "SIM-OBDII"
    );

    s_user_cfg.default_page = 0;
    s_user_cfg.brightness_day = 100;
    s_user_cfg.vehicle_profile_idx = 1;

    s_user_cfg.brake_temp_warn_c = 600;
    s_user_cfg.oil_pressure_warn_x10 = 10;

    s_user_cfg.temp_display_map[0] = 0;
    s_user_cfg.temp_display_map[1] = 1;
    s_user_cfg.temp_display_map[2] = 2;

    s_user_cfg.info_display_map[0] = 0;
    s_user_cfg.info_display_map[1] = 1;
    s_user_cfg.info_display_map[2] = 2;
    s_user_cfg.info_display_map[3] = 3;
    s_user_cfg.info_display_map[4] = 4;

    s_user_cfg.needle_source_idx = 0;
    simulator_role_t role =
        simulator_role_runtime_get();

    if (role == SIMULATOR_ROLE_MASTER) {
        s_user_cfg.device_role = 0;
    } else {
        s_user_cfg.device_role = 1;
    }
    s_user_cfg.chart_source_idx = 8;
    s_user_cfg.rpm_warn_threshold = 6000;
    s_user_cfg.rpm_warn_anim_en = 1;

    s_intro_enabled = 1;

    if (role == SIMULATOR_ROLE_SLAVE_LEFT) {
        s_device_position = 1;
    } else if (role == SIMULATOR_ROLE_MASTER) {
        s_device_position = 2;
    } else {
        s_device_position = 3;
    }

    for (size_t index = 0; index < 256; index++) {
        s_chart_alarm[index] = INT16_MAX;
    }

    s_speed_weighted_ms = 0;
    s_speed_total_ms = 0;
    s_nvs_initialized = true;
}

esp_err_t nvs_storage_init(void)
{
    simulator_nvs_initialize_defaults();
    return ESP_OK;
}

const nvs_user_cfg_t *nvs_cfg_get(void)
{
    simulator_nvs_initialize_defaults();
    return &s_user_cfg;
}

esp_err_t nvs_cfg_set(const nvs_user_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_FAIL;
    }

    simulator_nvs_initialize_defaults();
    s_user_cfg = *cfg;

    return ESP_OK;
}

int16_t nvs_chart_alarm_get(uint8_t item)
{
    simulator_nvs_initialize_defaults();
    return s_chart_alarm[item];
}

void nvs_chart_alarm_set(
    uint8_t item,
    int16_t raw_threshold
)
{
    simulator_nvs_initialize_defaults();
    s_chart_alarm[item] = raw_threshold;
}

uint8_t nvs_intro_enable_get(void)
{
    simulator_nvs_initialize_defaults();
    return s_intro_enabled;
}

void nvs_intro_enable_set(uint8_t en)
{
    simulator_nvs_initialize_defaults();
    s_intro_enabled = en ? 1u : 0u;
}

uint8_t nvs_device_position_get(void)
{
    simulator_nvs_initialize_defaults();
    return s_device_position;
}

void nvs_device_position_set(uint8_t pos)
{
    simulator_nvs_initialize_defaults();

    if (pos < 1u) {
        pos = 1u;
    } else if (pos > 3u) {
        pos = 3u;
    }

    s_device_position = pos;
}

const nvs_stat_t *nvs_stat_get(void)
{
    simulator_nvs_initialize_defaults();
    return &s_stat;
}

void nvs_stat_add_odometer(uint32_t delta_m)
{
    simulator_nvs_initialize_defaults();
    s_stat.odometer_m += delta_m;
}

void nvs_stat_add_runtime(uint32_t delta_s)
{
    simulator_nvs_initialize_defaults();
    s_stat.run_time_s += delta_s;
    s_stat.trip_run_time_s += delta_s;
}

void nvs_stat_add_trip(uint32_t delta_m)
{
    simulator_nvs_initialize_defaults();
    s_stat.trip_m += delta_m;
}

void nvs_stat_reset_trip(void)
{
    simulator_nvs_initialize_defaults();

    s_stat.trip_m = 0;
    s_stat.trip_run_time_s = 0;
    s_stat.avg_speed_kmh = 0;

    s_speed_weighted_ms = 0;
    s_speed_total_ms = 0;
}

void nvs_stat_update_speed(
    uint8_t speed_kmh,
    uint32_t dt_ms
)
{
    simulator_nvs_initialize_defaults();

    if (speed_kmh > s_stat.max_speed_kmh) {
        s_stat.max_speed_kmh = speed_kmh;
    }

    s_speed_weighted_ms +=
        (uint64_t)speed_kmh * (uint64_t)dt_ms;

    s_speed_total_ms += dt_ms;

    if (s_speed_total_ms > 0) {
        s_stat.avg_speed_kmh =
            (uint16_t)(
                s_speed_weighted_ms /
                s_speed_total_ms
            );
    }
}

void Backlight_Init(void)
{
    LCD_Backlight = 100;
}

void Set_Backlight(uint8_t light)
{
    if (light > Backlight_MAX) {
        light = Backlight_MAX;
    }

    LCD_Backlight = light;

    fprintf(
        stderr,
        "\n[SIMULATOR] Backlight set to %u%%\n",
        (unsigned int)LCD_Backlight
    );
}

void esp_restart(void)
{
    fprintf(
        stderr,
        "\n[SIMULATOR] esp_restart() requested; "
        "restart is ignored on desktop.\n"
    );
}
