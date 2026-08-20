#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// WiFi OTA 服务状态
typedef enum {
    OTA_WIFI_STATE_IDLE = 0,      // 未启动
    OTA_WIFI_STATE_STARTING,      // 正在启动
    OTA_WIFI_STATE_READY,         // 就绪，等待连接
    OTA_WIFI_STATE_RECEIVING,     // 正在接收数据
    OTA_WIFI_STATE_DONE,          // 完成
    OTA_WIFI_STATE_ERROR,         // 错误
} ota_wifi_state_t;

// WiFi OTA 信息（通过 BLE 通知 APP）
typedef struct {
    char ssid[33];           // WiFi SSID
    char password[17];       // WiFi 密码
    char ip[16];             // 设备 IP 地址
    char token[17];          // 认证 token (hex)
    uint16_t port;           // HTTP 端口
} ota_wifi_info_t;

// 回调：WiFi 状态变化时通知 BLE 层
typedef void (*ota_wifi_status_cb_t)(ota_wifi_state_t state, const char *message, uint32_t received, uint32_t expected);

// 启动 WiFi OTA 服务
// 返回 true 表示启动成功，info 中填充 WiFi 信息
bool ota_wifi_server_start(ota_wifi_info_t *info, ota_wifi_status_cb_t callback);

// 停止 WiFi OTA 服务
void ota_wifi_server_stop(void);

// 获取当前状态
ota_wifi_state_t ota_wifi_server_get_state(void);

// WiFi OTA 会话是否仍然占用 SoftAP/HTTP 资源。
// BLE 断开是 BLE -> WiFi OTA 的正常切换，不应因此中止 busy 会话。
bool ota_wifi_server_is_busy(void);

#ifdef __cplusplus
}
#endif
