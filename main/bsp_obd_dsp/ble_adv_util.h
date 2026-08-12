#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extract the device name (Complete/Shortened Local Name) from BLE advertising/scan response data.
// adv_data points to a contiguous buffer of adv_data_len + scan_rsp_len bytes (raw format from the ESP-IDF GAP scan callback).
// If no name is found, out_name is set to an empty string.
void ble_adv_extract_name(const uint8_t *adv_data, uint8_t adv_data_len, uint8_t scan_rsp_len,
                           char *out_name, size_t out_name_len);

#ifdef __cplusplus
}
#endif
