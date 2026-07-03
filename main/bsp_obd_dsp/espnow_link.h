#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_ROLE_MASTER 0
#define ESPNOW_ROLE_SLAVE  1

// 主表: 初始化 WiFi + ESP-NOW, 周期性把 OBD 数据缓存广播给从表。
// 与 BLE(连 ELM327)共存运行(ESP32-S3 单射频分时复用, sdkconfig 已开软件共存)。
void espnow_link_start_master(void);

// 从表: 初始化 WiFi + ESP-NOW, 接收主表广播并写入本地 OBD 数据缓存(不连 ELM327)。
void espnow_link_start_slave(void);

// 从表: 最近 ~2s 内是否收到过主表数据(用于"等待主机"提示)。
bool espnow_link_slave_has_data(void);

// 从表: 最近收到的主表名字(空串=尚未收到)。
const char *espnow_link_get_master_name(void);

#ifdef __cplusplus
}
#endif
