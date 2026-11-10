#include "nvs_storage.h"
#include <stddef.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include "export_path/ui.h"
#include "espnow_link.h"   // ESPNOW_ROLE_* (device_role default / bounds)

#define TAG                   "nvs_storage"
#define NS_CFG                "cfg"
#define KEY_CFG               "settings"
#define KEY_CHART_ALARM       "chartalarm"
#define CHART_ALARM_N         12   // = DISP_ITEM_COUNT; indexed by disp_item_t (ui_disp_item.h). Never shrink: a shorter buffer fails to load the stored 24B blob
#define CHART_ALARM_LEGACY_N  11   // blob size before AFR became a selectable display source
#define CHART_ALARM_OFF       32767 // "off" sentinel for alarm thresholds (unreachable, avoids false alarms)
#define KEY_MG_EXTRA          "mgextra"   // multi-gauge boot animation settings
#define KEY_CFG_VERSION       "cfgver"    // config version (missing = v0)
#define CFG_VERSION_CURRENT   4           // current version; bump on field add/semantic change (migration in nvs_storage_init)
#define LEGACY_VEHICLE_IDX_MAX 16         // old compiled-in vehicle table: idx 0 (OBD2 Generic) .. 16

static nvs_user_cfg_t s_cfg =   {
                        .protocol = 0, // OBD protocol select: 0=auto, 1~9=fixed, default auto
                        .theme_cfg.theme = 0,// UI theme index (0=DEFAULT, registry in ui_theme.c)
                        .theme_cfg.user_theme_domiant_color = COLOR_DOMIANT_PINK,// legacy, unused (kept for struct layout)
                        .theme_cfg.user_theme_secondary_color = COLOR_SECONDARY_PINK,// legacy, unused
                        .ble_device_name = "", // empty = use default "OBDII"
                        .brightness_day = 100,
                        .temp_display_map = NVS_TEMP_MAP_DEFAULT, // CLT, IAT, OIL
                        .info_display_map = NVS_INFO_MAP_DEFAULT, // CLT, OIL, LOAD, TPS, IAT
                        .oil_pressure_warn_x10 = 80,
                        .device_role = ESPNOW_ROLE_STANDALONE, // new devices (no cfg in NVS) default to standalone (no WiFi/ESP-NOW); existing devices are overridden by load_blob
                        .rpm_warn_threshold = 6000,
                        .rpm_warn_anim_en = 0,
                        .rpm_warn_linked_en = 0,
                    };
static SemaphoreHandle_t s_mux;       // serializes s_cfg read-modify-write + save (UI task vs vehicle manager)

// Per-item alarm thresholds (raw units), index = disp_item_t: CLT,IAT,OIL,LOD,TPS,RPM,SPD,BAT,OIP,RESERVED_9,BST,AFR
// By default only oil pressure (8.0bar = x10 80) keeps an alarm; the rest are off. Slot 9 is retired (never reuse).
static int16_t s_chart_alarm[CHART_ALARM_N] = {
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF,
    80, CHART_ALARM_OFF, CHART_ALARM_OFF, CHART_ALARM_OFF
};
_Static_assert(CHART_ALARM_N == DISP_ITEM_COUNT, "chartalarm blob is indexed by disp_item_t");
_Static_assert(offsetof(nvs_user_cfg_t, tpms) == 96, "TPMS bindings must start after the entire legacy cfg blob");

_Static_assert(sizeof(nvs_tpms_binding_t) == 39, "Legacy TPMS record layout is persistent");
_Static_assert(offsetof(nvs_user_cfg_t, tpms_sensor_id) == 252, "IDs must follow the complete MAC-only cfg blob");

// Multi-gauge boot animation settings (separate blob):
// intro_enable 0=OFF 1=RACE 2=VIDEO (SD OBV1, with legacy bootmedia fallback; default)
static struct __attribute__((packed)) {
    uint8_t intro_enable;   // 0=OFF 1=RACE 2=VIDEO
    uint8_t device_position; // 1/2/3
    uint8_t boot_mode;      // retired (no reader); byte kept so the stored 3-byte blob layout never changes
} s_mg = { 2, 1, 0 };

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

    s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;

    // ESP_ERR_NVS_NOT_FOUND = fresh device: s_cfg keeps its static defaults and is
    // persisted once after the repair pass below (bug #19).
    bool cfg_fresh = (load_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg)) == ESP_ERR_NVS_NOT_FOUND);

    {   // Chart alarm thresholds: load current blobs or preserve the pre-AFR 11-slot layout.
        nvs_handle_t h; size_t sz = 0;
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, KEY_CHART_ALARM, NULL, &sz) == ESP_OK) {
                if (sz == sizeof(s_chart_alarm)) {
                    nvs_get_blob(h, KEY_CHART_ALARM, s_chart_alarm, &sz);
                } else if (sz == CHART_ALARM_LEGACY_N * sizeof(int16_t)) {
                    int16_t legacy[CHART_ALARM_LEGACY_N];
                    if (nvs_get_blob(h, KEY_CHART_ALARM, legacy, &sz) == ESP_OK) {
                        memcpy(s_chart_alarm, legacy, sizeof(legacy));
                        s_chart_alarm[DISP_ITEM_AFR] = CHART_ALARM_OFF;
                    }
                }
            }
            nvs_close(h);
        }
        // Older firmware lacked the AFR initializer and persisted 0 with the whole array. The UI
        // cannot write below 800 (nmin 8 x div 100), so treat it as unset. Idempotent, RAM only.
        if (s_chart_alarm[DISP_ITEM_AFR] < 800) s_chart_alarm[DISP_ITEM_AFR] = CHART_ALARM_OFF;
    }
    {   // Multi-gauge boot animation settings: same as above, load if present else keep defaults.
        nvs_handle_t h; size_t sz = sizeof(s_mg);
        if (nvs_open(NS_CFG, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_blob(h, KEY_MG_EXTRA, &s_mg, &sz);
            nvs_close(h);
        }
        if (s_mg.device_position < 1 || s_mg.device_position > 3) s_mg.device_position = 1;
        if (s_mg.intro_enable > 2) s_mg.intro_enable = 2;   // legacy REI/SHINJI/ASUKA (3/4) map to VIDEO (2)
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
            // v2 → v3: the three built-in boot videos (REI/SHINJI/ASUKA = intro 2/3/4) were
            // replaced by a single app-flashed animation (VIDEO = 2). Old 3/4 map to 2; old 2
            // keeps its value but now plays /bootmedia/boot_block.*. Persist so the roller
            // never shows a stale selection again.
            if (stored_ver < 3) {
                if (s_mg.intro_enable > 2) {
                    s_mg.intro_enable = 2;
                }
                save_blob(NS_CFG, KEY_MG_EXTRA, &s_mg, sizeof(s_mg));
            }
            // v3 → v4: vehicles moved to JSON profiles (vp_manager). The active profile id
            // and every new setting live in nvs_app keys; nvs_user_cfg_t keeps its exact
            // size/layout so firmware rolled back by OTA still reads it. vehicle_profile_idx
            // stays as a downgrade mirror (written by nvs_cfg_vehicle_idx_mirror_set());
            // first_boot_migration maps it to a catalog id once. No blob change here.
            // Future: if (stored_ver < 5) { ... }
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
    if(s_cfg.default_page > 7) s_cfg.default_page = 0; // 0=Temp,1=Info,2=Chart,3=Needle,4=Gear,5=Rpm,6=Speed,7=TPMS
    if(s_cfg.device_role > 2) s_cfg.device_role = ESPNOW_ROLE_STANDALONE; // role: 0=master 1=slave 2=standalone; out-of-range -> standalone
    // Legacy vehicle index is only a stored byte now (downgrade mirror); clamp to the old table range.
    if(s_cfg.vehicle_profile_idx > LEGACY_VEHICLE_IDX_MAX) s_cfg.vehicle_profile_idx = 0;
    if(s_cfg.oil_pressure_warn_x10 > 100) s_cfg.oil_pressure_warn_x10 = 80;
    // 0=unset/legacy out-of-range -> default 6000; clamped here centrally so callers (ui.c / ui_ScreenPageRpmWarn.c) don't repeat the check
    if(s_cfg.rpm_warn_threshold < 1000) s_cfg.rpm_warn_threshold = 6000;

    // Persisted disp_item_t indexes: anything not selectable (out of range or a retired slot such as
    // DISP_ITEM_RESERVED_9) falls back to its default. Runs every boot, idempotent, RAM only
    // (written back on the next nvs_cfg_update). The custom pages use the same defaults.
    {
        static const uint8_t def_temp[3] = NVS_TEMP_MAP_DEFAULT;
        static const uint8_t def_info[5] = NVS_INFO_MAP_DEFAULT;
        for (int i = 0; i < 3; ++i) {
            if (!ui_disp_item_is_selectable(s_cfg.temp_display_map[i])) s_cfg.temp_display_map[i] = def_temp[i];
        }
        for (int i = 0; i < 5; ++i) {
            if (!ui_disp_item_is_selectable(s_cfg.info_display_map[i])) s_cfg.info_display_map[i] = def_info[i];
        }
        if (!ui_disp_item_is_selectable(s_cfg.needle_source_idx)) s_cfg.needle_source_idx = NVS_NEEDLE_SRC_DEFAULT;
        if (!ui_disp_item_is_selectable(s_cfg.chart_source_idx)) s_cfg.chart_source_idx = NVS_CHART_SRC_DEFAULT;
    }

    if (cfg_fresh) {
        // Fresh device: persist the static defaults (STANDALONE role, default maps) so the
        // stored blob never holds zeros, also for firmware rolled back by OTA.
        save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
        ESP_LOGI(TAG, "fresh device: default config saved");
    }

    return ESP_OK;
}

static void cfg_lock(void)   { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
static void cfg_unlock(void) { if (s_mux) xSemaphoreGive(s_mux); }

/* User config */
esp_err_t nvs_cfg_snapshot(nvs_user_cfg_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    cfg_lock();
    *out = s_cfg;
    cfg_unlock();
    return ESP_OK;
}

nvs_user_cfg_t nvs_cfg_read(void)
{
    nvs_user_cfg_t out;
    cfg_lock();
    out = s_cfg;
    cfg_unlock();
    return out;
}

esp_err_t nvs_cfg_update(const nvs_user_cfg_t *base, const nvs_user_cfg_t *updated)
{
    if (!base || !updated) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    cfg_lock();
    nvs_user_cfg_t previous = s_cfg;
    nvs_user_cfg_t next = previous;
    bool changed = false;
#define MERGE_CFG_FIELD(field) do { \
        if (memcmp(&base->field, &updated->field, sizeof(base->field)) != 0 && \
            memcmp(&next.field, &updated->field, sizeof(next.field)) != 0) { \
            memcpy(&next.field, &updated->field, sizeof(next.field)); \
            changed = true; \
        } \
    } while (0)
    /* Keep this list in sync with nvs_user_cfg_t; compare fields, never padding bytes. */
    MERGE_CFG_FIELD(protocol);
    MERGE_CFG_FIELD(theme_cfg.theme);
    MERGE_CFG_FIELD(theme_cfg.user_theme_domiant_color);
    MERGE_CFG_FIELD(theme_cfg.user_theme_secondary_color);
    MERGE_CFG_FIELD(theme_cfg.theme_slot);
    MERGE_CFG_FIELD(theme_cfg.rsv);
    MERGE_CFG_FIELD(ble_device_name);
    MERGE_CFG_FIELD(default_page);
    MERGE_CFG_FIELD(brightness_day);
    MERGE_CFG_FIELD(vehicle_profile_idx);
    MERGE_CFG_FIELD(rsv_brake_temp_warn_c);
    MERGE_CFG_FIELD(oil_pressure_warn_x10);
    MERGE_CFG_FIELD(temp_display_map);
    MERGE_CFG_FIELD(info_display_map);
    MERGE_CFG_FIELD(needle_source_idx);
    MERGE_CFG_FIELD(device_role);
    MERGE_CFG_FIELD(chart_source_idx);
    MERGE_CFG_FIELD(rpm_warn_threshold);
    MERGE_CFG_FIELD(rpm_warn_anim_en);
    MERGE_CFG_FIELD(espnow_master_mac);
    MERGE_CFG_FIELD(ble_obd_mac);
    MERGE_CFG_FIELD(rpm_warn_linked_en);
    MERGE_CFG_FIELD(rc_enabled);
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i) {
        MERGE_CFG_FIELD(tpms[i]);
        MERGE_CFG_FIELD(tpms_sensor_id[i]);
    }
#undef MERGE_CFG_FIELD
    if (changed) {
        s_cfg = next;
        err = save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
        if (err != ESP_OK) s_cfg = previous;
    }
    cfg_unlock();
    return err;
}

esp_err_t nvs_cfg_vehicle_idx_mirror_set(uint8_t idx)
{
    if(idx > LEGACY_VEHICLE_IDX_MAX) idx = 0;
    esp_err_t err = ESP_OK;
    cfg_lock();
    if(s_cfg.vehicle_profile_idx != idx){
        uint8_t previous = s_cfg.vehicle_profile_idx;
        s_cfg.vehicle_profile_idx = idx;
        err = save_blob(NS_CFG, KEY_CFG, &s_cfg, sizeof(s_cfg));
        if (err != ESP_OK) s_cfg.vehicle_profile_idx = previous;
    }
    cfg_unlock();
    return err;
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
    // Fresh device: no entry in NVS. Leave `out` at its static-initialiser defaults
    // (e.g. device_role = ESPNOW_ROLE_STANDALONE). Do NOT memset+save here — that
    // would zero out fields the caller set at compile time and immediately persist
    // them before the default-repair pass runs (bug #19).
    return ESP_ERR_NVS_NOT_FOUND;
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
