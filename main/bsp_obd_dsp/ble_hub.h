#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * ble_hub - the only owner of the Bluedroid GAP / GATTC / GATTS callbacks.
 *
 * Bluedroid keeps exactly one callback per layer; modules that call
 * esp_ble_gap_register_callback() themselves silently replace each other.
 * Every BLE module instead registers with the hub under a fixed app id:
 *   - GAP events are fanned out to every registered module (each filters);
 *   - GATTC/GATTS events are routed by gattc_if/gatts_if, which the hub learns
 *     from the REG_EVT whose app_id matches the module;
 *   - scanning is arbitrated: one owner at a time.
 *
 * ble_hub_init() brings up controller + Bluedroid (BLE only) once; it is safe
 * to call repeatedly.
 * -------------------------------------------------------------------------- */

typedef enum {
    BLE_HUB_APP_ELM327     = 0x00,  /* GATTC: ELM327 adapter */
    BLE_HUB_APP_RACECHRONO = 0x52,  /* GATTS: RaceChrono DIY */
    BLE_HUB_APP_GAUGE_PAIR = 0x53,  /* GATTC: multi-gauge pairing */
    BLE_HUB_APP_TPMS       = 0x54,  /* GAP: TPMS sensor pairing */
} ble_hub_app_t;

typedef struct {
    esp_gap_ble_cb_t gap_cb;     /* may be NULL */
    esp_gattc_cb_t   gattc_cb;   /* may be NULL; registers a GATTC app when set */
    esp_gatts_cb_t   gatts_cb;   /* may be NULL; registers a GATTS app when set */
    /* Optional one-way shutdown wakeup, after is_ready() becomes false and
     * outside the hub lock. Only wake tasks; do not call BT APIs or block. */
    void          (*shutdown_cb)(void);
} ble_hub_client_t;

esp_err_t ble_hub_init(void);
bool      ble_hub_is_ready(void);
/* Register a module; issues esp_ble_gattc_app_register / gatts_app_register
 * for the given app id as needed. */
esp_err_t ble_hub_register(ble_hub_app_t app, const ble_hub_client_t *client);

/* Scan arbitration. start fails with ESP_ERR_INVALID_STATE while another app
 * owns the scanner. duration_s 0 = until stopped. */
esp_err_t ble_hub_scan_start(ble_hub_app_t app, const esp_ble_scan_params_t *params,
                             uint32_t duration_s);
esp_err_t ble_hub_scan_stop(ble_hub_app_t app);
bool      ble_hub_scan_active(ble_hub_app_t *owner);

/* Release BT entirely (entering WiFi OTA mode); a reboot is required after. */
esp_err_t ble_hub_shutdown(void);

#ifdef __cplusplus
}
#endif
