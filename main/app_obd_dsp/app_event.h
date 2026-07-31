#pragma once
// ================================================================
//  app_event.h — 跨 task 事件队列
//
//  所有跨 task 通信走此队列, 消除直接函数调用的竞态条件。
//  生产者 (ESP-NOW callback / BLE callback): app_event_send()
//  消费者 (LVGL task / poll task):          app_event_recv()
//
//  迁移计划:
//    1. ESP-NOW recv → ui_showroom_set_page_from_sync() 改为 app_event_send()
//    2. BLE disconnect → 直接改 s_connected 改为 app_event_send()
//    3. LVGL task my_timerMain 里 app_event_recv() 处理所有事件
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // ESP-NOW → UI
    APP_EVT_ESPNOW_SYNC_SLOT = 1,   // data.u8 = slot (0-7)
    APP_EVT_ESPNOW_INTRO_STEP,      // data.u8 = intro_step (开机动画进度)
    APP_EVT_ESPNOW_ENTER_SHOWROOM,  // 无 data
    APP_EVT_ESPNOW_OBD_DATA,        // 从表收到主表 OBD 数据 (已在 espnow_link 处理)

    // BLE → poll task
    APP_EVT_BLE_CONNECTED,          // 无 data
    APP_EVT_BLE_DISCONNECTED,       // 无 data

    // 内部
    APP_EVT_BOOT_VIDEO_DONE,        // 视频播放完毕
    APP_EVT_SHOWROOM_EXIT,          // 退出 showroom
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        uint8_t  u8;
        uint16_t u16;
        int16_t  s16;
        uint32_t u32;
    } data;
} app_event_t;

#define APP_EVENT_QUEUE_DEPTH 16

// 初始化 (app_main 调用一次)
void app_event_init(void);

// 发送事件 (任意 task, 不阻塞)
bool app_event_send(app_event_type_t type, uint32_t data);

// 接收事件 (非阻塞, 无事件返回 false)
bool app_event_recv(app_event_t *out);

// 获取队列句柄 (用于 xQueueReceive 带超时)
QueueHandle_t app_event_queue(void);

#ifdef __cplusplus
}
#endif
