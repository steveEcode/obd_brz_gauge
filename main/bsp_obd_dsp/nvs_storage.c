#include "nvs_storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "export_path/ui.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "espnow_link.h"   // ESPNOW_ROLE_* (device_role default / bounds)

#define TAG                   "nvs_storage"
#define NS_CFG                "cfg"
#define KEY_CFG               "settings"
#define KEY_CHART_ALARM       "chartalarm"
#define CHART_ALARM_N         12   // = DISP_ITEM_COUNT (must stay in sync with disp_item_t in ui.c)
#define CHART_ALARM_OFF       32767 // "off" sentinel for alarm thresholds (unreachable, avoids false alarms)
#define KEY_MG_EXTRA          "mgextra"   // multi-gauge boot animation settings
#define KEY_CFG_VERSION       "cfgver"    // config version (missing = v0)
#define CFG_VERSION_CURRENT   2           // current version; bump on field add/semantic change (migration in nvs_storage_init)

static nvs_user_cfg_t s_cfg =   {
                        .protocol = 0, // OBD protocol select: 0=auto, 1~9=fixed, default auto
                        .theme_cfg.theme = 0,// UI theme index (0=DEFAULT, registry in ui_theme.c)
                        .theme_cfg.user_theme_domiant_color = COLOR_DOMIANT_PINK,// legacy, unused (kept for struct layout)
                        .theme_cfg.user_theme_secondary_color = COLOR_SECONDARY_PINK,// legacy, unused
                        .ble_device_name = "", // empty = use default "OBDII"
                        .temp_display_map = {0, 1, 2}, // CLT, IAT, OIL
                        .info_display_map = {0, 2, 3, 4, 1}, // CLT, OIL, LOAD, TPS, IAT
                        .brake_temp_warn_c = 600,
                        .oil_pressure_warn_x10 = 80,
                        .device_role = ESPNOW_ROLE_STANDALONE, // new devices (no cfg in NVS) default to standalone (no WiFi/ESP-NOW); existing devices are overridden by load_blob
                        .rpm_warn_threshold = 6000,
                        .rpm_warn_anim_en = 0,
                        .rpm_warn_linked_en = 0,
                    };
static nvs_stat_t     s_stat = {0};   // runtime-only stats, not persisted (reset every boot to save flash)
static SemaphoreHandle_t s_mux;

// Per-item alarm thresholds (raw units), index = disp_item_t: CLT,IAT,OIL,LOD,TPS,RPM,SPD,BAT,OIP,BKT,BST
// By default only oil pressure (8.0bar = x10 80) and brake temp (600°C = x10 6000) keep an alarm; the rest are off.
static int16_t s_chart_alarm[CHART_ALARM_N] = {
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    80, 6000, CHART_ALARM_OFF
};

// Multi-gauge boot animation settings (separate blob): default off, position 1
static struct __attribute__((packed)) {
    uint8_t intro_enable;   // 0/1
    uint8_t device_position; // 1/2/3
    uint8_t boot_mode;      // 0=default animation, 1=custom image, 2=video
} s_mg = { 0, 1, 0 };

/* Forward declarations */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len);
static esp_err_t save_blob(const char *ns,const char *key,const void *data,size_t len);

esp_err_t nvs_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    load_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
    // Mileage/trip stats are no longer persisted (see s_stat declaration); stay {0} and start fresh each boot.
    {   // Chart alarm thresholds: load if present in NVS; otherwise keep static defaults (don't overwrite to 0).
        nvs_handle_t h; size_t sz = sizeof(s_chart_alarm);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_CHART_ALARM, s_chart_alarm, &sz);
            nvs_close(h);
        }
    }
    {   // Multi-gauge boot animation settings: same as above, load if present else keep defaults.
        nvs_handle_t h; size_t sz = sizeof(s_mg);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_MG_EXTRA, &s_mg, &sz);
            nvs_close(h);
        }
        if (s_mg.device_position < 1 || s_mg.device_position > 3) s_mg.device_position = 1;
        if (s_mg.intro_enable > 4) s_mg.intro_enable = 0;
        if (s_mg.boot_mode > 2) s_mg.boot_mode = 0;
        ESP_LOGD("nvs", "mg loaded: intro=%u pos=%u boot=%u (blob_sz=%u)", s_mg.intro_enable, s_mg.device_position, s_mg.boot_mode, (unsigned)sz);
    }

    /* ---- Config version migration ----
       NOTE: this version migration currently only covers the separate s_mg (KEY_MG_EXTRA) blob.
       nvs_user_cfg_t (s_cfg) uses a different mechanism — the generic grow logic in load_blob():
       if the stored blob is smaller than the current struct, copy the old bytes, keep compile-time
       defaults for the new trailing fields, then rewrite the blob at the new size. That logic REQUIRES
       new fields to be appended at the END of nvs_user_cfg_t, otherwise old data would be reinterpreted
       into the wrong fields. Respect this constraint when adding fields to s_cfg; never insert in the middle. */
    {
        nvs_handle_t h;
        uint8_t stored_ver = 0;
        bool has_ver = false;
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, KEY_CFG_VERSION, &stored_ver) == ESP_OK) has_ver = true;
            nvs_close(h);
        }
        if (!has_ver || stored_ver < CFG_VERSION_CURRENT) {
            ESP_LOGW("nvs", "Config migration v%u → v%u", stored_ver, CFG_VERSION_CURRENT);
            // v0 → v1: boot_mode field added, default 0 (SKY GAUGE)
            if (stored_ver < 1) {
                s_mg.boot_mode = 0;
                save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
            }
            // v1 → v2: theme_cfg.theme becomes a real theme selector (see ui_theme.c).
            // The old default value 1 was never used; reset it to 0 (DEFAULT) so existing
            // devices keep their original look. Must persist, otherwise the next boot would
            // read 1 back from NVS. The version bump means a later deliberate AMBER (index 1)
            // selection won't be reset again.
            if (stored_ver < 2) {
                if (s_cfg.theme_cfg.theme == 1) {
                    s_cfg.theme_cfg.theme = 0;
                    save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
                }
            }
            // Future: if (stored_ver < 3) { ... migrate v2→v3 fields ... }
            // Write the new version number
            if (nvs_open(NS_CFG, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, KEY_CFG_VERSION, CFG_VERSION_CURRENT);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI("nvs", "Config migration done, now v%u", CFG_VERSION_CURRENT);
        }
    }

    /* Default-value repair for new fields (old NVS data has rsv[x] all zero) */
    if(s_cfg.brightness_day < 10) s_cfg.brightness_day = 100; // valid range 10-100; 0/unset/out-of-range all become 100
    if(s_cfg.default_page > 6) s_cfg.default_page = 0; // 0=Temp,1=Info,2=Chart,3=Needle,4=Gear,5=Rpm,6=Speed (brake temp merged into Chart)
    if(s_cfg.needle_source_idx >= 11) s_cfg.needle_source_idx = 0; // DISP_ITEM_COUNT=11 (CLT..BOOST)
    if(s_cfg.device_role > 2) s_cfg.device_role = ESPNOW_ROLE_STANDALONE; // role: 0=master 1=slave 2=standalone; out-of-range -> standalone
    if(s_cfg.chart_source_idx >= 11) s_cfg.chart_source_idx = 8; // chart item out-of-range -> default OILP (old NVS byte 0=CLT is also fine, unify to OILP)
    // Clamp vehicle profile index to the registered profile count (out-of-range -> index 0)
    uint8_t vehicle_count = 0;
    vehicle_profile_get_all(&vehicle_count);
    if(vehicle_count > 0 && s_cfg.vehicle_profile_idx >= vehicle_count) s_cfg.vehicle_profile_idx = 0;
    if(s_cfg.brake_temp_warn_c < 10 || s_cfg.brake_temp_warn_c > 1200) s_cfg.brake_temp_warn_c = 600;
    if(s_cfg.oil_pressure_warn_x10 > 100) s_cfg.oil_pressure_warn_x10 = 80;
    // 0=unset/legacy out-of-range -> default 6000; clamped here centrally so callers (ui.c / ui_ScreenPageRpmWarn.c) don't repeat the check
    if(s_cfg.rpm_warn_threshold < 1000) s_cfg.rpm_warn_threshold = 6000;

    // Validate TEMP/INFO custom display-item maps: 0..(DISP_ITEM_COUNT-1)
    for (int i = 0; i < 3; ++i) {
        if (s_cfg.temp_display_map[i] > 11) s_cfg.temp_display_map[i] = (uint8_t)i;
    }
    for (int i = 0; i < 5; ++i) {
        if (s_cfg.info_display_map[i] > 11) {
            static const uint8_t def_map[5] = {0, 2, 3, 4, 1};
            s_cfg.info_display_map[i] = def_map[i];
        }
    }

    s_mux = xSemaphoreCreateMutex();
    return ESP_OK;
}

/* User config */
const nvs_user_cfg_t * nvs_cfg_get(void){ return &s_cfg; }

esp_err_t nvs_cfg_set(const nvs_user_cfg_t *cfg)
{
    if(!cfg) return ESP_ERR_INVALID_ARG;
    if(memcmp(cfg,&s_cfg,sizeof(s_cfg))==0) return ESP_OK;
    s_cfg=*cfg;
    return save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
}

/* Chart alarm thresholds */
int16_t nvs_chart_alarm_get(uint8_t item){
    return (item < CHART_ALARM_N) ? s_chart_alarm[item] : CHART_ALARM_OFF;
}
void nvs_chart_alarm_set(uint8_t item, int16_t raw_threshold){
    if(item >= CHART_ALARM_N) return;
    if(s_chart_alarm[item] == raw_threshold) return;
    s_chart_alarm[item] = raw_threshold;
    save_blob(NS_CFG, KEY_CHART_ALARM, s_chart_alarm, sizeof(s_chart_alarm));
}

/* Multi-gauge boot animation settings */
uint8_t nvs_intro_enable_get(void){ return s_mg.intro_enable; }
void nvs_intro_enable_set(uint8_t en){
    if(en > 4) return;
    if(s_mg.intro_enable == en) return;
    s_mg.intro_enable = en;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}
uint8_t nvs_device_position_get(void){ return s_mg.device_position; }
void nvs_device_position_set(uint8_t pos){
    if(pos < 1 || pos > 3) return;
    if(s_mg.device_position == pos) return;
    s_mg.device_position = pos;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}
uint8_t nvs_boot_mode_get(void){ return s_mg.boot_mode; }
void nvs_boot_mode_set(uint8_t mode){
    if(mode > 2) return;
    if(s_mg.boot_mode == mode) return;
    s_mg.boot_mode = mode;
    save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
}

/* Statistics */
const nvs_stat_t * nvs_stat_get(void){return &s_stat;}
/*
 * Update driving statistics.
 * @param speed_kmh speed in km/h
 * @param dt_ms elapsed time in ms
 * @note skipped if speed is 0, or dt_ms < 1000
 */
void nvs_stat_update_speed(uint8_t speed_kmh, uint32_t dt_ms)
{
    if(dt_ms<1000) return;
    if(speed_kmh == 0) return;

    xSemaphoreTake(s_mux,portMAX_DELAY);
    /* 1. distance = v(km/h)*dt(ms)/3.6  (m) */
    double dist_m = ((double)speed_kmh * (double)dt_ms) / 3.6e3;
    s_stat.odometer_m += (uint64_t)dist_m;
    s_stat.trip_m     += (uint64_t)dist_m;

    /* 2. time */
    s_stat.run_time_s += dt_ms/1000;
    s_stat.trip_run_time_s += dt_ms/1000;

    /* 3. max speed */
    if(speed_kmh > s_stat.max_speed_kmh) s_stat.max_speed_kmh = speed_kmh;

    /* 4. avg speed = trip distance / trip time (m/s) -> km/h */
    if(s_stat.trip_run_time_s){
        double avg_ms = (double)s_stat.trip_m / (double)s_stat.trip_run_time_s; // m/s
        s_stat.avg_speed_kmh = (uint16_t)(avg_ms * 3.6 + 0.5);
        if(s_stat.avg_speed_kmh > s_stat.max_speed_kmh) s_stat.avg_speed_kmh = s_stat.max_speed_kmh;
    }

    xSemaphoreGive(s_mux);
}

/*
 * Reset current-trip statistics (trip distance, max speed, avg speed, running time).
*/
void nvs_stat_reset_trip(void){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    s_stat.trip_m=0;
    s_stat.max_speed_kmh=0;
    s_stat.avg_speed_kmh=0;
    s_stat.run_time_s=0;
    s_stat.trip_run_time_s=0;
    xSemaphoreGive(s_mux);
}

/*
 * Get current-trip statistics (distance, max speed, avg speed, running time).
 * @return a snapshot of the statistics struct
*/
nvs_stat_t nvs_stat_get_mileage(void){
    xSemaphoreTake(s_mux,portMAX_DELAY);
    nvs_stat_t stat = s_stat;
    xSemaphoreGive(s_mux);
    return stat;
}

/* Helpers */
static esp_err_t load_blob(const char *ns,const char *key,void *out,size_t len)
{
    nvs_handle_t h; esp_err_t err;
    if(nvs_open(ns,NVS_READONLY,&h)==ESP_OK){
        // Query the actual stored length first
        size_t stored_len = 0;
        err = nvs_get_blob(h, key, NULL, &stored_len);
        if(err == ESP_OK && stored_len > 0){
            size_t copy_len = (stored_len < len) ? stored_len : len;
            // Partial load: copy what exists; new trailing fields keep their static defaults
            err = nvs_get_blob(h, key, out, &copy_len);
            nvs_close(h);
            if(err == ESP_OK){
                if(stored_len < len){
                    // struct grew; rewrite NVS at the new size (loaded old fields + defaulted new fields)
                    save_blob(ns, key, out, len);
                }
                return ESP_OK;
            }
            return err;
        }
        nvs_close(h);
    }
    memset(out,0,len);
    return save_blob(ns,key,out,len);
}

static esp_err_t save_blob(const char *ns,const char *key,const void *data,size_t len)
{
    nvs_handle_t h; esp_err_t err=nvs_open(ns,NVS_READWRITE,&h);
    if(err!=ESP_OK) return err;
    err=nvs_set_blob(h,key,data,len);
    if(err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err;
}
