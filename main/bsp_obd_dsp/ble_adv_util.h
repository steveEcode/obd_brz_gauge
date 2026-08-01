#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 从 BLE 广播/扫描响应数据里提取设备名(Complete/Shortened Local Name)。
// adv_data 指向 adv_data_len + scan_rsp_len 字节的连续缓冲区(ESP-IDF GAP 扫描回调的原始格式)。
// 找不到名字时 out_name 写为空串。
void ble_adv_extract_name(const uint8_t *adv_data, uint8_t adv_data_len, uint8_t scan_rsp_len,
                           char *out_name, size_t out_name_len);

#ifdef __cplusplus
}
#endif
