#include "esp_system.h"

#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "simulator_role_runtime.h"

#include "freertos/semphr.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
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
static startup_animation_t s_startup_animation;

static uint64_t s_speed_weighted_ms;
static uint64_t s_speed_total_ms;

static const char *simulator_nvs_role_key(void)
{
    simulator_role_t role =
        simulator_role_runtime_get();

    switch (role) {
        case SIMULATOR_ROLE_MASTER:
            return "master";

        case SIMULATOR_ROLE_SLAVE_LEFT:
            return "slave-left";

        default:
            return "slave-right";
    }
}

static bool simulator_nvs_startup_path(
    char *out,
    size_t out_size
)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    const char *xdg_config =
        getenv("XDG_CONFIG_HOME");

    const char *home =
        getenv("HOME");

    char base_dir[PATH_MAX];

    if (
        xdg_config != NULL &&
        xdg_config[0] != '\0'
    ) {
        int written = snprintf(
            base_dir,
            sizeof(base_dir),
            "%s",
            xdg_config
        );

        if (
            written < 0 ||
            (size_t)written >= sizeof(base_dir)
        ) {
            return false;
        }
    } else {
        if (home == NULL || home[0] == '\0') {
            return false;
        }

        int written = snprintf(
            base_dir,
            sizeof(base_dir),
            "%s/.config",
            home
        );

        if (
            written < 0 ||
            (size_t)written >= sizeof(base_dir)
        ) {
            return false;
        }
    }

    if (
        mkdir(base_dir, 0755) != 0 &&
        errno != EEXIST
    ) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot create config directory: %s\n",
            base_dir
        );

        return false;
    }

    char app_dir[PATH_MAX];

    int app_written = snprintf(
        app_dir,
        sizeof(app_dir),
        "%s/obd_brz_gauge_simulator",
        base_dir
    );

    if (
        app_written < 0 ||
        (size_t)app_written >= sizeof(app_dir)
    ) {
        return false;
    }

    if (
        mkdir(app_dir, 0755) != 0 &&
        errno != EEXIST
    ) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot create config directory: %s\n",
            app_dir
        );

        return false;
    }

    int path_written = snprintf(
        out,
        out_size,
        "%s/startup-animation-%s.txt",
        app_dir,
        simulator_nvs_role_key()
    );

    return (
        path_written >= 0 &&
        (size_t)path_written < out_size
    );
}

static void simulator_startup_animation_load(void)
{
    char path[PATH_MAX];

    if (
        !simulator_nvs_startup_path(
            path,
            sizeof(path)
        )
    ) {
        return;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return;
    }

    unsigned int saved_value = 0;

    int scanned = fscanf(
        file,
        "%u",
        &saved_value
    );

    fclose(file);

    if (
        scanned == 1 &&
        saved_value < STARTUP_ANIM_COUNT
    ) {
        s_startup_animation =
            (startup_animation_t)saved_value;

        fprintf(
            stderr,
            "\n[SIMULATOR] Loaded startup animation %u from %s\n",
            saved_value,
            path
        );
    }
}

static void simulator_startup_animation_save(void)
{
    char path[PATH_MAX];

    if (
        !simulator_nvs_startup_path(
            path,
            sizeof(path)
        )
    ) {
        return;
    }

    char temporary_path[PATH_MAX];

    int written = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp",
        path
    );

    if (
        written < 0 ||
        (size_t)written >= sizeof(temporary_path)
    ) {
        return;
    }

    FILE *file =
        fopen(temporary_path, "w");

    if (file == NULL) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot write startup animation config: %s\n",
            temporary_path
        );

        return;
    }

    bool write_ok =
        fprintf(
            file,
            "%u\n",
            (unsigned int)s_startup_animation
        ) >= 0;

    bool close_ok =
        fclose(file) == 0;

    if (!write_ok || !close_ok) {
        remove(temporary_path);
        return;
    }

    if (
        rename(temporary_path, path) != 0
    ) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot replace startup animation config: %s\n",
            path
        );

        remove(temporary_path);
        return;
    }

    fprintf(
        stderr,
        "\n[SIMULATOR] Saved startup animation %u to %s\n",
        (unsigned int)s_startup_animation,
        path
    );
}


#define SIMULATOR_GAUGE_PAGE_COUNT 7u

static bool simulator_nvs_last_page_path(
    char *out,
    size_t out_size
)
{
    if (out == NULL || out_size == 0) {
        return false;
    }

    char startup_path[PATH_MAX];

    /*
     * 复用启动动画路径函数创建配置目录，
     * 再替换为独立的最后页面文件名。
     */
    if (
        !simulator_nvs_startup_path(
            startup_path,
            sizeof(startup_path)
        )
    ) {
        return false;
    }

    char *last_slash =
        strrchr(startup_path, '/');

    if (last_slash == NULL) {
        return false;
    }

    *last_slash = '\0';

    int written = snprintf(
        out,
        out_size,
        "%s/last-page-%s.txt",
        startup_path,
        simulator_nvs_role_key()
    );

    return (
        written >= 0 &&
        (size_t)written < out_size
    );
}

static void simulator_last_page_load(void)
{
    char path[PATH_MAX];

    if (
        !simulator_nvs_last_page_path(
            path,
            sizeof(path)
        )
    ) {
        return;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL) {
        return;
    }

    unsigned int saved_page = 0;

    int scanned = fscanf(
        file,
        "%u",
        &saved_page
    );

    fclose(file);

    if (
        scanned == 1 &&
        saved_page < SIMULATOR_GAUGE_PAGE_COUNT
    ) {
        s_user_cfg.default_page =
            (uint8_t)saved_page;

        fprintf(
            stderr,
            "\n[SIMULATOR] Loaded last gauge page %u from %s\n",
            saved_page,
            path
        );
    }
}

static void simulator_last_page_save(void)
{
    if (
        s_user_cfg.default_page >=
        SIMULATOR_GAUGE_PAGE_COUNT
    ) {
        return;
    }

    char path[PATH_MAX];

    if (
        !simulator_nvs_last_page_path(
            path,
            sizeof(path)
        )
    ) {
        return;
    }

    char temporary_path[PATH_MAX];

    int written = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp",
        path
    );

    if (
        written < 0 ||
        (size_t)written >= sizeof(temporary_path)
    ) {
        return;
    }

    FILE *file = fopen(
        temporary_path,
        "w"
    );

    if (file == NULL) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot write last page config: %s\n",
            temporary_path
        );

        return;
    }

    bool write_ok =
        fprintf(
            file,
            "%u\n",
            (unsigned int)s_user_cfg.default_page
        ) >= 0;

    bool close_ok =
        fclose(file) == 0;

    if (!write_ok || !close_ok) {
        remove(temporary_path);
        return;
    }

    if (
        rename(temporary_path, path) != 0
    ) {
        fprintf(
            stderr,
            "\n[SIMULATOR] Cannot replace last page config: %s\n",
            path
        );

        remove(temporary_path);
        return;
    }

    fprintf(
        stderr,
        "\n[SIMULATOR] Saved last gauge page %u to %s\n",
        (unsigned int)s_user_cfg.default_page,
        path
    );
}

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
    simulator_last_page_load();
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
    s_startup_animation = STARTUP_ANIM_ORIGINAL;

    /*
     * 磁盘中存在保存值时覆盖默认 Original。
     * MASTER / LEFT / RIGHT 使用不同配置文件。
     */
    simulator_startup_animation_load();

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

    uint8_t previous_default_page =
        s_user_cfg.default_page;

    s_user_cfg = *cfg;

    if (
        s_user_cfg.default_page <
            SIMULATOR_GAUGE_PAGE_COUNT &&
        s_user_cfg.default_page !=
            previous_default_page
    ) {
        simulator_last_page_save();
    }

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

startup_animation_t nvs_startup_animation_get(void)
{
    simulator_nvs_initialize_defaults();

    if (
        s_startup_animation < STARTUP_ANIM_NONE ||
        s_startup_animation >= STARTUP_ANIM_COUNT
    ) {
        return STARTUP_ANIM_ORIGINAL;
    }

    return s_startup_animation;
}

void nvs_startup_animation_set(
    startup_animation_t animation
)
{
    simulator_nvs_initialize_defaults();

    if (
        animation < STARTUP_ANIM_NONE ||
        animation >= STARTUP_ANIM_COUNT
    ) {
        return;
    }

    s_startup_animation = animation;

    simulator_startup_animation_save();

    fprintf(
        stderr,
        "\n[SIMULATOR] Startup animation set to %u\n",
        (unsigned int)animation
    );
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
