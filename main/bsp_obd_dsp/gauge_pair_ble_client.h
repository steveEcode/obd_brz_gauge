#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 三连表蓝牙配对协议常量: 主表(racechrono_ble_diy.c 里独立的一张配对服务属性表, 和
// RaceChrono 服务共用同一份广播但各自是独立的 GATT Primary Service)与本文件(从表客户端)共用。
#define GAUGE_PAIR_SERVICE_UUID 0x1FF9
#define GAUGE_PAIR_CHAR_MAC     0x0003

#define GAUGE_PAIR_SCAN_MAX_DEVICES 16

typedef struct {
    char    name[32];
    uint8_t addr[6];
    int     rssi;
} gauge_pair_scan_result_t;

// 每发现一台新设备(广播名以 "SkyGauge" 开头)调用一次
typedef void (*gauge_pair_scan_cb_t)(const gauge_pair_scan_result_t *dev, int total_count);

// 配对结果回调; success=true 时 name/mac 有效
typedef void (*gauge_pair_result_cb_t)(bool success, const char *name, const uint8_t mac[6]);

// 从表: 扫描附近广播 "SkyGauge" 前缀的主表设备(duration_s 秒)
void gauge_pair_ble_scan_start(int duration_s, gauge_pair_scan_cb_t cb);
void gauge_pair_ble_scan_stop(void);

// 从表: 连接选中的主表设备, 读取其配对特征值(本机 ESP-NOW MAC), 读到后自动断开。
// 结果(成功/失败)通过 cb 回调一次; 调用方需自行处理 NVS 落盘与界面跳转。
void gauge_pair_ble_connect(const uint8_t addr[6], const char *name, gauge_pair_result_cb_t cb);

#ifdef __cplusplus
}
#endif
