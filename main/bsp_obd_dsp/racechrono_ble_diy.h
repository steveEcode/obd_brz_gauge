#pragma once

#include <stdbool.h>
#include "esp_gap_ble_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start RaceChrono BLE DIY GATTS service (UUID 0x1FF8).
// Requires BLE controller + bluedroid to be enabled by caller.
// When enable_racechrono is false, only Info (0x1FFA) + OTA (0x1FFB) services
// are created, skipping RC (0x1FF8) and Pair (0x1FF9); the device also stays
// invisible (no advertising) until OTA mode is entered.
void racechrono_ble_diy_start(bool enable_racechrono);

// Enter/leave OTA advertising mode: publishes the OTA advert (Info+OTA UUIDs +
// device name) so the phone OTA app can discover this device, and restores the
// previous advertising state when leaving.
void racechrono_ble_diy_set_ota_mode(bool enable);

// Current advertising/GAP device name (role dependent: "SkyGauge-XXYY" on MASTER,
// "SkyGarageRC" elsewhere). Valid after GATTS registration.
const char *racechrono_ble_diy_get_adv_name(void);

// Forward GAP callbacks from the app's single BLE GAP callback.
void racechrono_ble_diy_handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

// Query connection state to RaceChrono app.
bool racechrono_ble_diy_is_connected(void);

#ifdef __cplusplus
}
#endif
