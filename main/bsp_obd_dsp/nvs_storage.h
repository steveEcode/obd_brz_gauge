#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Theme config. The index/selectors are real now (see ui_theme.c); the two
// color fields are legacy and unused, kept only to preserve struct layout.
typedef struct {
    uint8_t  theme;         // UI theme index (0=DEFAULT, registry in ui_theme.c)
    uint32_t user_theme_domiant_color;   // legacy, unused (kept for layout)
    uint32_t user_theme_secondary_color;   // legacy, unused (kept for layout)
    uint8_t  theme_slot;    // unused — only one theme partition (theme_0) exists, kept for struct layout
    uint8_t rsv[4];         // reserved for future use
} theme_cfg_t;

// Factory display-item defaults (disp_item_t values). Also the fallback for a stored
// index that is not ui_disp_item_is_selectable() (nvs_storage_init + custom pages).
#define NVS_TEMP_MAP_DEFAULT    {0, 1, 2}         // CLT, IAT, OIL
#define NVS_INFO_MAP_DEFAULT    {0, 2, 3, 4, 1}   // CLT, OIL, LOAD, TPS, IAT
#define NVS_NEEDLE_SRC_DEFAULT  0                 // CLT
#define NVS_CHART_SRC_DEFAULT   8                 // OILP

#define NVS_TPMS_WHEELS 4
/* Legacy MAC-only records: layout must remain unchanged. Never infer an ID from these. */
typedef struct {
    uint8_t mac[6];
    uint8_t addr_type;
    char name[32];
} nvs_tpms_binding_t;

/*------------------ User configuration (written only when changed) ------------------*/
typedef struct {
    uint8_t protocol;      // OBD protocol: 0=auto, 1~9=fixed
    theme_cfg_t theme_cfg;   // theme config
    char    ble_device_name[32]; // last connected BLE device name, empty = not configured
    uint8_t default_page;   // default boot page: 0=Temp, 1=Info, 2=Chart, 3=Needle, 4=Gear, 5=RPM, 6=Speed, 7=TPMS
    uint8_t brightness_day; // brightness 10-100, 0=unset (use 100)
    uint8_t vehicle_profile_idx; // legacy vehicle index, 0=OBD2 Generic, 1..16=old compiled-in table
                                  // (mapping now in vp_manager.c k_legacy_ids[] / vp_manager_legacy_id())
    uint16_t rsv_brake_temp_warn_c; // retired, layout only (offset 60, never reuse)
    uint16_t oil_pressure_warn_x10; // oil pressure warning threshold, 0.1bar
    uint8_t temp_display_map[3]; // TEMP page, 3 rows display-item mapping
    uint8_t info_display_map[5]; // INFO page, 5-cell display-item mapping
    uint8_t needle_source_idx;   // needle page data source (disp_item_t value, default 0=CLT)
    uint8_t device_role;         // multi-gauge role: 0=master (reads ELM327), 1=slave (receives master data)
    uint8_t chart_source_idx;    // chart page data item (disp_item_t value, default 8=OILP)
    uint16_t rpm_warn_threshold; // RPM warning threshold (rpm), 0=unset (default 6000)
    uint8_t rpm_warn_anim_en;    // RPM warning flash enable: 0=off, 1=on
    uint8_t espnow_master_mac[6];// master MAC a slave is bound to (all-zero = unbound / accept any)
    uint8_t ble_obd_mac[6];      // paired ELM327 exact MAC (all-zero = unbound, fuzzy-match by ble_device_name).
                                 // NOTE: new fields MUST be appended at the END of this struct;
                                 // see the load_blob grow logic comment in nvs_storage.c.
    uint8_t rpm_warn_linked_en;  // multi-gauge linked flash: 0=off 1=on (gauges turn red in sequence by
                                 // position, then all flash at threshold; logic in ui.c)
    uint8_t rc_enabled;          // RaceChrono BLE service: 0=off (minimal mode: Pair on MASTER only), 1=on (RC+Pair)
                                 // NOTE: new fields MUST be appended at the END of this struct;
                                 // see the load_blob grow logic comment in nvs_storage.c.
    uint8_t rsv_tpms_padding[3]; // old sizeof(cfg)=96; never interpret trailing padding as a binding
    nvs_tpms_binding_t tpms[NVS_TPMS_WHEELS]; // legacy MAC bindings, retained for blob compatibility
    char tpms_sensor_id[NVS_TPMS_WHEELS][7]; // offset 252; empty=unbound, six uppercase HEX digits otherwise
} nvs_user_cfg_t;

esp_err_t nvs_storage_init(void);

/* User config accessors. Update merges only fields changed since `base`, so
 * concurrent writers to unrelated fields cannot overwrite each other. */
esp_err_t nvs_cfg_snapshot(nvs_user_cfg_t *out);
esp_err_t nvs_cfg_update(const nvs_user_cfg_t *base, const nvs_user_cfg_t *updated);
/* Return a lock-protected value copy for read-only callers. */
nvs_user_cfg_t nvs_cfg_read(void);

// Per-item alarm threshold for the chart page (raw units; value>=threshold alarms; 32767=off). item = disp_item_t value.
int16_t nvs_chart_alarm_get(uint8_t item);
void    nvs_chart_alarm_set(uint8_t item, int16_t raw_threshold);

// Multi-gauge boot animation: 0=OFF, 1=RACE AS ONE, 2=VIDEO. Stored as a separate blob, not in the cfg struct.
uint8_t nvs_intro_enable_get(void);           // 0=OFF 1=RACE 2=VIDEO (SD OBV1, with legacy bootmedia fallback)
void    nvs_intro_enable_set(uint8_t en);
uint8_t nvs_device_position_get(void);        // 1/2/3
void    nvs_device_position_set(uint8_t pos);

// Downgrade mirror: vehicle_profile_idx (0=generic, 1..16 = old compiled-in table) is kept
// in sync with the active JSON profile so firmware rolled back by OTA picks the same car.
// Called by vp_manager on activation; idx > 16 is stored as 0. Saves only when changed.
esp_err_t nvs_cfg_vehicle_idx_mirror_set(uint8_t idx);
