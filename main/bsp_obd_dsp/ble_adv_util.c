#include "ble_adv_util.h"
#include <string.h>
#include "esp_gap_ble_api.h"

void ble_adv_extract_name(const uint8_t *adv_data, uint8_t adv_data_len, uint8_t scan_rsp_len,
                           char *out_name, size_t out_name_len) {
    if (!out_name || out_name_len == 0) return;
    out_name[0] = '\0';
    if (!adv_data) return;

    uint8_t total_len = adv_data_len + scan_rsp_len;
    uint8_t idx = 0;
    while (idx + 1 < total_len) {
        uint8_t field_len = adv_data[idx];
        if (field_len == 0) break;
        if (idx + 1 + field_len > total_len) break;

        uint8_t ad_type = adv_data[idx + 1];
        uint8_t payload_len = field_len - 1;
        const uint8_t *payload = &adv_data[idx + 2];

        if ((ad_type == ESP_BLE_AD_TYPE_NAME_CMPL || ad_type == ESP_BLE_AD_TYPE_NAME_SHORT) && payload_len > 0) {
            size_t copy_len = payload_len < (out_name_len - 1) ? payload_len : (out_name_len - 1);
            memcpy(out_name, payload, copy_len);
            out_name[copy_len] = '\0';
            return;
        }
        idx += (uint8_t)(field_len + 1);
    }
}
