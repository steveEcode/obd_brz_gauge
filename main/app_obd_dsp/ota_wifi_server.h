#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi OTA 服务状态
typedef enum {
    OTA_WIFI_STATE_IDLE = 0,      // 未启动
    OTA_WIFI_STATE_STARTING,      // 正在启动
    OTA_WIFI_STATE_READY,         // 就绪，等待连接
    OTA_WIFI_STATE_RECEIVING,     // 正在接收数据 / 正在写入 Flash
    OTA_WIFI_STATE_DONE,          // 完成（即将重启）
    OTA_WIFI_STATE_ERROR,         // 错误（服务仍在运行，可重试）
} ota_wifi_state_t;

// WiFi OTA 会话信息（由 ota_wifi_server_start 填充）
typedef struct {
    char ssid[33];           // WiFi SSID
    char password[17];       // WiFi 密码
    char ip[16];             // 设备 IP 地址
    char token[17];          // 认证 token (hex)
    uint16_t port;           // HTTP 端口
} ota_wifi_info_t;

// 启动 WiFi OTA 服务（LVGL 任务调用）。
//
// 进入时先释放单射频与内部 DRAM 的占用：ble_hub_shutdown()（BLE 彻底关闭，
// 不可逆，需重启恢复）、视频播放缓冲、theme_0 图片缓存。调用方必须先删除
// 所有主题页面对象。返回 true 表示启动成功，info 中填充 WiFi 信息。
bool ota_wifi_server_start(ota_wifi_info_t *info);

// 停止 WiFi OTA 服务。与启动/停止并发时返回 false，调用方稍后重试；
// 不等待生命周期锁，避免 LVGL 调用方与等待 LVGL 的 HTTP handler 互锁。
bool ota_wifi_server_stop(void);

// 从 ota_wifi_server_start() 进入起到 ota_wifi_server_stop() 结束为 true。
// SD 导入、主题库切换等后台任务据此暂停。
bool ota_wifi_server_is_active(void);

/* Destructive, local-screen confirmation only; call from an internal-stack
 * worker, NEVER under lvgl_mux. Gates new requests on the HTTP task, refuses
 * active uploads, stops HTTP and takes the storage job lock before formatting.
 * If reboot_required is set, maintenance stays latched until caller reboots,
 * even on error. No USB takeover/background card jobs are allowed meanwhile. */
esp_err_t ota_wifi_server_format_sd(char *detail, size_t capacity, bool *reboot_required);


// 当前 SoftAP SSID；未启动时为 ""。
const char *ota_wifi_server_get_ssid(void);

#ifdef __cplusplus
}
#endif
