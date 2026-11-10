#pragma once

#include <stddef.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/tpms_fbb0.h"

#define TPMS_SCAN_MAX 20

typedef struct {
    tpms_fbb0_data_t data;
    uint8_t mac[6];                    /* diagnostic only, never binding identity */
    uint8_t addr_type;
    int rssi;
} tpms_scan_result_t;

typedef enum { TPMS_SCAN_IDLE, TPMS_SCAN_RUNNING,
               TPMS_SCAN_DONE, TPMS_SCAN_FAILED } tpms_scan_state_t;

/* Only valid FBB0 + CRC broadcasts enter the stable, ID-deduplicated list.
 * Pairing saves an ID, not a GATT connection or a promise of live data.
 * Public control functions are called serially by the LVGL task. */
esp_err_t tpms_ble_scan_start(void);
void tpms_ble_scan_stop(void);
tpms_scan_state_t tpms_ble_scan_snapshot(tpms_scan_result_t *out, size_t cap, size_t *count);
esp_err_t tpms_ble_bind(unsigned wheel, const char *sensor_id);
esp_err_t tpms_ble_unbind(unsigned wheel);
bool tpms_ble_is_bound(const char *sensor_id);

/* Freshness is a display timeout, not a guaranteed sensor reporting period. */
#define TPMS_FRESH_MS 120000u
typedef struct {
    char sensor_id[7];
    bool bound, received, fresh;
    uint32_t age_ms;
    tpms_scan_result_t sample;
} tpms_wheel_data_t;
void tpms_ble_snapshot(tpms_wheel_data_t out[NVS_TPMS_WHEELS]);
/* LVGL control context only; never steals another client's scan ownership.
 * allow=false gives reconnect/manual pairing priority; cached samples survive. */
void tpms_ble_service_step(bool allow_background);
void tpms_ble_background_pause(bool pause);
