#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBD_OTA_SERVICE_UUID     0x1FFB
#define OBD_OTA_CHAR_CONTROL     0x0001
#define OBD_OTA_CHAR_DATA        0x0002
#define OBD_OTA_CHAR_STATUS      0x0003

void ota_update_ble_start(esp_gatt_if_t gatts_if);
void ota_update_ble_on_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
void ota_update_ble_on_connect(esp_gatt_if_t gatts_if, uint16_t conn_id);
void ota_update_ble_on_disconnect(void);
bool ota_update_ble_is_connected(void);
bool ota_update_ble_is_active(void);

#ifdef __cplusplus
}
#endif
