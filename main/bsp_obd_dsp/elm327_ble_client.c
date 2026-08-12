#include "elm327_ble_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_defs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "app_obd_dsp/vehicle_custom_config.h"
#include "racechrono_ble_diy.h"
#include "ble_adv_util.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include "nvs_storage.h"

// UUID constants
#define UUID16_OBD_SERVICE      0xFFF0  // common ELM327 BLE adapters (FFF1 write / FFF2 notify)
#define UUID16_OBD_SERVICE_18F0  0x18F0  // IOS-Vlink / Vlink (2AF1 write / 2AF0 notify)
#define UUID16_OBD_SERVICE_FF12  0xFF12  // config service of some adapters (e.g. Viecar) (FF15 write / FF14 notify)
#define UUID16_OBD_WRITE_CHAR    0xFFF1
#define UUID16_CCCD              0x2902

static const char *TAG = "elm327_ble";

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;   // 0 is a valid real interface number; NONE distinguishes "not registered yet"
static uint16_t s_conn_id = 0xFFFF;
static esp_bd_addr_t s_peer_bda = {0};
static volatile bool s_connected = false;
static bool s_have_service = false;
static uint16_t s_service_start = 0x0001, s_service_end = 0xFFFF; // default: full range
static uint16_t s_all_attr_end = 0xFFFF; // tracks highest seen end handle
static bool s_have_18f0 = false;          // 0x18F0 service (the real OBD communication service of IOS-Vlink)
static uint16_t s_18f0_start = 0, s_18f0_end = 0;
static bool s_have_ff12 = false;          // 0xFF12 service (fallback)
static uint16_t s_ff12_start = 0, s_ff12_end = 0;
static uint16_t s_char_write_handle = 0; // FFF1
static uint16_t s_char_notify_handle = 0; // prefer FFF2, fall back to FFF1 if absent
static uint16_t s_cccd_handle = 0;
static esp_gatt_write_type_t s_write_type = ESP_GATT_WRITE_TYPE_RSP; // write type, auto-selected from characteristic properties
static elm327_ble_callbacks_t s_cbs = {0};
static char s_target_name[32] = "OBDII";
// Exact MAC match: once set, match_device_target accepts only this address and ignores the name (prevents misconnecting to same-named devices)
static esp_bd_addr_t s_target_bda = {0};
static bool s_target_bda_valid = false;

// ---- Scan mode related ----
static bool s_scan_only_mode = false;  // true=scan only, no connect
static ble_scan_found_cb_t s_scan_cb = NULL;
static ble_scan_result_t s_scan_list[BLE_SCAN_MAX_DEVICES];
static int s_scan_count = 0;
static bool s_ble_inited = false;  // whether the BLE stack has been initialized
static bool s_poll_task_started = false; // whether the poll task has been created
static TaskHandle_t s_poll_task_handle = NULL; // poll task handle, used for task-notification wakeup
static volatile bool s_notify_ready = false;   // set only after CCCD notify subscription completes; init/poll proceed based on this (prevents losing handshake responses)
static volatile int64_t s_last_obd_valid_us = 0; // time of the last valid OBD data; a timeout without data triggers self-heal re-init
static volatile bool s_got_valid_data = false;   // whether a genuinely valid frame has been parsed since the last poll round; set only by the parse path (not by do_elm_init), lets the poll task clear the self-heal counter
static uint8_t s_oil_query_mode = 0;     // current query mode index (0-2)

// Called when valid OBD data is parsed: refreshes the "valid data" timestamp and sets the flag.
// do_elm_init only sets the timestamp and does NOT go through here — just having initialized does not mean "data is flowing". Otherwise heal_attempts
// would be cleared right after every re-init, and "3 consecutive self-heals escalate to forced reconnect" could never be reached.
static inline void mark_obd_data_valid(void) {
    s_last_obd_valid_us = esp_timer_get_time();
    s_got_valid_data = true;
}

// ---- Data-driven override state ----
static const vehicle_override_t *s_ov = NULL;  // override config of the current vehicle profile
static const oil_formula_t *s_oil_formula_pri = NULL;   // primary oil-temp formula
static const oil_formula_t *s_oil_formula_sec = NULL;   // secondary oil-temp formula
static bool s_oil_use_override = false;  // true=use override formulas, false=use the legacy enum
static uint8_t s_oil_override_idx = 0;   // 0=primary, 1=secondary
static uint8_t s_oil_override_fail = 0;  // consecutive failure count of the current formula
#define OIL_OVERRIDE_FAIL_MAX 5
static int s_mode21_oil_idx = 33;        // Mode21 oil-temp byte index, adaptively updated
static int16_t s_last_mode21_oil = -100; // oil temp parsed from the last Mode21 response
static int s_mode21_hold_cnt = 0;        // ZC6 consistency hold count; consecutive noise frames, new value accepted only past a threshold
static int64_t s_last_mode21_oil_us = 0; // timestamp (us) of the last accepted oil-temp value

// ---- Vehicle-profile-based oil-temp query strategy ----
static oil_temp_query_mode_t s_oil_mode_priority[4] = {
    OIL_TEMP_MODE_PID_5C,
    OIL_TEMP_MODE_UDS_22_10_17,
    OIL_TEMP_MODE_TOYOTA_21_01,
};  // default priority, updated from the vehicle profile config after boot
static uint32_t s_oil_mode_fail_count[12] = {0};  // consecutive failure count per mode (poll idx 0~11)
#define OIL_MODE_FAIL_THRESHOLD 5  // switch to the next mode only after a mode fails this many times
#define OBD_POLL_SLOT_GAP_MS   30  // idle gap between poll slots (ms); was 100, lowered to raise refresh rate — too small overwhelms clone adapters
static bool s_vehicle_profile_inited = false;

// Oil-temp diagnostic stats
static struct {
    uint32_t mode0_ok;  // 01 5C success count
    uint32_t mode1_ok;  // 22 10 17 success count
    uint32_t mode2_ok;  // 21 01 success count
    uint32_t mode3_ok;  // 22 11 1F (Mazda) success count
    uint32_t mode4_ok;  // 22 13 10 (Mazda) success count
    uint32_t mode5_ok;  // CAN 0x441 (Porsche) success count
    uint32_t mode6_ok;  // 22 58 22 (MINI/BMW) success count
    uint32_t mode7_ok;  // 22 44 02 (BMW F-series) success count
    uint32_t mode8_ok;  // 22 03 F3 (BMW G-series) success count
    uint32_t mode9_ok;  // 22 44 02 G formula (BMW G-series) success count
    uint32_t mode10_ok; // 22 D0 02 (BMW G-series) success count
    uint32_t mode11_ok; // 22 11 1F (BMW, A-50) success count
    uint32_t mode0_fail;
    uint32_t mode1_fail;
    uint32_t mode2_fail;
    uint32_t mode3_fail;
    uint32_t mode4_fail;
    uint32_t mode5_fail;
    uint32_t mode6_fail;
    uint32_t mode7_fail;
    uint32_t mode8_fail;
    uint32_t mode9_fail;
    uint32_t mode10_fail;
    uint32_t mode11_fail;
    int16_t last_raw_temp; // raw temperature (unfiltered)
    int16_t last_filtered_temp; // filtered temperature
} s_oil_diag = {0};

static int8_t s_oil_temp_offset = 0;  // user calibration offset, in °C

// Global ready flag
static volatile bool s_elm_ready = true; // initially true so the first ATZ can be sent
static volatile bool s_expect_mode21 = false; // true=last command was 21 01, waiting for a 61 01 response
static volatile bool s_porsche_441_seen = false; // whether a 0x441 frame was successfully parsed during this monitor window
static volatile bool s_zc6_can_rpm_seen = false; // whether a 0x140 frame (ZC/N6 CAN RPM) was successfully parsed during this monitor window
static uint8_t s_can_rpm_fail_count = 0;         // consecutive failure count for CAN 140
#define CAN_RPM_FAIL_THRESHOLD 3                  // fall back to 01 0C after this many consecutive failures

// ---- ZC6 CAN continuous monitor mode (ATCM/ATCF + ATMA, parse each frame as it arrives) ----
static volatile bool s_zc6_can_monitor_active = false;
#define ZC6_CAN_MONITOR_BUF_SIZE 160
static char s_zc6_can_monitor_buf[ZC6_CAN_MONITOR_BUF_SIZE];
static size_t s_zc6_can_monitor_len = 0;
static int64_t s_zc6_can_monitor_entered_us = 0;   // time the monitor was entered
static int64_t s_zc6_can_monitor_last_sample_us = 0; // time of the last successful parse
static uint32_t s_zc6_can_monitor_obd_cycle = 0;   // OBD query cycle counter while monitoring
#define ZC6_CAN_OBD_INTERVAL 50                     // exit ATMA every 50 cycles (~6s) to query OBD PIDs once
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd);

// Accumulation buffer for multi-packet responses (21 01 responses span multiple BLE packets)
#define ACCUM_BUF_SIZE 512
static char s_accum_buf[ACCUM_BUF_SIZE];
static size_t s_accum_len = 0;
static int64_t s_accum_start_us = 0; // accumulation start time (us)

// ---- Auto protocol detection ----
static volatile int s_protocol_detect_idx = -1;  // -1=not detecting, 0-10=protocol number being tried
static volatile bool s_protocol_detect_got_response = false;  // whether a valid response was received
static volatile int32_t s_protocol_detect_rpm = -1;  // detected RPM value (-1=none)

// ---- Oil-temp query mode conversion ----
// Convert oil_temp_query_mode_t to poll index (0-2)
static inline uint8_t oil_mode_to_poll_idx(oil_temp_query_mode_t mode) {
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C: return 0;
        case OIL_TEMP_MODE_UDS_22_10_17: return 1;
        case OIL_TEMP_MODE_TOYOTA_21_01: return 2;
        case OIL_TEMP_MODE_MAZDA_22_111F: return 3;
        case OIL_TEMP_MODE_MAZDA_22_1310: return 4;
        case OIL_TEMP_MODE_PORSCHE_CAN_441: return 5;
        case OIL_TEMP_MODE_MINI_22_5822: return 6;
        case OIL_TEMP_MODE_BMW_22_4402: return 7;
        case OIL_TEMP_MODE_BMW_22_03F3: return 8;
        case OIL_TEMP_MODE_BMW_G_22_4402: return 9;
        case OIL_TEMP_MODE_BMW_22_D002: return 10;
        case OIL_TEMP_MODE_BMW_22_111F: return 11;
        default: return 0;
    }
}

static const char *get_vehicle_fixed_header_cmd(void) {
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->obd_functional_addr) {
        return "ATSH7DF\r"; // functional addressing (vehicle-wide broadcast), same as phone apps; BMW needs this, otherwise physical 7E0 may not respond
    }
    return "ATSH7E0\r";     // physical addressing to the engine ECU; Subaru/default
}

// Initialize the oil-temp query strategy (reads the primary/secondary/tertiary priority chain from the vehicle profile config)
static void init_oil_temp_strategy(void) {
    // Take the profile's full priority chain: after the primary fails consecutively (threshold times), fall back to secondary, tertiary.
    const oil_temp_strategy_t *st = vehicle_profile_get_oil_temp_strategy();
    s_oil_mode_priority[0] = st ? st->primary    : OIL_TEMP_MODE_PID_5C;
    s_oil_mode_priority[1] = st ? st->secondary  : OIL_TEMP_MODE_NONE;
    s_oil_mode_priority[2] = st ? st->tertiary   : OIL_TEMP_MODE_NONE;
    s_oil_mode_priority[3] = st ? st->quaternary : OIL_TEMP_MODE_NONE;

    // ---- Data-driven override init ----
    s_ov = vehicle_profile_get_override();
    s_oil_formula_pri = s_ov ? s_ov->oil_primary : NULL;
    s_oil_formula_sec = s_ov ? s_ov->oil_secondary : NULL;
    s_oil_use_override = (s_oil_formula_pri != NULL);
    s_oil_override_idx = 0;
    s_oil_override_fail = 0;

    if (s_oil_mode_priority[0] == OIL_TEMP_MODE_TOYOTA_21_01) {
        // ZC/N6 fixed at d[33]
        s_mode21_oil_idx = 33;
    }
    s_last_mode21_oil = -100;
    s_last_mode21_oil_us = 0;
    s_mode21_hold_cnt = 0;

    // Reset failure counts
    memset(s_oil_mode_fail_count, 0, sizeof(s_oil_mode_fail_count));
    s_oil_query_mode = 0;
    s_vehicle_profile_inited = true;

    const vehicle_profile_t *profile = vehicle_profile_get_active();
    ESP_LOGI(TAG, "Oil temp strategy for [%s]: Primary=%d",
             profile ? profile->name : "UNKNOWN", s_oil_mode_priority[0]);
}

// Select the next query mode based on the current strategy
static oil_temp_query_mode_t get_next_oil_query_mode(uint8_t *poll_idx) {
    if (!s_vehicle_profile_inited) {
        init_oil_temp_strategy();
    }
    
    // Walk the priority list and find the first valid mode that hasn't failed too much
    for (int i = 0; i < 4; i++) {
        oil_temp_query_mode_t mode = s_oil_mode_priority[i];
        if (mode == OIL_TEMP_MODE_NONE) continue;
        
        uint8_t idx = oil_mode_to_poll_idx(mode);
        // Use this mode if its failure count is below the threshold
        if (s_oil_mode_fail_count[idx] < OIL_MODE_FAIL_THRESHOLD) {
            *poll_idx = idx;
            return mode;
        }
    }
    
    // All modes have failed too much; reset failure counts and use the primary mode
    memset(s_oil_mode_fail_count, 0, sizeof(s_oil_mode_fail_count));
    *poll_idx = oil_mode_to_poll_idx(s_oil_mode_priority[0]);
    return s_oil_mode_priority[0];
}

// Record oil-temp query success/failure (internal use)
static void record_oil_temp_success(oil_temp_query_mode_t mode) {
    uint8_t idx = oil_mode_to_poll_idx(mode);
    s_oil_mode_fail_count[idx] = 0;  // success, clear the failure count

    // Update diagnostic stats
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C:
            s_oil_diag.mode0_ok++;
            break;
        case OIL_TEMP_MODE_UDS_22_10_17:
            s_oil_diag.mode1_ok++;
            break;
        case OIL_TEMP_MODE_TOYOTA_21_01:
            s_oil_diag.mode2_ok++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_111F:
            s_oil_diag.mode3_ok++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_1310:
            s_oil_diag.mode4_ok++;
            break;
        case OIL_TEMP_MODE_PORSCHE_CAN_441:
            s_oil_diag.mode5_ok++;
            break;
        case OIL_TEMP_MODE_MINI_22_5822:
            s_oil_diag.mode6_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_4402:
            s_oil_diag.mode7_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_03F3:
            s_oil_diag.mode8_ok++;
            break;
        case OIL_TEMP_MODE_BMW_G_22_4402:
            s_oil_diag.mode9_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_D002:
            s_oil_diag.mode10_ok++;
            break;
        case OIL_TEMP_MODE_BMW_22_111F:
            s_oil_diag.mode11_ok++;
            break;
        default:
            break;
    }
    ESP_LOGD(TAG, "Oil temp query SUCCESS for mode %u (fail_count reset to 0)", mode);
}

static void record_oil_temp_failure(oil_temp_query_mode_t mode) {
    uint8_t idx = oil_mode_to_poll_idx(mode);
    s_oil_mode_fail_count[idx]++;

    // Update diagnostic stats
    switch (mode) {
        case OIL_TEMP_MODE_PID_5C:
            s_oil_diag.mode0_fail++;
            break;
        case OIL_TEMP_MODE_UDS_22_10_17:
            s_oil_diag.mode1_fail++;
            break;
        case OIL_TEMP_MODE_TOYOTA_21_01:
            s_oil_diag.mode2_fail++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_111F:
            s_oil_diag.mode3_fail++;
            break;
        case OIL_TEMP_MODE_MAZDA_22_1310:
            s_oil_diag.mode4_fail++;
            break;
        case OIL_TEMP_MODE_PORSCHE_CAN_441:
            s_oil_diag.mode5_fail++;
            break;
        case OIL_TEMP_MODE_MINI_22_5822:
            s_oil_diag.mode6_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_4402:
            s_oil_diag.mode7_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_03F3:
            s_oil_diag.mode8_fail++;
            break;
        case OIL_TEMP_MODE_BMW_G_22_4402:
            s_oil_diag.mode9_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_D002:
            s_oil_diag.mode10_fail++;
            break;
        case OIL_TEMP_MODE_BMW_22_111F:
            s_oil_diag.mode11_fail++;
            break;
        default:
            break;
    }
    ESP_LOGD(TAG, "Oil temp query FAILED for mode %u (fail_count now %u)", mode, s_oil_mode_fail_count[idx]);
}

// Default callbacks and poll task (optional)
static void default_on_connected(void) { ESP_LOGD(TAG, "OBD BLE connected"); }
static void default_on_disconnected(void) { ESP_LOGD(TAG, "OBD BLE disconnected"); }
static void default_on_raw_notify(const uint8_t *data, size_t len) {
    // Print raw data only at debug level (no output in production)
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        char printbuf[128] = {0};
        size_t plen = (len < sizeof(printbuf)-1) ? len : sizeof(printbuf)-1;
        for (size_t i = 0; i < plen; i++) {
            printbuf[i] = (data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.';
        }
        ESP_LOGD(TAG, "RAW[%d]: %s", (int)len, printbuf);
    }
    // Receiving '>' means the ELM is ready; the next command can be sent
    // xTaskNotify wakes the poll task immediately, avoiding the 10ms polling overhead
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '>') {
            s_elm_ready = true;
            if (s_poll_task_handle) xTaskNotify(s_poll_task_handle, 0, eNoAction);
            break;
        }
    }
}

// ---- Protocol auto-detection ----
// Try all protocols (1-11); send 01 0C (read RPM) to check whether the protocol is valid
static int elm327_auto_detect_protocol(void) {
    ESP_LOGD(TAG, "=== Starting protocol auto-detect ===");

    // Send generic init commands first (no protocol selection involved)
    const char *init_cmds[] = {
        "ATZ\r",        // reset
        "ATE0\r",       // Echo off
        "ATAT1\r",      // adaptive timing
        "ATST 19\r",    // set timeout
    };
    
    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Try protocols 1-11
    for (int proto = 1; proto <= 11; proto++) {
        ESP_LOGD(TAG, "[DETECT] Trying protocol %d...", proto);

        // Set the protocol
        char atsp_cmd[16];
        snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", proto);
        elm327_ble_send_ascii_blocking(atsp_cmd);
        vTaskDelay(pdMS_TO_TICKS(50));

        // Init detection state
        s_protocol_detect_idx = proto;
        s_protocol_detect_got_response = false;
        s_protocol_detect_rpm = -1;

        // Send test command 01 0C (read RPM)
        elm327_ble_send_ascii_blocking("01 0C\r");
        esp_log_level_t prev_level = esp_log_level_get(TAG);
        esp_log_level_set(TAG, ESP_LOG_INFO);
        ESP_LOGD(TAG, "[DETECT] Sent 01 0C, waiting...");
        esp_log_level_set(TAG, prev_level);
        
        // Wait for a response, up to 2 seconds
        uint32_t wait_ms = 0;
        while (wait_ms < 2000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_ms += 50;

            if (s_protocol_detect_got_response) {
                // Success!
                s_protocol_detect_idx = -1;  // exit detection mode
                ESP_LOGD(TAG, "[DETECT] Protocol %d: SUCCESS! (RPM=%ld)", proto, s_protocol_detect_rpm);
                return proto;
            }
        }
        
        ESP_LOGW(TAG, "[DETECT] Protocol %d: No valid response (timeout)", proto);
    }
    
    s_protocol_detect_idx = -1;  // exit detection mode
    ESP_LOGW(TAG, "=== Protocol auto-detect FAILED ===");
    return 0;  // 0 means detection failed; the default protocol 6 will be used
}

static void default_on_parsed_rpm(uint16_t rpm) { ESP_LOGD(TAG, "RPM: %u", rpm); obd_data_set_rpm(rpm); }
static void default_on_parsed_speed(uint8_t kmh) {
    // Speed correction: multiply by the current profile's speed_scale (e.g. BMW X1 ×1.0606); unset/≤0 treated as 1.0
    const vehicle_profile_t *p = vehicle_profile_get_active();
    float sc = (p && p->speed_scale > 0.0f) ? p->speed_scale : 1.0f;
    int32_t v = (int32_t)((float)kmh * sc + 0.5f);
    if (v > 255) v = 255;
    // CAN profiles: force speed=0 when RPM<800, so CAN-bus noise doesn't creep the speed up while stationary
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->can_broadcast_mode && obd_data_get_rpm() < 800)
        v = 0;
    // All profiles: ≤2km/h counts as stationary, to avoid low-speed noise
    if (v <= 2) v = 0;
    ESP_LOGD(TAG, "SPEED: %u -> %d km/h (x%.4f)", kmh, (int)v, sc);
    obd_data_set_speed((uint8_t)v);
}
static void default_on_parsed_coolant_temp(uint32_t coolant_temp) { ESP_LOGD(TAG, "CLT: %u C", coolant_temp); obd_data_set_coolant_temp((int16_t)coolant_temp); }
static void default_on_parsed_intake_temp(uint32_t intake_temp) { ESP_LOGD(TAG, "IAT: %u C", intake_temp); obd_data_set_intake_temp((int16_t)intake_temp); }

// Inline helper: apply the oil-temp offset before storing
static inline void obd_data_set_oil_temp_with_offset(int16_t temp) {
    int16_t adjusted = temp + s_oil_temp_offset;
    // Make sure it stays in the valid range
    if (adjusted < -20) adjusted = -20;
    if (adjusted > 150) adjusted = 150;
    if (s_oil_temp_offset != 0) {
        ESP_LOGD(TAG, "OIL offset applied: %d + %d = %d", temp, s_oil_temp_offset, adjusted);
    }
    obd_data_set_oil_temp(adjusted);
}

// float → int16_t rounding (safe for positive values; negative-value precision is a non-issue in the oil-temp range)
static inline int16_t oil_f2i(float f) { return (int16_t)(f + 0.5f); }

static void default_on_parsed_oil_temp(uint32_t oil_temp)
{
    // Track internally as float so integer truncation doesn't stall a 1°C change forever.
    // Example: filtered=90, raw=91 → 0.65*90+0.35*91=90.35 → rounds to display 90,
    //     but the float keeps accumulating; another 91 → 0.65*90.35+0.35*91=90.578 → displays 91 ✓
    static float s_oil_filtered = -100.0f;
    static int16_t s_oil_pending = -100;
    static uint8_t s_oil_pending_cnt = 0;

    int16_t in = (int16_t)oil_temp;
    s_oil_diag.last_raw_temp = in;

    if (in < -20 || in > 150) {
        ESP_LOGD(TAG, "OIL: Out of range raw=%d", in);
        return;
    }

    // 1. Init
    if (s_oil_filtered <= -40.0f) {
        s_oil_filtered = (float)in;
        s_oil_pending = in;
        s_oil_pending_cnt = 1;
        s_oil_diag.last_filtered_temp = in;
        ESP_LOGI(TAG, "OIL: Init with raw=%d", in);
        obd_data_set_oil_temp_with_offset(in);
        return;
    }

    float fdiff = s_oil_filtered - (float)in;
    int diff = (int)(fdiff >= 0 ? fdiff : -fdiff);

    // 2. Small change (<=5°C): weighted average; float precision ensures 1°C drifts accumulate correctly
    if (diff <= 5) {
        s_oil_pending = -100;
        s_oil_pending_cnt = 0;
        s_oil_filtered = 0.65f * s_oil_filtered + 0.35f * (float)in;
        int16_t disp = oil_f2i(s_oil_filtered);
        s_oil_diag.last_filtered_temp = disp;
        ESP_LOGD(TAG, "OIL: raw=%d filtered=%.2f disp=%d", in, s_oil_filtered, disp);
        obd_data_set_oil_temp_with_offset(disp);
        return;
    }

    // 3. Medium change (5~15°C): accept after 4 confirmations
    if (diff <= 15) {
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            ESP_LOGD(TAG, "OIL: Medium spike first occurrence raw=%d, need confirmation", in);
            return;
        }
        if (s_oil_pending_cnt >= 4) {
            s_oil_filtered = 0.5f * s_oil_filtered + 0.5f * (float)in;
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            int16_t disp = oil_f2i(s_oil_filtered);
            s_oil_diag.last_filtered_temp = disp;
            ESP_LOGI(TAG, "OIL: Medium change confirmed raw=%d filtered=%.2f disp=%d", in, s_oil_filtered, disp);
            obd_data_set_oil_temp_with_offset(disp);
        } else {
            ESP_LOGD(TAG, "OIL: Medium spike pending (%u/%d)", s_oil_pending_cnt, 4);
        }
        return;
    }

    // 4. Large change (>15°C): accept directly after 3 confirmations
    ESP_LOGW(TAG, "OIL: Large spike filtered=%.1f raw=%d (Δ=%d)", s_oil_filtered, in, diff);
    if (diff >= 20) {
        if (s_oil_pending == in) {
            s_oil_pending_cnt++;
        } else {
            s_oil_pending = in;
            s_oil_pending_cnt = 1;
            return;
        }
        if (s_oil_pending_cnt >= 3) {
            s_oil_filtered = (float)in;
            s_oil_pending = -100;
            s_oil_pending_cnt = 0;
            s_oil_diag.last_filtered_temp = in;
            ESP_LOGI(TAG, "OIL: Large change ACCEPTED raw=%d (confirmed 3x)", in);
            obd_data_set_oil_temp_with_offset(in);
        }
    }
}
static void default_on_parsed_load_pct(uint32_t load_pct) { ESP_LOGD(TAG, "LOAD: %u%%", load_pct); obd_data_set_load_pct((int16_t)load_pct); }
static void default_on_parsed_control_module_voltage(uint32_t bat_mv) { ESP_LOGD(TAG, "BAT: %u.%uV", bat_mv/1000, (bat_mv%1000)/100); obd_data_set_bat_mv((int32_t)bat_mv); }
static void default_on_parsed_throttle_position(uint32_t tps_pct) { ESP_LOGD(TAG, "TPS: %u%%", tps_pct); obd_data_set_tps((int16_t)tps_pct); }
static void default_on_parsed_gear(int8_t gear) {
    // BMW G CAN broadcast gear (0x3F9 byte6 nibble), raw−4 mapping:
    //   3→R, 4→N, 5→1, 6→2, …  (0-2 reserved/Park etc.)
    if (gear == 3) {
        obd_data_set_gear(-1);  // R
    } else if (gear == 4) {
        obd_data_set_gear(GEAR_NEUTRAL);  // N/P
    } else if (gear >= 5 && gear <= 12) {
        obd_data_set_gear((int8_t)(gear - 4));  // 5→1, 6→2, ...
    } else {
        obd_data_set_gear(127);  // invalid/unknown
    }
}
// MAP(kPa) → turbo gauge pressure (0.1bar): gauge = (MAP - atmospheric ≈100kPa), 10kPa = 0.1bar
static void default_on_parsed_manifold_pressure(uint32_t map_kpa) {
    int16_t boost_x10 = (int16_t)(((int32_t)map_kpa - 100) / 10);
    if (boost_x10 < 0) boost_x10 = 0; // don't display negative pressure (vacuum), floor at 0
    ESP_LOGD(TAG, "MAP: %u kPa -> boost %d.%d bar", map_kpa, boost_x10/10, boost_x10%10);
    obd_data_set_boost_x10(boost_x10);
}

// PID 01 44: Commanded Equivalence Ratio (λ), formula: (A*256+B)/32768, range 0~<2
// λ=1.0 stoichiometric AFR (gasoline ~14.7:1), λ<1 rich, λ>1 lean
// Conversion: AFR = λ × 14.7, stored ×100 (1470 = 14.70:1)
static void default_on_parsed_afr(uint32_t afr_x100) {
    ESP_LOGD(TAG, "AFR: %d.%02d:1 (λ=%.3f)", afr_x100/100, afr_x100%100, (float)afr_x100/1470.0f);
    obd_data_set_afr_x100((int16_t)afr_x100);
}

// Porsche 997.2/987.2: oil temp/pressure live in CAN broadcast frame 0x441; capturing them requires ELM327 monitor mode.
// Sequence: filter to receive only 441 → headers on → monitor a few frames → stop → restore (headers off / auto receive-address).
// Frame parsing happens in the notify callback (see the "441 " branch); this function only issues the monitor commands in sequence.
// Note: monitor mode differs from normal request/response and depends on the adapter (cheap clones may not support ATMA/ATCRA).
static void porsche_read_can_441(void) {
    // Oil temp/pressure change slowly (on the order of minutes); no need to run this 520ms blocking CAN monitor every round.
    // Skip it every few rounds; the saved time lets RPM/other slots run faster, and the display keeps the last reading.
    const uint8_t skip_rounds = 4;
    static uint8_t s_round = 0;
    if (++s_round < skip_rounds) return;
    s_round = 0;

    elm327_ble_send_ascii_blocking("AT CRA 441\r"); // receive filter: accept only ID=0x441
    elm327_ble_send_ascii_blocking("AT H1\r");        // headers on: monitor output includes IDs, making 441 identifiable
    // Start monitoring (ATMA streams frames continuously and produces no '>'; manage the ready flag manually)
    uint8_t cmd[8];
    size_t n = elm327_ble_ascii_cmd_to_bytes("AT MA\r", cmd, sizeof(cmd));
    s_porsche_441_seen = false;
    if (n) { s_elm_ready = false; elm327_ble_send_command(cmd, n); }
    vTaskDelay(pdMS_TO_TICKS(400));   // monitor window stretched to 400ms to accommodate slowly broadcast 441 frames
    uint8_t stop = '\r';
    elm327_ble_send_command(&stop, 1); // any character stops monitoring → ELM flushes frames + '>' (triggers parsing)
    vTaskDelay(pdMS_TO_TICKS(120));    // wait for the notify callback to finish parsing the frame
    // Diagnostics: explicitly log this round's monitor result for serial debugging (captured = parsing issue; not captured = no 441 on bus / adapter doesn't support ATMA)
    if (s_porsche_441_seen) {
        ESP_LOGD(TAG, "[441] frame captured & parsed OK");
    } else {
        ESP_LOGW(TAG, "[441] NO 441 frame in window (check FULL[] log above: ATMA returned what?)");
        record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
    }
    // Restore: headers off, auto receive-address again (otherwise later normal PID responses get filtered/misparsed)
    elm327_ble_send_ascii_blocking("AT H0\r");
    elm327_ble_send_ascii_blocking("AT AR\r");
}

// ZC/N6: RPM is in CAN broadcast frame 0x140 (bits 16-29, 14bit LE, direct rpm).
// ---- ZC/N6 CAN continuous monitor: enter/exit/byte-wise feed/line-wise parse ----

static void zc6_can_monitor_enter(void)
{
    // No filter; all frames pass through, and software parses only the CAN IDs in the override rules
    const char *cmds[] = {
        "ATE0\r", "ATL0\r", "ATS1\r", "ATH1\r", "ATMA\r",
    };
    s_zc6_can_monitor_active = false;
    s_zc6_can_monitor_len = 0;
    s_zc6_can_monitor_buf[0] = '\0';
    s_zc6_can_monitor_entered_us = 0;
    s_zc6_can_monitor_last_sample_us = 0;
    s_accum_len = 0;
    s_accum_buf[0] = '\0';

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        elm327_ble_send_ascii_blocking(cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(i + 1 == sizeof(cmds) / sizeof(cmds[0]) ? 80 : 30));
    }
    s_zc6_can_monitor_active = true;
    s_zc6_can_monitor_entered_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[ZC/N6 CAN] Entered ATMA monitor (0x140/141/0D1)");
}

static void zc6_can_monitor_exit(void)
{
    s_zc6_can_monitor_active = false;
    s_elm_ready = true;
    uint8_t stop = '\r';
    elm327_ble_send_command(&stop, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    s_accum_len = 0;
    s_accum_buf[0] = '\0';
    // Light restore: clear filter + headers off, no full ATZ reset (saves ~500ms)
    elm327_ble_send_ascii_blocking("AT AR\r");
    elm327_ble_send_ascii_blocking("AT H0\r");
    ESP_LOGI(TAG, "[ZC/N6 CAN] Exited ATMA (light restore)");
}

// Line-wise parse: ZC/N6 (0x140/0x360) + ZD8 (0x40/0x345); skipped automatically if the frame is not on the bus
static bool zc6_can_monitor_parse_line(const char *line)
{
    // Generic CAN frame parse: extract CAN ID + data from the ATMA line and apply the override rules
    const vehicle_override_t *ov = vehicle_profile_get_override();
    if (!ov || !ov->can_rules || ov->can_rule_count == 0) {
        ESP_LOGD(TAG, "[CAN] no override/rules for active profile");
        return false;
    }

    // ---- Diagnostics: sample-print CAN IDs from all ATMA lines (first 60 lines) ----
    {
        static uint32_t s_can_line_count = 0;
        static uint16_t s_seen_ids[32] = {0};
        static uint8_t s_seen_count = 0;
        if (s_can_line_count < 60) {
            uint16_t id = 0;
            int n = sscanf(line, "%hx ", &id);
            if (n == 1 && id > 0 && id < 0x800) {
                bool dup = false;
                for (uint8_t j = 0; j < s_seen_count; j++)
                    if (s_seen_ids[j] == id) { dup = true; break; }
                if (!dup && s_seen_count < 32) {
                    s_seen_ids[s_seen_count++] = id;
                    ESP_LOGI(TAG, "[CAN] new ID: 0x%03X (line=%lu)", id, (unsigned long)s_can_line_count);
                }
            }
        }
        s_can_line_count++;
    }

    // Parse the CAN ID at the line start (ATMA lines have the fixed format "<ID> <D0> <D1> ... <Dn>", ID first).
    // Note: must NOT strstr the whole line for an "<id> " substring like before — if some data byte's hex
    // text happens to equal another monitored ID (e.g. oil-temp byte =0x40 while 0x040 is also monitored),
    // it false-matches inside the data segment, mislabels this frame as another ID while the real ID is skipped,
    // and the corresponding channel never reads anything.
    uint16_t line_id = 0;
    if (sscanf(line, "%hx ", &line_id) != 1) return false;

    bool id_watched = false;
    for (uint8_t i = 0; i < ov->can_rule_count; i++) {
        if (ov->can_rules[i].can_id == line_id) { id_watched = true; break; }
    }
    if (!id_watched) return false;

    uint8_t data[8] = {0};
    int vals = sscanf(line, "%*x %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
                      &data[0],&data[1],&data[2],&data[3],
                      &data[4],&data[5],&data[6],&data[7]);
    if (vals < 1) return false;

    float channels[CH_COUNT];
    for (int c = 0; c < CH_COUNT; c++) channels[c] = -32768.0f;
    can_apply_rules(ov->can_rules, ov->can_rule_count, line_id, data, channels);

    if (channels[CH_RPM] >= 0 && s_cbs.on_parsed_rpm)
        s_cbs.on_parsed_rpm((uint16_t)channels[CH_RPM]);
    if (channels[CH_SPEED] >= 0 && s_cbs.on_parsed_speed_kmh)
        s_cbs.on_parsed_speed_kmh((uint8_t)channels[CH_SPEED]);
    if (channels[CH_OIL_TEMP] > -40 && channels[CH_OIL_TEMP] <= 215 && s_cbs.on_parsed_oil_temp)
        s_cbs.on_parsed_oil_temp((uint32_t)(int16_t)channels[CH_OIL_TEMP]);
    if (channels[CH_COOLANT] > -40 && channels[CH_COOLANT] <= 215 && s_cbs.on_parsed_coolant_temp)
        s_cbs.on_parsed_coolant_temp((uint32_t)(int16_t)channels[CH_COOLANT]);
    if (channels[CH_TPS] >= 0 && s_cbs.on_parsed_throttle_position)
        s_cbs.on_parsed_throttle_position((uint32_t)channels[CH_TPS]);
    if (channels[CH_GEAR] > 0 && channels[CH_GEAR] < 127 && s_cbs.on_parsed_gear)
        s_cbs.on_parsed_gear((int8_t)channels[CH_GEAR]);

    s_zc6_can_monitor_last_sample_us = esp_timer_get_time();
    mark_obd_data_valid();
    return true;
}

// Byte-wise feed: split into lines on \r\n and call parse_line for each line
static void zc6_can_monitor_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char ch = (char)data[i];
        if (ch == '>') { s_elm_ready = true; continue; }
        if (ch == '\r' || ch == '\n') {
            if (s_zc6_can_monitor_len > 0) {
                s_zc6_can_monitor_buf[s_zc6_can_monitor_len] = '\0';
                zc6_can_monitor_parse_line(s_zc6_can_monitor_buf);
                s_zc6_can_monitor_len = 0;
            }
            continue;
        }
        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7E) continue;
        if (s_zc6_can_monitor_len + 1 >= ZC6_CAN_MONITOR_BUF_SIZE)
            s_zc6_can_monitor_len = 0;
        s_zc6_can_monitor_buf[s_zc6_can_monitor_len++] = ch;
    }
}

// ELM327 init sequence (protocol selection + AT config + bus warm-up + oil-temp strategy).
// Called on every (re)connect and only after the notify subscription is ready, so handshake responses aren't lost before subscribing.
static void do_elm_init(void) {
    char atsp_cmd[16];
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    // ---- Protocol selection ----
    uint8_t protocol_to_use = cfg->protocol;
    // Vehicle-forced protocol takes priority (e.g. BMW/Porsche auto-detect is unstable; lock to protocol 6 and skip detection)
    const vehicle_profile_t *vp_proto = vehicle_profile_get_active();
    if (vp_proto && vp_proto->forced_protocol != 0) {
        protocol_to_use = vp_proto->forced_protocol;
        ESP_LOGD(TAG, "Vehicle '%s' forces protocol %d (skip auto-detect)", vp_proto->name, protocol_to_use);
    } else if (protocol_to_use == 0) {
        // Auto protocol detection
        ESP_LOGD(TAG, "Protocol auto-detect enabled (current NVS: 0-auto)");
        int detected_proto = elm327_auto_detect_protocol();
        if (detected_proto > 0) {
            protocol_to_use = (uint8_t)detected_proto;
            nvs_user_cfg_t new_cfg = *cfg;
            new_cfg.protocol = protocol_to_use;
            nvs_cfg_set(&new_cfg);
            ESP_LOGD(TAG, "Protocol auto-detect SUCCESS! Saving protocol %d to NVS", protocol_to_use);
        } else {
            protocol_to_use = 6;
            ESP_LOGW(TAG, "Protocol auto-detect FAILED, using fallback protocol 6");
        }
    }

    snprintf(atsp_cmd, sizeof(atsp_cmd), "ATSP%d\r", protocol_to_use);
    const char *fixed_header_cmd = get_vehicle_fixed_header_cmd();
    // Timeout command: prefer the profile's obd_timeout, default 0x19
    char atst_cmd[12];
    uint8_t timeout_val = (vp_proto && vp_proto->obd_timeout) ? vp_proto->obd_timeout : 0x19;
    snprintf(atst_cmd, sizeof(atst_cmd), "ATST %02X\r", timeout_val);
    const char *init_cmds[] = {
        "ATZ\r", "ATE0\r", "ATL0\r", "ATS1\r", "ATH0\r", "ATAT1\r", atst_cmd,
        atsp_cmd, fixed_header_cmd,
    };
    for (size_t i = 0; i < (sizeof(init_cmds) / sizeof(init_cmds[0])); ++i) {
        elm327_ble_send_ascii_blocking(init_cmds[i]);
        ESP_LOGD(TAG, " AT init Cmd send %s", init_cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    // Bus warm-up: send 01 00 a few extra times to give the vehicle CAN/ELM protocol time to handshake (the bus may not be awake on a cold start)
    for (int probe = 0; probe < 3; ++probe) {
        elm327_ble_send_ascii_blocking("01 00\r");
        ESP_LOGD(TAG, " CMD 01 00 probe #%d", probe);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    // ---- Init the oil-temp query strategy (based on vehicle profile config) ----
    init_oil_temp_strategy();
    s_can_rpm_fail_count = 0;  // retry CAN RPM after reconnect
    const vehicle_profile_t *active_profile = vehicle_profile_get_active();
    ESP_LOGD(TAG, "Active vehicle profile: %s", active_profile ? active_profile->name : "Unknown");
    s_last_obd_valid_us = esp_timer_get_time();   // give a fresh "valid data" baseline so self-heal doesn't trigger right after init
}

static void obd_poll_task(void *arg) {
    s_poll_task_handle = xTaskGetCurrentTaskHandle();
    esp_task_wdt_add(NULL);  // register with the watchdog
    uint32_t tick_count = 0;
    bool inited = false;
    uint8_t heal_attempts = 0;   // consecutive self-heal count; escalates to a forced reconnect if resending ATZ a few times still yields no data

    // 8-slot poll: 0=RPM, 1=IAT, 2=Speed, 3=CLT, 4=Load(0x04), 5=TPS(0x11), 6=OIL(vehicle strategy), 7=BAT(0x42)
    while (1)
    {
        esp_task_wdt_reset();  // feed the watchdog
        // Showroom mode: pause OBD polling to avoid overwriting the dummy data
        extern bool ui_showroom_is_active(void);
        if (ui_showroom_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // Not connected or notify subscription not ready → mark for re-init and wait
        if (!s_connected || !s_notify_ready) {
            inited = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        // Init only after the notify subscription is ready following a (re)connect; re-runs on every reconnect (fixes "must disconnect/reconnect to get readings")
        if (!inited) {
            vTaskDelay(pdMS_TO_TICKS(300));   // give the subscription a bit more time to settle
            do_elm_init();
            inited = true;
            tick_count = 0;
            continue;
        }
        // Real valid data is flowing (any frame parsed since the last round) → clear the self-heal counter.
        // Must NOT judge by timestamp: do_elm_init also refreshes the timestamp; if that counted as "data flowing",
        // heal_attempts would be cleared right after every re-init and the "3 consecutive self-heals → forced reconnect" escalation could never run.
        if (s_got_valid_data) {
            s_got_valid_data = false;
            heal_attempts = 0;
        }
        // Self-heal: no valid data for >10s straight (covers no response / SEARCHING / UNABLE TO CONNECT / NO DATA).
        if ((esp_timer_get_time() - s_last_obd_valid_us) > 10000000) {
            s_last_obd_valid_us = esp_timer_get_time();
            heal_attempts++;
            if (heal_attempts >= 3) {
                // Retried ATZ a few times with still no data → force-close BLE; the DISCONNECT callback auto-reconnects,
                // equivalent to automatically doing a "manual disconnect/reconnect" (re-subscribe notify + re-run init; by then the bus is usually awake).
                ESP_LOGW(TAG, "Self-heal escalate: force BLE reconnect (re-init didn't help)");
                heal_attempts = 0;
                inited = false;
                esp_ble_gattc_close(s_gattc_if, s_conn_id);
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            ESP_LOGW(TAG, "No valid OBD data >10s, re-init ELM (self-heal #%u)...", heal_attempts);
            inited = false;
            continue;
        }
        // ---- ZC/N6 CAN mode: RPM via CAN 0x140, everything else via standard OBD (identical to regular ZC/N6) ----
        // Two phases: ATMA monitor for RPM → exit every N cycles, run one full standard OBD poll round → back to ATMA
        const vehicle_profile_t *vp_poll = vehicle_profile_get_active();
        bool can_broadcast = vp_poll && vp_poll->can_broadcast_mode;
        static bool s_zc_can_obd_phase = false;  // true=running the standard OBD poll

        if (can_broadcast && !s_zc_can_obd_phase) {
            // ---- ATMA phase: receive only 0x140 RPM ----
            if (!s_zc6_can_monitor_active) {
                zc6_can_monitor_enter();
                s_zc6_can_monitor_obd_cycle = 0;
            }
            // Self-heal
            if (s_zc6_can_monitor_entered_us > 0 &&
                (esp_timer_get_time() - s_last_obd_valid_us) > 10000000) {
                ESP_LOGW(TAG, "[ZC/N6 CAN] No data >10s, re-enter ATMA");
                zc6_can_monitor_exit();
                zc6_can_monitor_enter();
                s_zc6_can_monitor_obd_cycle = 0;
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            s_zc6_can_monitor_obd_cycle++;
            if (s_zc6_can_monitor_obd_cycle >= ZC6_CAN_OBD_INTERVAL) {
                // Switch to the OBD phase
                zc6_can_monitor_exit();
                s_zc_can_obd_phase = true;
                tick_count = 1;  // start from slot1, skip slot0 (RPM is already provided via CAN)
            }
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        // ---- Standard OBD polling (ZC/N6 CAN's OBD phase, or non-CAN profiles) ----
        // Fully reuses the switch(tick_count) below, identical to regular ZC/N6
        if (can_broadcast && s_zc_can_obd_phase && tick_count == 0) {
            // One standard OBD round done, return to the ATMA phase
            s_zc_can_obd_phase = false;
            zc6_can_monitor_enter();
            s_zc6_can_monitor_obd_cycle = 0;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        {
        switch(tick_count)
        {
            case 0:// Engine RPM
                elm327_ble_send_ascii_blocking("01 0C\r");
                ESP_LOGD(TAG, "Send 01 0C");
                break;
            case 1:// Intake air temp
                elm327_ble_send_ascii_blocking("01 0F\r");
                ESP_LOGD(TAG, "Send 01 0F");
                break;
            case 6: // Auto oil-temp query (based on vehicle strategy) (CAN mode gets it from 0x360, skip)
                if (can_broadcast) break;
                {
                    // ---- Data-driven oil-temp query ----
                    const oil_formula_t *oil_f = NULL;
                    if (s_oil_use_override) {
                        oil_f = (s_oil_override_idx == 0) ? s_oil_formula_pri : s_oil_formula_sec;
                    }

                    if (oil_f && oil_f->type != OIL_SPECIAL) {
                        // Generic formula: auto-build the command
                        char cmd_buf[24];
                        // FCA extended addressing (Giulia etc.): the oil-temp DID sits behind 18DA10F1; temporarily switch headers to query, then restore the standard header.
                        // Otherwise, functional-addressing profiles temporarily switch to physical addressing 7E0 for UDS queries.
                        const char *uds_hdr = (s_ov && s_ov->uds_header_cmd) ? s_ov->uds_header_cmd : NULL;
                        bool need_phys = !uds_hdr && s_ov && s_ov->functional_addr && oil_f->type == OIL_UDS_22;
                        if (uds_hdr) elm327_ble_send_ascii_blocking(uds_hdr);
                        else if (need_phys) elm327_ble_send_ascii_blocking("ATSH7E0\r");
                        if (oil_formula_build_cmd(oil_f, cmd_buf, sizeof(cmd_buf))) {
                            elm327_ble_send_ascii_blocking(cmd_buf);
                            ESP_LOGI(TAG, "[Slot6] Override oil: %s", cmd_buf);
                        }
                        if (uds_hdr) elm327_ble_send_ascii_blocking(get_vehicle_fixed_header_cmd());
                        else if (need_phys) elm327_ble_send_ascii_blocking("ATSH7DF\r");
                        s_expect_mode21 = false;
                    } else if (oil_f && oil_f->type == OIL_SPECIAL && oil_f->special_id == 0) {
                        // Toyota Mode 21 01 (special multi-frame)
                        elm327_ble_send_ascii_blocking("21 01\r");
                        s_expect_mode21 = true;
                        ESP_LOGI(TAG, "[Slot6] Override oil: Toyota 21 01");
                    } else if (oil_f && oil_f->type == OIL_SPECIAL && oil_f->special_id == 1) {
                        // Porsche CAN 0x441
                        s_expect_mode21 = false;
                        porsche_read_can_441();
                        ESP_LOGI(TAG, "[Slot6] Override oil: Porsche CAN 441");
                    } else {
                        // No override: use the legacy enum logic (OBD2 Generic etc.)
                        uint8_t poll_idx = 0;
                        oil_temp_query_mode_t mode = get_next_oil_query_mode(&poll_idx);
                        s_expect_mode21 = (mode == OIL_TEMP_MODE_TOYOTA_21_01);
                        if (mode == OIL_TEMP_MODE_PID_5C)
                            elm327_ble_send_ascii_blocking("01 5C\r");
                        else if (mode == OIL_TEMP_MODE_TOYOTA_21_01)
                            elm327_ble_send_ascii_blocking("21 01\r");
                        else if (mode == OIL_TEMP_MODE_PORSCHE_CAN_441)
                            porsche_read_can_441();
                        else {
                            // Remaining legacy enums go through generic UDS building
                            char cmd_buf[24];
                            oil_formula_t legacy_f = {0};
                            legacy_f.type = OIL_UDS_22;
                            legacy_f.pid_len = 2;
                            // Map the PID from the legacy enum
                            switch (mode) {
                                case OIL_TEMP_MODE_UDS_22_10_17: legacy_f.pid[0]=0x10; legacy_f.pid[1]=0x17; break;
                                case OIL_TEMP_MODE_MAZDA_22_111F: legacy_f.pid[0]=0x11; legacy_f.pid[1]=0x1F; break;
                                case OIL_TEMP_MODE_MAZDA_22_1310: legacy_f.pid[0]=0x13; legacy_f.pid[1]=0x10; break;
                                case OIL_TEMP_MODE_MINI_22_5822: legacy_f.pid[0]=0x58; legacy_f.pid[1]=0x22; break;
                                case OIL_TEMP_MODE_BMW_22_4402:
                                case OIL_TEMP_MODE_BMW_G_22_4402: legacy_f.pid[0]=0x44; legacy_f.pid[1]=0x02; break;
                                case OIL_TEMP_MODE_BMW_22_03F3: legacy_f.pid[0]=0x03; legacy_f.pid[1]=0xF3; break;
                                case OIL_TEMP_MODE_BMW_22_D002: legacy_f.pid[0]=0xD0; legacy_f.pid[1]=0x02; break;
                                case OIL_TEMP_MODE_BMW_22_111F: legacy_f.pid[0]=0x11; legacy_f.pid[1]=0x1F; break;
                                default: legacy_f.type = OIL_STD_PID; legacy_f.pid[0]=0x5C; legacy_f.pid_len=1; break;
                            }
                            bool need_phys = (mode == OIL_TEMP_MODE_BMW_22_03F3 ||
                                              mode == OIL_TEMP_MODE_BMW_G_22_4402 ||
                                              mode == OIL_TEMP_MODE_BMW_22_D002 ||
                                              mode == OIL_TEMP_MODE_BMW_22_111F);
                            if (need_phys) elm327_ble_send_ascii_blocking("ATSH7E0\r");
                            if (oil_formula_build_cmd(&legacy_f, cmd_buf, sizeof(cmd_buf)))
                                elm327_ble_send_ascii_blocking(cmd_buf);
                            if (need_phys) elm327_ble_send_ascii_blocking("ATSH7DF\r");
                        }
                        s_oil_query_mode = poll_idx;
                    }
                }
                break;
            case 2:// Vehicle speed
                elm327_ble_send_ascii_blocking("01 0D\r");
                ESP_LOGD(TAG, "Send 01 0D");
                break;
            case 3:// Coolant temp (CAN mode gets it from 0x360, skip)
                if (!can_broadcast) {
                    elm327_ble_send_ascii_blocking("01 05\r");
                    ESP_LOGD(TAG, "Send 01 05");
                }
                break;
            case 4:// Engine load (0x04, 0~100%)
                elm327_ble_send_ascii_blocking("01 04\r");
                ESP_LOGD(TAG, "[Slot4] Send 01 04 (engine load)");
                break;
            case 5:// Throttle position TPS (0x11, 0~100%)
                elm327_ble_send_ascii_blocking("01 11\r");
                ESP_LOGD(TAG, "[Slot5] Send 01 11 (TPS)");
                break;
            case 7:// Battery voltage (0x42)
                elm327_ble_send_ascii_blocking("01 42\r");
                ESP_LOGD(TAG, "[Slot7] Send 01 42 (bat voltage)");
                break;
            case 8:// Boost pressure: intake manifold absolute pressure (0x0B, kPa), queried only for turbo profiles
                {
                    const vehicle_profile_t *vp = vehicle_profile_get_active();
                    if (vp && vp->has_boost) {
                        elm327_ble_send_ascii_blocking("01 0B\r");
                        ESP_LOGD(TAG, "[Slot8] Send 01 0B (boost/MAP)");
                    }
                }
                break;
            case 9:// Air-fuel ratio AFR (01 44, Commanded Equivalence Ratio)
                elm327_ble_send_ascii_blocking("01 44\r");
                ESP_LOGD(TAG, "[Slot9] Send 01 44 (AFR/lambda)");
                break;
            default:
                break;
        }

        // Faster RPM updates: on standard-OBD-polling profiles, RPM shouldn't refresh only once per full round (10 slots).
        // slot0 already queries RPM; here we append an extra 01 0C after every slot1~9,
        // cutting the RPM refresh interval from "a whole round" to "one slot" while slow variables (temps/voltage etc.) keep their cadence.
        // CAN broadcast profiles get RPM via ATMA passthrough; they don't need this (and shouldn't — it would fight ATMA for the bus).
        if (!can_broadcast && tick_count != 0) {
            elm327_ble_send_ascii_blocking("01 0C\r");
        }

        tick_count++;
        if(tick_count >= 10)
        {
            tick_count = 0;
        }
        } // end standard OBD poll block

        // Inter-slot idle gap: prefer the profile's poll_gap_ms (e.g. ZC/N6, MX-5 ND use 1ms); fall back to the global default 30ms.
        // Too small overwhelms cheap BLE adapters; profiles with fast CAN-bus response can safely go smaller.
        // If the user sets poll_gap_ms = 0, skip vTaskDelay and move on directly.
        {
            const vehicle_profile_t *vp_gap = vehicle_profile_get_active();
            uint32_t gap = (vp_gap && vp_gap->poll_gap_ms > 0)
                           ? vp_gap->poll_gap_ms : OBD_POLL_SLOT_GAP_MS;
            if (gap > 0) vTaskDelay(pdMS_TO_TICKS(gap));
        }
    }
}

// Mode 21 multi-frame parser: extract all data bytes after "61 01".
// Skips ELM327 line-number prefixes ("N: ") and ISO-TP consecutive-frame sequence bytes (0x20~0x2F).
// Returns the number of bytes extracted; results stored in out[].
static int parse_mode21_data(const char *buf, uint32_t *out, int max_out) {
    const char *p = strstr(buf, "61 01");
    if (!p) return 0;
    p += 5; // skip "61 01"
    if (*p == ' ') p++;

    int count = 0;
    bool new_line = false;

    while (*p && count < max_out) {
        if (*p == '>') break;
        if (*p == '\r' || *p == '\n') {
            new_line = true;
            p++;
            continue;
        }
        if (new_line) {
            // Skip the "N: " prefix (one or more digits + colon + space)
            while (isdigit((unsigned char)*p)) p++;
            if (*p == ':') p++;
            while (*p == ' ') p++;
            // Skip ISO-TP consecutive-frame sequence bytes (0x20~0x2F)
            if (isxdigit((unsigned char)*p) && isxdigit((unsigned char)*(p+1))) {
                char tmp[3] = {*p, *(p+1), '\0'};
                unsigned bval = (unsigned)strtoul(tmp, NULL, 16);
                if (bval >= 0x20 && bval <= 0x2F) {
                    p += 2;
                    if (*p == ' ') p++;
                }
            }
            new_line = false;
            continue;
        }
        // Parse one hex byte pair
        if (isxdigit((unsigned char)*p) && isxdigit((unsigned char)*(p+1))) {
            char tmp[3] = {*p, *(p+1), '\0'};
            out[count++] = (uint32_t)strtoul(tmp, NULL, 16);
            p += 2;
        } else {
            p++;
        }
        if (*p == ' ') p++;
    }
    return count;
}

// Extract the oil-temp byte from Mode21 data (using ZC/N6 as the reference).
// ZC/N6: always use only d[33], never enter the adaptive search (which may mis-pick another byte and jump to 60/70°C).
static bool extract_mode21_oil_temp(const uint32_t *d, int count, int32_t *oil_c) {
    if (!d || count <= 0 || !oil_c) return false;

    int16_t coolant = obd_data_get_coolant_temp();

    ESP_LOGD(TAG, "Mode21 extract: total_count=%d, coolant=%d", count, coolant);

    // ---- ZC/N6: locate by tail offset, handling both 38- and 39-byte response lengths ----
    // With 38 bytes oil temp is at d[33]=d[38-5]; with 39 bytes at d[34]=d[39-5]. A fixed d[33] reads the wrong byte on 39-byte frames.
    // Identify by profile name (not index): both the "ZN/C6 CAN" and "ZN/C6 PID" variants take this parse path.
    const vehicle_profile_t *vp_m21 = vehicle_profile_get_active();
    if (vp_m21 && strncmp(vp_m21->name, "ZN/C6", 5) == 0) {
        #define ZC_MODE21_OIL_TAIL_OFFSET 5
        int zc_idx = count - ZC_MODE21_OIL_TAIL_OFFSET;
        if (zc_idx < 0 || zc_idx >= count) {
            ESP_LOGW(TAG, "Mode21 ZC/N6 short response count=%d, skip", count);
            s_oil_diag.mode2_fail++;
            return false;
        }
        int32_t zc_temp = (int32_t)d[zc_idx] - 40;
        if (zc_temp < -10 || zc_temp > 150) {
            ESP_LOGW(TAG, "Mode21 ZC/N6 d[%d] out of range: raw=%u, skip", zc_idx, (unsigned)d[zc_idx]);
            s_oil_diag.mode2_fail++;
            return false;
        }
        // Consistency check: oil temp physically cannot jump more than 8°C between two polls (~270ms).
        // But if >3s since the last accepted value (failed frames caused a large gap), accept directly (the real temp may have changed).
        int64_t now_us = esp_timer_get_time();
        bool time_gap = (s_last_mode21_oil_us == 0) || ((now_us - s_last_mode21_oil_us) > 3000000);
        bool consistent = (s_last_mode21_oil <= -50) || time_gap ||
                          (abs((int)zc_temp - (int)s_last_mode21_oil) <= 8);
        if (consistent) {
            s_last_mode21_oil = (int16_t)zc_temp;
            s_last_mode21_oil_us = now_us;
            s_mode21_hold_cnt = 0;
            s_oil_diag.mode2_ok++;
            *oil_c = zc_temp;
            ESP_LOGI(TAG, "Mode21 ZC/N6 bytes=%d d[%d]=0x%02X -> %dC", count, zc_idx, (unsigned)d[zc_idx], (int)zc_temp);
            return true;
        }
        // Consistency check failed: hold the last value to avoid displaying single-frame noise
        if (s_mode21_hold_cnt < 30) {
            s_mode21_hold_cnt++;
            s_oil_diag.mode2_ok++;
            *oil_c = s_last_mode21_oil;
            ESP_LOGW(TAG, "Mode21 ZC/N6 spike HELD(%d/30): prev=%d new=%d",
                     s_mode21_hold_cnt, (int)s_last_mode21_oil, (int)zc_temp);
            return true;
        }
        // Hold timeout (~8s of continuous inconsistency): treat as a real temperature change; accept and reset the baseline
        ESP_LOGW(TAG, "Mode21 ZC/N6 hold timeout: accept %d (was %d)", (int)zc_temp, (int)s_last_mode21_oil);
        s_last_mode21_oil = (int16_t)zc_temp;
        s_last_mode21_oil_us = esp_timer_get_time();
        s_mode21_hold_cnt = 0;
        s_oil_diag.mode2_ok++;
        *oil_c = zc_temp;
        return true;
    }

    // ---- Strategy 1: use the index found last time (fast path) ----
    if (s_mode21_oil_idx >= 0 && s_mode21_oil_idx < count) {
        int32_t c = (int32_t)d[s_mode21_oil_idx] - 40;
        bool in_range = (c >= -10 && c <= 150);
        // Oil temp physically cannot jump more than 8°C between two polls (~270ms); use this to filter noise bytes
        bool consistent = (s_last_mode21_oil <= -50) || (abs((int)c - (int)s_last_mode21_oil) <= 8);
        if (in_range && consistent) {
            s_last_mode21_oil = (int16_t)c;
            s_mode21_hold_cnt = 0;
            *oil_c = c;
            ESP_LOGD(TAG, "Mode21: Using cached idx=%d -> %dC", s_mode21_oil_idx, (int)c);
            s_oil_diag.mode2_ok++;
            return true;
        }
        if (in_range && !consistent) {
            // Briefly hold the last value to avoid displaying single-frame noise
            if (s_mode21_hold_cnt < 30) {
                s_mode21_hold_cnt++;
                s_oil_diag.mode2_ok++;
                *oil_c = s_last_mode21_oil;
                ESP_LOGW(TAG, "Mode21: Fast path spike HELD(%d/30) idx=%d prev=%d new=%d",
                         s_mode21_hold_cnt, s_mode21_oil_idx, (int)s_last_mode21_oil, (int)c);
                return true;
            }
            // Hold timeout: the cached index's value is in range but inconsistent for ~8s straight — treat as a real temperature change.
            // Accept the new value and reset the baseline; do NOT fall into the adaptive search (which may mis-pick another byte and cause jumps).
            ESP_LOGW(TAG, "Mode21: Fast path hold timeout: accept new val=%d at idx=%d (was %d), reset baseline",
                     (int)c, s_mode21_oil_idx, (int)s_last_mode21_oil);
            s_last_mode21_oil = (int16_t)c;
            s_mode21_hold_cnt = 0;
            s_oil_diag.mode2_ok++;
            *oil_c = c;
            return true;
        }
        // Only when the cached index's value is out of range (stale index) do we fall into the adaptive search to rediscover
    }

    // ---- Strategy 2: intelligent search ----
    // Two-stage search: first strictly check the difference from coolant temp (±25°C), then widen the range
    int best_idx = -1;
    int32_t best_temp = 0;
    int best_distance = -1;
    int strict_count = 0;

    for (int idx = 0; idx < count; idx++) {
        int32_t c = (int32_t)d[idx] - 40;

        // Basic range check: -10 to 150°C (broad)
        if (c < -10 || c > 150) continue;

        // Strict match: oil temp is usually 5~20°C above coolant temp; a byte exactly equal to coolant temp is likely the coolant echo, lower its priority
        if (coolant > -40) {
            int diff = (int)c - (int)coolant;

            // Stage 1: strict ±25°C
            if (diff >= -25 && diff <= 25) {
                // Oil slightly above coolant scores highest; exactly equal to coolant (diff≈0) scores medium (may be the coolant echo byte)
                int priority = (diff > 0 && diff <= 20) ? 1000 :   // ideal: oil > coolant
                               (diff >= -5 && diff <= 0) ? 700  :   // cold engine or equal temp: acceptable
                               (diff > 20 && diff <= 25) ? 400  : 100; // extremely hot or colder than coolant
                int score = priority - abs(diff);  // smaller difference = higher score
                
                if (score > best_distance) {
                    best_distance = score;
                    best_idx = idx;
                    best_temp = c;
                    strict_count++;
                    ESP_LOGD(TAG, "  Strict match: idx=%d temp=%dC diff=%d score=%d", idx, (int)c, diff, score);
                }
            }
        } else {
            // When coolant temp is invalid, take any valid temperature (but log a warning)
            if (best_idx < 0) {
                best_idx = idx;
                best_temp = c;
                ESP_LOGW(TAG, "  Fallback (no coolant): idx=%d temp=%dC", idx, (int)c);
            }
        }
    }

    if (best_idx >= 0) {
        ESP_LOGD(TAG, "Mode21 selected: idx=%d temp=%dC (strict_matches=%d)", best_idx, (int)best_temp, strict_count);
        s_mode21_oil_idx = best_idx;
        s_last_mode21_oil = (int16_t)best_temp;
        s_mode21_hold_cnt = 0;
        s_oil_diag.mode2_ok++;
        *oil_c = best_temp;
        return true;
    }
    
    ESP_LOGW(TAG, "Mode21: No valid candidate found (coolant=%d)", coolant);
    s_oil_diag.mode2_fail++;
    return false;
}

static void start_scan(void) {
    esp_ble_gap_start_scanning(10); // 10s
}

static bool match_device_target(const esp_ble_gap_cb_param_t *pr, const char *target_name,
                                char *found_name, size_t found_name_len) {
    if (!pr) return false;
    ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len, pr->scan_rst.scan_rsp_len,
                     found_name, found_name_len);
    (void)target_name;

    // Only exact MAC matches: never auto-connect when no MAC is bound (no fuzzy name matching,
    // to prevent connecting to a nearby same-named adapter that yields no data). The user must manually select and bind a MAC on the BLE SCAN page.
    if (!s_target_bda_valid) return false;
    return memcmp(pr->scan_rst.bda, s_target_bda, sizeof(esp_bd_addr_t)) == 0;
}

static void request_discovery(void) {
    // NULL = discover all services, to stay compatible with ELM327 adapters using different UUIDs
    esp_ble_gattc_search_service(s_gattc_if, s_conn_id, NULL);
}

static void enable_notify_if_ready(void) {
    if (s_cccd_handle) {
        uint8_t notify_en[2] = {0x01, 0x00};
        esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, s_cccd_handle,
                                       sizeof(notify_en), notify_en,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

void elm327_ble_init_and_start(const char *target_name, const elm327_ble_callbacks_t *cbs) {
    if (cbs) s_cbs = *cbs;
    if (target_name && target_name[0]) {
        strncpy(s_target_name, target_name, sizeof(s_target_name)-1);
        s_target_name[sizeof(s_target_name)-1] = '\0';
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    }
    if (!esp_bluedroid_get_status()) {
        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    } else if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_ERROR_CHECK(esp_bluedroid_enable());
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(0));
    s_ble_inited = true;
}

bool elm327_ble_send_command(const uint8_t *data, size_t len) {
    if (!s_connected || s_char_write_handle == 0) {
        s_elm_ready = true; // restore the flag when unable to send, to prevent a permanent timeout
        return false;
    }
    if (len == 0 || data == NULL) { s_elm_ready = true; return false; }
    esp_err_t err = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_char_write_handle,
                                             len, (uint8_t *)data,
                                             s_write_type, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) s_elm_ready = true; // also restore on send failure
    return err == ESP_OK;
}

// Block until the previous response ends ('>' received) before sending.
// Uses FreeRTOS task notifications instead of 10ms polling: xTaskNotify wakes immediately on '>', zero wait overhead.
bool elm327_ble_send_ascii_blocking(const char *ascii_cmd)
{
    if (!s_elm_ready) {
        uint32_t waited_ms = 0;
        while (!s_elm_ready && waited_ms < 3000) {
            // Wait at most 10ms (as a fallback); xTaskNotify wakes early when '>' arrives
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            waited_ms += 10;
            // Feed the watchdog: this wait loop itself can run close to 3s (adapter unresponsive / protocol detection timing out repeatedly).
            // The TWDT timeout is only 5s; without feeding here, a few consecutive protocol-detect timeouts would
            // trip the obd_poll task's watchdog and reboot (reproduced in testing).
            esp_task_wdt_reset();
        }
        if (!s_elm_ready) {
            ESP_LOGW(TAG, "Timeout (>3s) waiting previous response, forcing send: %s", ascii_cmd);
            s_elm_ready = true;
        }
    }
    s_elm_ready = false;
    uint8_t buf[32];
    size_t n = elm327_ble_ascii_cmd_to_bytes(ascii_cmd, buf, sizeof(buf));
    if (n) return elm327_ble_send_command(buf, n);
    else {
        s_elm_ready = true;
        return false;
    }
}

// Copy an ASCII command (e.g. "01 0C\r") into the output buffer, stripping whitespace while keeping the ASCII format the ELM327 expects
size_t elm327_ble_ascii_cmd_to_bytes(const char *ascii, uint8_t *out_buf, size_t out_buf_len) {
    size_t out = 0;
    const char *p = ascii;
    while (*p && out < out_buf_len) {
        if (*p == ' ' || *p == '\t') {
            p++;                // skip whitespace
            continue;
        }
        out_buf[out++] = (uint8_t)(*p++); // copy the ASCII byte directly
    }
    return out;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    racechrono_ble_diy_handle_gap_event(event, param);

    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        start_scan();
        break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *pr = param;
        if (pr->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            char dev_name[32] = {0};
            ble_adv_extract_name(pr->scan_rst.ble_adv, pr->scan_rst.adv_data_len,
                             pr->scan_rst.scan_rsp_len, dev_name, sizeof(dev_name));

            if (s_scan_only_mode) {
                // Scan mode: collect the device list
                if (dev_name[0] != '\0' && s_scan_count < BLE_SCAN_MAX_DEVICES) {
                    // Check if it already exists
                    bool exists = false;
                    for (int i = 0; i < s_scan_count; i++) {
                        if (strcmp(s_scan_list[i].name, dev_name) == 0) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        strncpy(s_scan_list[s_scan_count].name, dev_name, 31);
                        memcpy(s_scan_list[s_scan_count].addr, pr->scan_rst.bda, 6);
                        s_scan_list[s_scan_count].rssi = pr->scan_rst.rssi;
                        s_scan_count++;
                        ESP_LOGD(TAG, "Scan found [%d]: %s (RSSI %d)", s_scan_count, dev_name, pr->scan_rst.rssi);
                        if (s_scan_cb) s_scan_cb(&s_scan_list[s_scan_count - 1], s_scan_count);
                    }
                }
            } else {
                // Normal mode: connect after matching
                bool matched = match_device_target(pr, s_target_name, dev_name, sizeof(dev_name));
                ESP_LOGD(TAG, "Scan: name=%s rssi=%d target=%s match=%d",
                         dev_name[0] ? dev_name : "<no-name>", pr->scan_rst.rssi,
                         s_target_name, matched);
                if (matched) {
                    ESP_LOGD(TAG, "Found target %s (dev=%s), connecting...",
                             s_target_name, dev_name[0] ? dev_name : "<no-name>");
                    esp_ble_gap_stop_scanning();
                    esp_ble_gattc_open(s_gattc_if, pr->scan_rst.bda, pr->scan_rst.ble_addr_type, true);
                }
            }
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
    default:
        break;
    }
}

// Locate a CAN ID's frame-header token in the multi-line accumulation buffer; only accept matches "at the line start".
// Must not strstr the whole buffer directly: if some frame's data-byte text happens to equal another monitored ID
// (e.g. oil-temp byte =0x40 while the profile also monitors 0x040), it false-matches inside the data segment and mislabels the frame.
static const char *find_can_id_token(const char *buf, uint16_t id) {
    char upper[8], lower[8];
    snprintf(upper, sizeof(upper), "%X ", id);
    snprintf(lower, sizeof(lower), "%x ", id);
    for (int pass = 0; pass < 2; pass++) {
        const char *needle = (pass == 0) ? upper : lower;
        const char *scan = buf;
        while ((scan = strstr(scan, needle)) != NULL) {
            if (scan == buf || scan[-1] == '\r' || scan[-1] == '\n') return scan;
            scan += 1; // coincidental match inside data bytes; skip it and keep looking for a real line start
        }
    }
    return NULL;
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT: {
        s_gattc_if = gattc_if;
        esp_ble_scan_params_t scan_params = {
            .scan_type              = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval          = 0x60,
            .scan_window            = 0x30,
            .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
        };
        esp_ble_gap_set_scan_params(&scan_params);
        break;
    }
    case ESP_GATTC_CONNECT_EVT: {
        s_connected = true;
        s_conn_id = param->connect.conn_id;
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        if (s_cbs.on_connected) s_cbs.on_connected();
        request_discovery();
        break;
    }
    case ESP_GATTC_OPEN_EVT: {
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Open failed status=%d", param->open.status);
            start_scan();
        }
        break;
    }
    case ESP_GATTC_SEARCH_RES_EVT: {
        const esp_gatt_id_t *srvc_id = &param->search_res.srvc_id;
        uint16_t sh = param->search_res.start_handle;
        uint16_t eh = param->search_res.end_handle;
        if (srvc_id->uuid.len == ESP_UUID_LEN_16) {
            ESP_LOGD(TAG, "Service found: UUID=0x%04X handle=%04X~%04X",
                     srvc_id->uuid.uuid.uuid16, sh, eh);
            if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE) {
                s_have_service = true;
                s_service_start = sh;
                s_service_end = eh;
                ESP_LOGD(TAG, "Target service FFF0 matched");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_18F0) {
                s_have_18f0 = true;
                s_18f0_start = sh;
                s_18f0_end = eh;
                ESP_LOGD(TAG, "Target service 18F0 matched (IOS-Vlink OBD)");
            } else if (srvc_id->uuid.uuid.uuid16 == UUID16_OBD_SERVICE_FF12) {
                s_have_ff12 = true;
                s_ff12_start = sh;
                s_ff12_end = eh;
                ESP_LOGD(TAG, "Target service FF12 matched");
            }
        } else {
            ESP_LOGD(TAG, "Service found: UUID(long) handle=%04X~%04X", sh, eh);
        }
        // Record the max handle range, used for the full-range fallback search
        if (eh > s_all_attr_end || s_all_attr_end == 0xFFFF) s_all_attr_end = eh;
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        ESP_LOGD(TAG, "Service discovery complete. have_FFF0=%d have_18F0=%d have_FF12=%d",
                 s_have_service, s_have_18f0, s_have_ff12);

        // Preference order: 0xFFF0 > 0x18F0 (IOS-Vlink) > 0xFF12 > full-range fallback
        if (!s_have_service) {
            if (s_have_18f0) {
                s_service_start = s_18f0_start;
                s_service_end   = s_18f0_end;
                ESP_LOGD(TAG, "Using 18F0 service range 0x%04X~0x%04X", s_service_start, s_service_end);
            } else if (s_have_ff12) {
                s_service_start = s_ff12_start;
                s_service_end   = s_ff12_end;
                ESP_LOGD(TAG, "Using FF12 service range 0x%04X~0x%04X", s_service_start, s_service_end);
            } else {
                s_service_start = 0x0001;
                s_service_end = (s_all_attr_end > 0x0001) ? s_all_attr_end : 0xFFFF;
                ESP_LOGW(TAG, "FFF0/18F0/FF12 not found, using full range 0x0001~0x%04X", s_service_end);
            }
        }

        // Enumerate all characteristics and select by property (WRITE/NOTIFY)
        uint16_t char_count = 0;
        esp_err_t ret = esp_ble_gattc_get_attr_count(gattc_if, s_conn_id,
            ESP_GATT_DB_CHARACTERISTIC, s_service_start, s_service_end, 0, &char_count);
        ESP_LOGD(TAG, "get_attr_count ret=%d, char_count=%d", ret, char_count);

        if (ret != ESP_OK || char_count == 0) {
            ESP_LOGE(TAG, "No characteristics found in range! Cannot communicate.");
            break;
        }

        // Allocate the characteristic array
        uint16_t alloc_count = char_count;
        esp_gattc_char_elem_t *chars = (esp_gattc_char_elem_t *)malloc(alloc_count * sizeof(esp_gattc_char_elem_t));
        if (!chars) { ESP_LOGE(TAG, "malloc failed"); break; }

        ret = esp_ble_gattc_get_all_char(gattc_if, s_conn_id,
            s_service_start, s_service_end, chars, &alloc_count, 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "get_all_char failed: %d", ret);
            free(chars); break;
        }

        // Print all characteristics and auto-select the write/notify handles
        ESP_LOGD(TAG, "=== All characteristics (%d) ===", alloc_count);
        for (int i = 0; i < alloc_count; i++) {
            esp_gattc_char_elem_t *c = &chars[i];
            if (c->uuid.len == ESP_UUID_LEN_16) {
                ESP_LOGD(TAG, "  [%d] UUID=0x%04X handle=0x%04X prop=0x%02X",
                         i, c->uuid.uuid.uuid16, c->char_handle, c->properties);
            } else if (c->uuid.len == ESP_UUID_LEN_128) {
                ESP_LOGD(TAG, "  [%d] UUID128=%02X%02X...%02X%02X handle=0x%04X prop=0x%02X",
                         i, c->uuid.uuid.uuid128[15], c->uuid.uuid.uuid128[14],
                            c->uuid.uuid.uuid128[1],  c->uuid.uuid.uuid128[0],
                            c->char_handle, c->properties);
            }
            // Pick the first characteristic with the WRITE property as the write handle
            if (s_char_write_handle == 0 &&
                (c->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
                s_char_write_handle = c->char_handle;
                // Prefer WRITE_NR (write without response) to avoid status=3 (WRITE_NOT_PERMIT)
                s_write_type = (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE_NR)
                               ? ESP_GATT_WRITE_TYPE_NO_RSP
                               : ESP_GATT_WRITE_TYPE_RSP;
                ESP_LOGD(TAG, "  >> Selected as WRITE handle: 0x%04X (write_type=%s)",
                         s_char_write_handle,
                         s_write_type == ESP_GATT_WRITE_TYPE_NO_RSP ? "NO_RSP" : "RSP");
            }
            // Pick the first characteristic with the NOTIFY property as the notify handle
            if (s_char_notify_handle == 0 &&
                (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
                s_char_notify_handle = c->char_handle;
                ESP_LOGD(TAG, "  >> Selected as NOTIFY handle: 0x%04X", s_char_notify_handle);
            }
        }
        free(chars);

        if (s_char_write_handle == 0) {
            ESP_LOGE(TAG, "No WRITE characteristic found! Cannot send commands.");
            break;
        }
        // If there's no dedicated NOTIFY characteristic, reuse the write handle
        if (s_char_notify_handle == 0) {
            s_char_notify_handle = s_char_write_handle;
            ESP_LOGD(TAG, "No NOTIFY char found, using WRITE handle 0x%04X for notify", s_char_notify_handle);
        }

        // Register for notifications
        int sret = esp_ble_gattc_register_for_notify(gattc_if, s_peer_bda, s_char_notify_handle);
        ESP_LOGD(TAG, "register_for_notify handle=0x%04X ret=%d", s_char_notify_handle, sret);

        // Find the CCCD
        esp_gattc_descr_elem_t descr_elems[2];
        uint16_t count = 2;
        esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = UUID16_CCCD } };
        ret = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
            s_char_notify_handle, cccd_uuid, descr_elems, &count);
        if (ret == ESP_OK && count > 0) {
            s_cccd_handle = descr_elems[0].handle;
            ESP_LOGD(TAG, "Found CCCD, handle: 0x%04X", s_cccd_handle);
        } else {
            ESP_LOGW(TAG, "CCCD not found (ret=%d cnt=%d)", ret, count);
        }
        enable_notify_if_ready();
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGD(TAG, "Notifications enabled");
            s_notify_ready = true;   // subscription ready → let the poll task proceed with ELM init
        } else {
            ESP_LOGW(TAG, "Enable notify failed status=%d", param->write.status);
        }
        break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
        if (s_cbs.on_raw_notify) s_cbs.on_raw_notify(param->notify.value, param->notify.value_len);
        const uint8_t *v = param->notify.value;
        int n = param->notify.value_len;

        // ZC6 CAN continuous monitor mode: feed byte-wise, parse line-wise, bypass the accumulation buffer
        if (s_zc6_can_monitor_active) {
            zc6_can_monitor_feed(v, (size_t)n);
            break;
        }

        // ---- Accumulate multi-packet data until '>' (the ELM327 prompt) is received ----
        // Accumulation timeout guard: force-flush if '>' doesn't arrive within 10s (ATMA mode may go long without a '>' prompt)
        if (s_accum_len > 0) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - s_accum_start_us) > 10000000) {
                ESP_LOGW(TAG, "Accum timeout (>10s), flushing %d bytes", (int)s_accum_len);
                s_accum_len = 0;
                s_accum_buf[0] = '\0';
                s_elm_ready = true;
            }
        }
        if (s_accum_len == 0) {
            s_accum_start_us = esp_timer_get_time();
        }
        size_t space_left = ACCUM_BUF_SIZE - 1 - s_accum_len;
        size_t copy_n = ((size_t)n < space_left) ? (size_t)n : space_left;
        memcpy(s_accum_buf + s_accum_len, v, copy_n);
        s_accum_len += copy_n;
        s_accum_buf[s_accum_len] = '\0';

        // Keep waiting if '>' hasn't arrived
        if (memchr(s_accum_buf, '>', s_accum_len) == NULL) break;

        // Full response received, start parsing
        char *buf = s_accum_buf;
        ESP_LOGD(TAG, "FULL[%d]: %.200s", (int)s_accum_len, buf); // diagnostics: print every full response

        // Raw dump of oil-temp-related responses (to verify adapters like gt96 return data correctly)
        {
            uint32_t first_tok = 0;
            int ntk = sscanf(buf, "%x", &first_tok);
            if (s_expect_mode21 && ntk == 1 && first_tok == 0x61) {
                ESP_LOGI(TAG, "[OIL RX] Mode21 raw[%d]: %.200s", (int)s_accum_len, buf);
            } else if (!s_expect_mode21 && ntk == 1 && first_tok == 0x62) {
                ESP_LOGI(TAG, "[OIL RX] Mode22 raw[%d]: %.200s", (int)s_accum_len, buf);
            } else if (strstr(buf, "441 ")) {
                ESP_LOGI(TAG, "[OIL RX] CAN441 raw[%d]: %.200s", (int)s_accum_len, buf);
            }
        }

        // The ELM327 may prepend an echo before the data, so use strstr to search the whole buffer for response headers.
        // Note: p61 must be checked before p41, because the 2101 multi-frame response body may contain 0x41 bytes,
        // which would false-match "41 " and skip Mode21 parsing.
        char *p61 = strstr(buf, "61 01"); // Mode 21 response header (exact match "61 01")
        char *p41 = strstr(buf, "41 ");
        char *p62 = strstr(buf, "62 ");
        // Porsche CAN broadcast frame 0x441 (under ATH1 monitoring it looks like "441 D0 D1 ... D7"). Parsed only when the current
        // profile uses this mode, and must be checked before p41 (since "441 " contains the substring "41 "). byte5=oil temp (x-60°C), byte7=oil pressure (x/25.4 bar).
        char *p441 = (s_oil_mode_priority[0] == OIL_TEMP_MODE_PORSCHE_CAN_441) ? strstr(buf, "441 ") : NULL;
        // ---- CAN broadcast frame parsing: data-driven ----
        const vehicle_profile_t *vp_can = vehicle_profile_get_active();
        const vehicle_override_t *ov_can = vehicle_profile_get_override();
        bool has_can_rules = ov_can && ov_can->can_rules && ov_can->can_rule_count > 0;
        // Compat with the legacy can_broadcast_mode flag
        bool ft86_can_mode = (vp_can && vp_can->can_broadcast_mode) || has_can_rules;

        // Collect all unique CAN IDs from the override and search for them in buf
        if (ft86_can_mode) {
            uint16_t seen_ids[8] = {0};
            uint8_t seen_count = 0;
            // Decide which rule table to scan: override first, otherwise the legacy hardcoded one
            const can_rule_t *rules = has_can_rules ? ov_can->can_rules : NULL;
            uint8_t rule_count = has_can_rules ? ov_can->can_rule_count : 0;

            // If there are no override rules but can_broadcast_mode=true, use the legacy inline parsing (compat)
            if (!has_can_rules) {
                // Legacy ZC/N6 hardcoded parsing (kept for compatibility)
                char *p140 = strstr(buf, "140 ");
                if (p140) {
                    unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
                    int vals = sscanf(p140, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
                    if (vals >= 9 && id == 0x140) {
                        uint16_t can_rpm = (uint16_t)(b2 | ((b3 & 0x3F) << 8));
                        s_zc6_can_rpm_seen = true;
                        if (s_cbs.on_parsed_rpm) s_cbs.on_parsed_rpm(can_rpm);
                        uint8_t tps_pct = (uint8_t)((uint32_t)b6 * 100 / 255);
                        if (s_cbs.on_parsed_throttle_position) s_cbs.on_parsed_throttle_position(tps_pct);
                    }
                }
                char *p360 = strstr(buf, "360 ");
                if (p360) {
                    unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
                    int vals = sscanf(p360, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
                    if (vals >= 9 && id == 0x360) {
                        int32_t oil_c = (int32_t)b2 - 40;
                        int32_t clt_c = (int32_t)b3 - 40;
                        if (oil_c >= -40 && oil_c <= 215 && s_cbs.on_parsed_oil_temp)
                            s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
                        if (clt_c >= -40 && clt_c <= 215 && s_cbs.on_parsed_coolant_temp)
                            s_cbs.on_parsed_coolant_temp((uint32_t)clt_c);
                    }
                }
                char *p0d1 = strstr(buf, "0D1 ");
                if (p0d1) {
                    unsigned id=0,b0,b1,b2,b3;
                    int vals = sscanf(p0d1, "%x %x %x %x %x", &id,&b0,&b1,&b2,&b3);
                    if (vals >= 4 && id == 0x0D1) {
                        uint8_t speed_kmh = (uint8_t)(((b0 | (b1 << 8)) * 157 + 5000) / 10000);
                        if (s_cbs.on_parsed_speed_kmh) s_cbs.on_parsed_speed_kmh(speed_kmh);
                    }
                }
                mark_obd_data_valid();
                goto can_parse_done;
            }

            // ---- Generic CAN rule parsing ----
            // Collect unique CAN IDs
            for (uint8_t i = 0; i < rule_count; i++) {
                bool found = false;
                for (uint8_t j = 0; j < seen_count; j++) {
                    if (seen_ids[j] == rules[i].can_id) { found = true; break; }
                }
                if (!found && seen_count < 8) seen_ids[seen_count++] = rules[i].can_id;
            }
            // For each unique CAN ID, find and parse it in buf (line-start matches only, to avoid coincidental hits inside data bytes)
            for (uint8_t si = 0; si < seen_count; si++) {
                const char *p = find_can_id_token(buf, seen_ids[si]);
                if (!p) continue;
                // Parse the hex bytes
                uint8_t data[8] = {0};
                uint32_t parsed_id = 0;
                int vals = sscanf(p, "%x %hhx %hhx %hhx %hhx %hhx %hhx %hhx %hhx",
                                  &parsed_id, &data[0],&data[1],&data[2],&data[3],
                                  &data[4],&data[5],&data[6],&data[7]);
                if (vals < 2 || parsed_id != seen_ids[si]) continue;

                // Apply the rules
                float channels[CH_COUNT];
                for (int c = 0; c < CH_COUNT; c++) channels[c] = -32768.0f;
                can_apply_rules(rules, rule_count, seen_ids[si], data, channels);

                // Write into obd_data_cache
                if (channels[CH_RPM] >= 0 && s_cbs.on_parsed_rpm)
                    s_cbs.on_parsed_rpm((uint16_t)channels[CH_RPM]);
                if (channels[CH_SPEED] >= 0 && s_cbs.on_parsed_speed_kmh)
                    s_cbs.on_parsed_speed_kmh((uint8_t)channels[CH_SPEED]);
                if (channels[CH_OIL_TEMP] > -40 && channels[CH_OIL_TEMP] <= 215 && s_cbs.on_parsed_oil_temp)
                    s_cbs.on_parsed_oil_temp((uint32_t)(int16_t)channels[CH_OIL_TEMP]);
                if (channels[CH_COOLANT] > -40 && channels[CH_COOLANT] <= 215 && s_cbs.on_parsed_coolant_temp)
                    s_cbs.on_parsed_coolant_temp((uint32_t)(int16_t)channels[CH_COOLANT]);
                if (channels[CH_TPS] >= 0 && s_cbs.on_parsed_throttle_position)
                    s_cbs.on_parsed_throttle_position((uint32_t)channels[CH_TPS]);
                if (channels[CH_GEAR] > 0 && channels[CH_GEAR] < 127 && s_cbs.on_parsed_gear)
                    s_cbs.on_parsed_gear((int8_t)channels[CH_GEAR]);
                if (channels[CH_LOAD] >= 0 && s_cbs.on_parsed_load_pct)
                    s_cbs.on_parsed_load_pct((int16_t)channels[CH_LOAD]);

                mark_obd_data_valid();
                ESP_LOGD(TAG, "[CAN 0x%03X] parsed %d vals", seen_ids[si], vals);
            }
            can_parse_done: ;
        }
        // Any valid data frame header received → refresh the "valid data" timestamp and set the flag
        if (p41 || p62 || p61 || p441) mark_obd_data_valid();

        if (p441 != NULL) {
            unsigned id=0,b0,b1,b2,b3,b4,b5,b6,b7;
            int vals = sscanf(p441, "%x %x %x %x %x %x %x %x %x", &id,&b0,&b1,&b2,&b3,&b4,&b5,&b6,&b7);
            if (vals >= 9 && id == 0x441) {
                // byte5: oil temp, formula taken from the current vehicle profile (°C = x*num/den+off); coefficients differ between generations
                const oil_temp_strategy_t *st441 = vehicle_profile_get_oil_temp_strategy();
                int32_t num = st441 ? st441->can_num : 1;
                int32_t den = st441 ? st441->can_den : 1;
                int32_t off = st441 ? st441->can_off : -60;
                if (den == 0) { num = 1; den = 1; off = -60; }  // unconfigured → default to 997.2 (x-60)
                int32_t oil_c = (int32_t)b5 * num / den + off;
                // Oil pressure: byte/25.4 bar == byte*50/127 (0.1bar). References mostly say byte7, but some scanners show byte6.
                // Use byte7 for now; log both b6/b7 so we can compare against real oil pressure on the car (idle ~1-2bar / high rev ~4-5bar) to confirm which byte it is.
                int16_t oilp_x10 = (int16_t)((b7 * 50) / 127);  // byte7: oil pressure, 0x7F(127)=5.0bar
                s_porsche_441_seen = true;
                if (oil_c >= -40 && oil_c <= 215 && s_cbs.on_parsed_oil_temp) {
                    record_oil_temp_success(OIL_TEMP_MODE_PORSCHE_CAN_441);
                    s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
                } else {
                    record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
                }
                obd_data_set_oil_pressure_x10(oilp_x10);        // same-frame oil pressure → OILP display
                // Diagnostics: full frame + candidate bytes, to verify whether oil pressure is really in byte6 or byte7
                ESP_LOGD(TAG, "[CAN 441] RAW b0..b7=%02X %02X %02X %02X %02X %02X %02X %02X",
                         b0, b1, b2, b3, b4, b5, b6, b7);
                ESP_LOGI(TAG, "[CAN 441] bytes=8 b5=0x%02X formula=%d*%d/%d%+d -> %dC",
                         b5, b5, (int)num, (int)den, (int)off, (int)oil_c);
            } else {
                ESP_LOGD(TAG, "[CAN 441] parse fail vals=%d", vals);
                record_oil_temp_failure(OIL_TEMP_MODE_PORSCHE_CAN_441);
            }
        } else if (p61 != NULL && s_expect_mode21) {
            // Mode 21 multi-frame response (Toyota 2101)
            // s_expect_mode21 guard: parse only when we know the 21 01 command was sent,
            // preventing data bytes of other PID responses that happen to contain "61 01" from falsely triggering it (e.g. 41 0C 61 01 at ~6208rpm)
            s_expect_mode21 = false;
            uint32_t d[64] = {0};
            int count = parse_mode21_data(buf, d, 64);
            // Full dump: print in two segments to avoid ESP_LOGI truncation (~11 chars per byte; 30 bytes exceed the 256-char limit)
            { char _hx[256]; int _o, _h = count / 2;
              _o = 0; for(int _i=0;_i<_h;_i++) _o+=snprintf(_hx+_o,sizeof(_hx)-_o,"[%d]%02X(%d) ",_i,(unsigned)d[_i],(int)d[_i]-40);
              ESP_LOGD(TAG,"[21 01] bytes=%d [0-%d]: %s", count, _h-1, _hx);
              _o = 0; for(int _i=_h;_i<count;_i++) _o+=snprintf(_hx+_o,sizeof(_hx)-_o,"[%d]%02X(%d) ",_i,(unsigned)d[_i],(int)d[_i]-40);
              ESP_LOGD(TAG,"[21 01] [%d-%d]: %s", _h, count-1, _hx); }
            int32_t oil_c = 0;
            if (extract_mode21_oil_temp(d, count, &oil_c)) {
                ESP_LOGI(TAG, "Mode21 oil temp=%dC (idx=%d, bytes=%d)", 
                         (int)oil_c, s_mode21_oil_idx, count);
                record_oil_temp_success(OIL_TEMP_MODE_TOYOTA_21_01);
                // Route through the callback so smoothing + offset are applied uniformly
                if (s_cbs.on_parsed_oil_temp) s_cbs.on_parsed_oil_temp((uint32_t)oil_c);
            } else {
                ESP_LOGW(TAG, "21 01 parse failed: count=%d", count);
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
            }
        } else if (p41 != NULL && !s_expect_mode21) {
            // Mode 01 response: "41 PP DD ..."
            uint32_t d[6] = {0};
            uint32_t mode = 0, pid = 0;
            int values = sscanf(p41, "%x %x %x %x %x %x %x %x",
                &mode, &pid, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]);
            // Raw response dump for oil-temp PID 0x5C
            if (pid == 0x5C) {
                ESP_LOGI(TAG, "[OIL RX] PID0x5C raw[%d]: %.200s", (int)s_accum_len, buf);
            }
            ESP_LOGD(TAG, "OBD mode01 mode=%02X pid=%02X d=%02X %02X %02X val=%d",
                     mode, pid, d[0], d[1], d[2], values);
            if (values >= 3 && mode == 0x41) {
                int dc = values - 2;

                // During protocol detection, only handle RPM (0x0C)
                if (s_protocol_detect_idx >= 0 && pid != 0x0C) {
                    break;  // skip non-target PIDs
                }

                switch (pid) {
                    case 0x04: // Engine load (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_load_pct && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_load_pct((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x05: // Coolant temp
                        if (dc >= 1 && s_cbs.on_parsed_coolant_temp && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_coolant_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x0C: // RPM
                        if (dc >= 2) {
                            uint16_t rpm_val = (uint16_t)(((d[0] << 8) | d[1]) / 4);

                            if (s_protocol_detect_idx >= 0) {
                                // Protocol detection mode
                                s_protocol_detect_rpm = (int32_t)rpm_val;
                                s_protocol_detect_got_response = true;
                                ESP_LOGD(TAG, "[PROTOCOL_DETECT] Protocol %d: RPM=%u OK", s_protocol_detect_idx, rpm_val);
                            } else {
                                // Normal mode
                                if (s_cbs.on_parsed_rpm)
                                    s_cbs.on_parsed_rpm(rpm_val);
                            }
                        }
                        break;
                    case 0x0D: // Vehicle speed
                        if (dc >= 1 && s_cbs.on_parsed_speed_kmh && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_speed_kmh((uint8_t)d[0]);
                        break;
                    case 0x0F: // Intake air temp
                        if (dc >= 1 && s_cbs.on_parsed_intake_temp && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_intake_temp((uint32_t)((int32_t)d[0] - 40));
                        break;
                    case 0x11: // Throttle position TPS (0~100%)
                        if (dc >= 1 && s_cbs.on_parsed_throttle_position && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_throttle_position((uint32_t)d[0] * 100 / 255);
                        break;
                    case 0x0B: // Intake manifold absolute pressure MAP (kPa) → boost gauge pressure
                        if (dc >= 1 && s_cbs.on_parsed_manifold_pressure && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_manifold_pressure((uint32_t)d[0]);
                        break;
                    case 0x5C: // Oil temp PID (standard, used for ZD8)
                        if (dc >= 1 && s_cbs.on_parsed_oil_temp && s_protocol_detect_idx < 0) {
                            int32_t oil_temp = (int32_t)d[0] - 40;
                            // Validate range: -40 to 215°C
                            if (oil_temp >= -40 && oil_temp <= 215) {
                                ESP_LOGI(TAG, "[PID 0x5C] bytes=%d raw=0x%02X -> %dC", dc, (unsigned)d[0], (int)oil_temp);
                                record_oil_temp_success(OIL_TEMP_MODE_PID_5C);
                                s_cbs.on_parsed_oil_temp((uint32_t)oil_temp);
                            } else {
                                ESP_LOGD(TAG, "[PID 0x5C] Oil temp out of range: %d (raw=%02X)", (int)oil_temp, d[0]);
                                record_oil_temp_failure(OIL_TEMP_MODE_PID_5C);
                            }
                        }
                        break;
                    case 0x42: // Battery voltage (mV)
                        if (dc >= 2 && s_cbs.on_parsed_control_module_voltage && s_protocol_detect_idx < 0)
                            s_cbs.on_parsed_control_module_voltage((d[0] << 8) | d[1]);
                        break;
                    case 0x44: // Air-fuel ratio AFR - Commanded Equivalence Ratio (λ)
                        // λ = (A*256+B)/32768, range 0~<2
                        // AFR = λ × 14.7, stored ×100: 1470 = 14.70:1
                        if (dc >= 2 && s_cbs.on_parsed_afr && s_protocol_detect_idx < 0) {
                            uint32_t raw = (d[0] << 8) | d[1];
                            // λ = raw / 32768, AFR×100 = λ × 1470
                            uint32_t afr_x100 = (raw * 1470UL) / 32768UL;
                            if (afr_x100 >= 800 && afr_x100 <= 2200) {
                                s_cbs.on_parsed_afr(afr_x100);
                            }
                        }
                        break;
                    default:
                        ESP_LOGD(TAG, "Unhandled PID 0x%02X", pid);
                        break;
                }
            }
        } else if (p62 != NULL) {
            // Mode 22 response: "62 HH LL D0 D1 ..."  (d0=A, d1=B)
            // If Mode21 was expected but Mode22 arrived, clear the expect flag and record a failure
            if (s_expect_mode21) {
                ESP_LOGW(TAG, "21 01 expected but got Mode22 response");
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
                s_expect_mode21 = false;
            }
            uint32_t mode22 = 0, ph = 0, pl = 0, d0 = 0, d1 = 0;
            int values = sscanf(p62, "%x %x %x %x %x", &mode22, &ph, &pl, &d0, &d1);
            if (values >= 4 && mode22 == 0x62 && s_cbs.on_parsed_oil_temp) {
                uint32_t pid16 = (ph << 8) | pl;

                // ---- Data-driven override parsing ----
                if (s_oil_use_override) {
                    const oil_formula_t *oil_f = (s_oil_override_idx == 0) ? s_oil_formula_pri : s_oil_formula_sec;
                    if (oil_f && oil_f->type == OIL_UDS_22) {
                        uint32_t expect_pid = (oil_f->pid[0] << 8) | oil_f->pid[1];
                        if (pid16 == expect_pid) {
                            uint32_t resp_data[4] = {d0, d1, 0, 0};
                            int16_t temp = oil_formula_parse_resp(oil_f, resp_data, (uint8_t)(values - 3));
                            if (temp != -32768) {
                                ESP_LOGI(TAG, "[Override 22 %02X%02X] -> %dC", oil_f->pid[0], oil_f->pid[1], temp);
                                s_oil_override_fail = 0;
                                record_oil_temp_success(s_oil_mode_priority[0]);
                                s_cbs.on_parsed_oil_temp((uint32_t)temp);
                            } else {
                                s_oil_override_fail++;
                                record_oil_temp_failure(s_oil_mode_priority[0]);
                                if (s_oil_override_fail >= OIL_OVERRIDE_FAIL_MAX && s_oil_formula_sec) {
                                    s_oil_override_idx = 1;
                                    s_oil_override_fail = 0;
                                    ESP_LOGW(TAG, "[Override] primary failed %d times, switch to secondary", OIL_OVERRIDE_FAIL_MAX);
                                }
                            }
                            goto oil_temp_done;
                        }
                    }
                }

                // ---- Legacy PID switch/case (when there's no override or the PID doesn't match) ----
                if (pid16 == 0x1310) {
                    // Mazda Skyactiv oil temp PID 1310: (A*256+B)/100 - 40 (°C), requires 2 data bytes
                    if (values >= 5) {
                        int32_t mazda_oil = (int32_t)(((d0 * 256) + d1) / 100) - 40;
                        if (mazda_oil >= -40 && mazda_oil <= 215) {
                            ESP_LOGI(TAG, "[22 13 10] bytes=%d raw=(%02X,%02X) -> %dC", values-2, (unsigned)d0, (unsigned)d1, (int)mazda_oil);
                            record_oil_temp_success(OIL_TEMP_MODE_MAZDA_22_1310);
                            s_cbs.on_parsed_oil_temp((uint32_t)mazda_oil);
                        } else {
                            ESP_LOGD(TAG, "[22 13 10] Oil temp out of range: %d (A=%02X B=%02X)", (int)mazda_oil, (unsigned)d0, (unsigned)d1);
                            record_oil_temp_failure(OIL_TEMP_MODE_MAZDA_22_1310);
                        }
                    } else {
                        record_oil_temp_failure(OIL_TEMP_MODE_MAZDA_22_1310);
                    }
                } else if (pid16 == 0x111F) {
                    // PID 111F: °C = A - 50 (both Mazda Skyactiv and BMW use this formula)
                    // Decide which mode's stats to record based on the current strategy
                    bool is_bmw_111f = (s_oil_mode_priority[0] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[1] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[2] == OIL_TEMP_MODE_BMW_22_111F ||
                                        s_oil_mode_priority[3] == OIL_TEMP_MODE_BMW_22_111F);
                    oil_temp_query_mode_t mode_111f = is_bmw_111f ? OIL_TEMP_MODE_BMW_22_111F
                                                                  : OIL_TEMP_MODE_MAZDA_22_111F;
                    int32_t oil_111f = (int32_t)d0 - 50;
                    if (oil_111f >= -40 && oil_111f <= 215) {
                        ESP_LOGI(TAG, "[22 11 1F] bytes=%d raw=0x%02X -> %dC (%s)", values-2, (unsigned)d0, (int)oil_111f,
                                 is_bmw_111f ? "BMW" : "Mazda");
                        record_oil_temp_success(mode_111f);
                        s_cbs.on_parsed_oil_temp((uint32_t)oil_111f);
                    } else {
                        ESP_LOGD(TAG, "[22 11 1F] Oil temp out of range: %d (raw=%02X)", (int)oil_111f, (unsigned)d0);
                        record_oil_temp_failure(mode_111f);
                    }
                } else if (pid16 == 0x5822) {
                    // MINI/BMW oil temp PID 5822: °C = A - 60 (°F = A*9/5 - 76)
                    int32_t mini_oil = (int32_t)d0 - 60;
                    if (mini_oil >= -40 && mini_oil <= 215) {
                        ESP_LOGI(TAG, "[22 58 22] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)mini_oil);
                        record_oil_temp_success(OIL_TEMP_MODE_MINI_22_5822);
                        s_cbs.on_parsed_oil_temp((uint32_t)mini_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 58 22] Oil temp out of range: %d (raw=%02X)", (int)mini_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_MINI_22_5822);
                    }
                } else if (pid16 == 0x4402) {
                    if (s_oil_mode_priority[0] == OIL_TEMP_MODE_BMW_G_22_4402 ||
                        s_oil_mode_priority[1] == OIL_TEMP_MODE_BMW_G_22_4402 ||
                        s_oil_mode_priority[2] == OIL_TEMP_MODE_BMW_G_22_4402) {
                        // BMW G-series oil temp PID 4402: °C = (A*256+B)*191.25/255-48 (two bytes)
                        if (values >= 5) {
                            int32_t raw = (int32_t)d0 * 256 + (int32_t)d1;
                            int32_t bmw_g_oil = (int32_t)(raw * 191.25f / 255.0f - 48.0f);
                            if (bmw_g_oil >= -48 && bmw_g_oil <= 143) {
                                ESP_LOGI(TAG, "[22 44 02 G] bytes=%d raw=%04X -> %dC", values-2, (unsigned)raw, (int)bmw_g_oil);
                                record_oil_temp_success(OIL_TEMP_MODE_BMW_G_22_4402);
                                s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                            } else {
                                ESP_LOGD(TAG, "[22 44 02 G] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_g_oil, (unsigned)d0, (unsigned)d1);
                                record_oil_temp_failure(OIL_TEMP_MODE_BMW_G_22_4402);
                            }
                        } else {
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_G_22_4402);
                        }
                    } else {
                        // BMW F-series oil temp PID 4402: °C = B - 64 (second data byte d1 of the response)
                        if (values >= 5) {
                            int32_t bmw_oil = (int32_t)d1 - 64;
                            if (bmw_oil >= -40 && bmw_oil <= 215) {
                                ESP_LOGI(TAG, "[22 44 02 F] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d1, (int)bmw_oil);
                                record_oil_temp_success(OIL_TEMP_MODE_BMW_22_4402);
                                s_cbs.on_parsed_oil_temp((uint32_t)bmw_oil);
                            } else {
                                ESP_LOGD(TAG, "[22 44 02 F] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_oil, (unsigned)d0, (unsigned)d1);
                                record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                            }
                        } else {
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_4402);
                        }
                    }
                } else if (pid16 == 0xD002) {
                    // BMW G-series oil-pan oil temp PID D002: °C = (A*256+B)*191.25/255-48 (two bytes)
                    if (values >= 5) {
                        int32_t raw = (int32_t)d0 * 256 + (int32_t)d1;
                        int32_t bmw_g_oil = (int32_t)(raw * 191.25f / 255.0f - 48.0f);
                        if (bmw_g_oil >= -48 && bmw_g_oil <= 143) {
                            ESP_LOGI(TAG, "[22 D0 02] bytes=%d raw=%04X -> %dC", values-2, (unsigned)raw, (int)bmw_g_oil);
                            record_oil_temp_success(OIL_TEMP_MODE_BMW_22_D002);
                            s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                        } else {
                            ESP_LOGD(TAG, "[22 D0 02] Oil temp out of range: %d (A=%02X B=%02X)", (int)bmw_g_oil, (unsigned)d0, (unsigned)d1);
                            record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_D002);
                        }
                    } else {
                        record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_D002);
                    }
                } else if (pid16 == 0x03F3) {
                    // BMW G-series oil temp PID 03F3: °C = A - 40 (Header 7E0 physical addressing)
                    int32_t bmw_g_oil = (int32_t)d0 - 40;
                    if (bmw_g_oil >= -40 && bmw_g_oil <= 215) {
                        ESP_LOGI(TAG, "[22 03 F3] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)bmw_g_oil);
                        record_oil_temp_success(OIL_TEMP_MODE_BMW_22_03F3);
                        s_cbs.on_parsed_oil_temp((uint32_t)bmw_g_oil);
                    } else {
                        ESP_LOGD(TAG, "[22 03 F3] Oil temp out of range: %d (raw=%02X)", (int)bmw_g_oil, (unsigned)d0);
                        record_oil_temp_failure(OIL_TEMP_MODE_BMW_22_03F3);
                    }
                } else if (pid16 == 0x1017 || pid16 == 0x0011 || pid16 == 0x1C00) {
                    ESP_LOGI(TAG, "[22 10 17] bytes=%d raw=0x%02X -> %dC", values-2, (unsigned)d0, (int)d0 - 40);
                    record_oil_temp_success(OIL_TEMP_MODE_UDS_22_10_17);
                    s_cbs.on_parsed_oil_temp((uint32_t)((int32_t)d0 - 40));
                } else {
                    record_oil_temp_failure(OIL_TEMP_MODE_UDS_22_10_17);
                }
            }
            oil_temp_done: ;
        } else {
            // Invalid data or plain text (NO DATA, SEARCHING, OK, etc.)
            if (strstr(buf, "NO DATA")) {
                ESP_LOGD(TAG, "NO DATA for last PID"); // diagnostics: which PID had no data (timeouts are handled by timestamp-based self-heal)
            } else if (strstr(buf, "SEARCHING")) {
                ESP_LOGD(TAG, "ELM327 searching protocol...");
            } else {
                ESP_LOGD(TAG, "Other response: %.60s", buf); // diagnostics: other unknown response
            }
            // If Mode21 was expected but an unrelated response arrived, record a failure too
            if (s_expect_mode21) {
                ESP_LOGW(TAG, "21 01 expected but got: %.40s", buf);
                record_oil_temp_failure(OIL_TEMP_MODE_TOYOTA_21_01);
                s_expect_mode21 = false;
            }
        }

        // Clear the accumulation buffer after a full response
        s_accum_len = 0;
        s_accum_buf[0] = '\0';
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "Write failed status=%d", param->write.status);
            s_elm_ready = true; // release on write failure too, to prevent the poll task from getting stuck forever
        }
        break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
        s_connected = false;
        s_notify_ready = false;   // disconnect → notifications invalid; must re-subscribe + re-init after reconnect
        s_conn_id = 0xFFFF;
        s_have_service = false;
        s_service_start = 0x0001;
        s_service_end = 0xFFFF;
        s_all_attr_end = 0xFFFF;
        s_have_18f0 = false;
        s_18f0_start = s_18f0_end = 0;
        s_have_ff12 = false;
        s_ff12_start = s_ff12_end = 0;
        s_write_type = ESP_GATT_WRITE_TYPE_RSP; // reset the write type after disconnect
        s_expect_mode21 = false;
        s_char_write_handle = s_char_notify_handle = s_cccd_handle = 0;
        s_accum_len = 0; s_accum_buf[0] = '\0'; // clear the response accumulation buffer
        s_zc6_can_monitor_active = false;        // reset the CAN continuous monitor
        s_zc6_can_monitor_len = 0;
        s_got_valid_data = false;                // prevent the stale pre-disconnect flag from being mis-consumed after reconnect
        s_last_mode21_oil = -100;
        s_mode21_hold_cnt = 0;
        s_protocol_detect_idx = -1;  // clear protocol detection state
        s_protocol_detect_got_response = false;
        s_protocol_detect_rpm = -1;
        s_oil_query_mode = 0;  // reset the oil-temp query counter
        if (s_cbs.on_disconnected) s_cbs.on_disconnected();
        // Always resume scanning after disconnect:
        //  - Scan mode: keep listing devices
        //  - Normal mode: auto-reconnect to the target (auto-recovers after a drop / self-heal forced disconnect, no manual reconnect needed)
        start_scan();
        break;
    }
    default:
        break;
    }
}


void elm327_ble_start_default(const char *target_name, const uint8_t mac[6]) {

    const elm327_ble_callbacks_t cbs = {
        .on_connected = default_on_connected,
        .on_disconnected = default_on_disconnected,
        .on_raw_notify = default_on_raw_notify,
        .on_parsed_rpm = default_on_parsed_rpm,
        .on_parsed_speed_kmh = default_on_parsed_speed,
        .on_parsed_coolant_temp = default_on_parsed_coolant_temp,
        .on_parsed_intake_temp = default_on_parsed_intake_temp,
        .on_parsed_oil_temp = default_on_parsed_oil_temp,
        .on_parsed_load_pct = default_on_parsed_load_pct,
        .on_parsed_control_module_voltage = default_on_parsed_control_module_voltage,
        .on_parsed_throttle_position = default_on_parsed_throttle_position,
        .on_parsed_gear = default_on_parsed_gear,
        .on_parsed_manifold_pressure = default_on_parsed_manifold_pressure,
        .on_parsed_afr = default_on_parsed_afr,
    };
    s_scan_only_mode = false;
    bool mac_set = mac && (mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) != 0;
    if (mac_set) {
        memcpy(s_target_bda, mac, sizeof(esp_bd_addr_t));
        s_target_bda_valid = true;
    } else {
        s_target_bda_valid = false;
    }
    elm327_ble_init_and_start(target_name, &cbs);
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 4096, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

// ---- Scan-mode implementation ----

static void ble_ensure_init(void) {
    if (s_ble_inited) return;
    // Init the BLE stack (no target name; init only)
    elm327_ble_init_and_start(NULL, NULL);
}

// For callers that only need peripheral BLE advertising (RaceChrono DIY / SkyGauge pairing) without connecting an OBD device right now:
// idempotently bring up the controller + Bluedroid + GAP/GATTC callbacks, without starting any ELM327 scan/connect.
void elm327_ble_ensure_stack_init(void) {
    ble_ensure_init();
}

void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb) {
    ble_ensure_init();
    s_scan_only_mode = true;
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    ESP_LOGD(TAG, "Starting scan-only mode (%ds)...", duration_s);
    esp_ble_gap_start_scanning(duration_s);
}

void elm327_ble_scan_only_stop(void) {
    esp_ble_gap_stop_scanning();
    s_scan_only_mode = false;
    ESP_LOGD(TAG, "Scan-only stopped. Found %d devices.", s_scan_count);
}

void elm327_ble_connect_by_addr(const uint8_t mac[6], const char *name) {
    if (!mac) return;
    ESP_LOGD(TAG, "Connect by addr: %02X:%02X:%02X:%02X:%02X:%02X (%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name ? name : "");
    s_scan_only_mode = false;
    if (name && name[0]) {
        strncpy(s_target_name, name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    }
    memcpy(s_target_bda, mac, sizeof(esp_bd_addr_t));
    s_target_bda_valid = true;

    // Set default callbacks (if not already set)
    if (!s_cbs.on_connected) {
        s_cbs.on_connected = default_on_connected;
        s_cbs.on_disconnected = default_on_disconnected;
        s_cbs.on_raw_notify = default_on_raw_notify;
        s_cbs.on_parsed_rpm = default_on_parsed_rpm;
        s_cbs.on_parsed_speed_kmh = default_on_parsed_speed;
        s_cbs.on_parsed_coolant_temp = default_on_parsed_coolant_temp;
        s_cbs.on_parsed_intake_temp = default_on_parsed_intake_temp;
        s_cbs.on_parsed_oil_temp = default_on_parsed_oil_temp;
        s_cbs.on_parsed_load_pct = default_on_parsed_load_pct;
        s_cbs.on_parsed_control_module_voltage = default_on_parsed_control_module_voltage;
        s_cbs.on_parsed_throttle_position = default_on_parsed_throttle_position;
        s_cbs.on_parsed_manifold_pressure = default_on_parsed_manifold_pressure;
        s_cbs.on_parsed_afr = default_on_parsed_afr;
    }
    // Start scanning; auto-connect once found
    esp_ble_gap_start_scanning(15);
    // Create the poll task (if not already created)
    if (!s_poll_task_started) {
        xTaskCreate(obd_poll_task, "obd_poll", 4096, NULL, 4, NULL);
        s_poll_task_started = true;
    }
}

bool elm327_ble_is_connected(void) {
    return s_connected;
}

void elm327_ble_disconnect(void) {
    if (s_connected && s_gattc_if != 0 && s_conn_id != 0xFFFF) {
        ESP_LOGD(TAG, "Disconnecting from BLE device...");
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }
}

const char *elm327_ble_get_connected_name(void) {
    return s_target_name;
}

// ---- Oil-temp calibration API implementation ----
void elm327_oil_temp_set_offset(int8_t offset_c) {
    s_oil_temp_offset = offset_c;
    ESP_LOGI(TAG, "OIL temp offset set to %d°C", offset_c);
}

int8_t elm327_oil_temp_get_offset(void) {
    return s_oil_temp_offset;
}

void elm327_oil_temp_get_diag(elm327_oil_diag_t *out) {
    if (!out) return;
    out->mode0_ok = s_oil_diag.mode0_ok;
    out->mode1_ok = s_oil_diag.mode1_ok;
    out->mode2_ok = s_oil_diag.mode2_ok;
    out->mode0_fail = s_oil_diag.mode0_fail;
    out->mode1_fail = s_oil_diag.mode1_fail;
    out->mode2_fail = s_oil_diag.mode2_fail;
    out->last_raw = s_oil_diag.last_raw_temp;
    out->last_filtered = s_oil_diag.last_filtered_temp;
    out->current_mode = s_oil_query_mode;
    
    ESP_LOGI(TAG, "OIL DIAG: Mode0(01 5C)=%u/%u, Mode1(22 10 17)=%u/%u, Mode2(21 01)=%u/%u, Mode3(22 11 1F Mz)=%u/%u, Mode4(22 13 10)=%u/%u, Mode5(CAN 441)=%u/%u, Mode6(22 58 22)=%u/%u, Mode7(22 44 02 F)=%u/%u, Mode8(22 03 F3)=%u/%u, Mode9(22 44 02 G)=%u/%u, Mode10(22 D0 02)=%u/%u, Mode11(22 11 1F BM)=%u/%u",
             out->mode0_ok, out->mode0_fail, out->mode1_ok, out->mode1_fail,
             out->mode2_ok, out->mode2_fail, s_oil_diag.mode3_ok, s_oil_diag.mode3_fail,
             s_oil_diag.mode4_ok, s_oil_diag.mode4_fail, s_oil_diag.mode5_ok, s_oil_diag.mode5_fail,
             s_oil_diag.mode6_ok, s_oil_diag.mode6_fail, s_oil_diag.mode7_ok, s_oil_diag.mode7_fail,
             s_oil_diag.mode8_ok, s_oil_diag.mode8_fail,
             s_oil_diag.mode9_ok, s_oil_diag.mode9_fail,
             s_oil_diag.mode10_ok, s_oil_diag.mode10_fail,
             s_oil_diag.mode11_ok, s_oil_diag.mode11_fail);
}

