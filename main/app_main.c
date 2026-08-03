/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
// 原作者：Ray.Zhai
// 时间：2025-09-14
// 适配 Waveshare ESP32-S3-Touch-LCD-1.85 by adaptation

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include "lvgl.h"

/* Waveshare BSP 驱动 */
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"       // 内部 include CST816.h & TCA9554PWR.h

/* 应用层 */
#include "bsp_obd_dsp/bsp_board.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/racechrono_ble_diy.h"
#include "app_obd_dsp/boot_media_mount.h"
#include "bsp_obd_dsp/rs485_brake_temp.h"
#include "bsp_obd_dsp/ads1115_oil_pressure.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "export_path/ui_ext.h"
#include "app_obd_dsp/app_event.h"

// ===== 三连表角色 =====
// 正常用「设置页 → 下滑进 MULTI-GAUGE 页」选择主/从(存 NVS device_role, 重启生效), 一份固件即可。
// 下面这个宏只是调试用的强制覆盖: 取消注释会忽略设置、强制本板为从表。平时保持注释。
// #define ESPNOW_FORCE_SLAVE

static const char *TAG = "obd_dsp";

extern void ui_init(void);
SemaphoreHandle_t lvgl_mux = NULL; // non-static: used by BLE scan page

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LCD & LVGL 配置 ///////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* 分辨率直接来自 ST77916.h 中的宏 */
#define LCD_H_RES               EXAMPLE_LCD_WIDTH       // 360
#define LCD_V_RES               EXAMPLE_LCD_HEIGHT      // 360
#define LCD_BIT_PER_PIXEL       EXAMPLE_LCD_COLOR_BITS  // 16

/* LVGL 参数 */
#define LVGL_BUFF_SIZE              (LCD_H_RES * 20)
#define LVGL_TICK_PERIOD_MS         2
#define LVGL_TASK_MAX_DELAY_MS      500
#define LVGL_TASK_MIN_DELAY_MS      2
#define LVGL_TASK_STACK_SIZE        (8 * 1024)
#define LVGL_TASK_PRIORITY          4   // 提高(原2): 动画时不易被 BLE/OBD(优先级4) 抢占而掉帧

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LVGL 回调函数 /////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* DMA 传输完成通知 LVGL */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

/* LVGL 刷新回调 */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
}

/* 坐标对齐 (偶数边界) */
static void lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// 触摸输入回调 //////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* 触摸读取回调 (轮询模式) */
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;
    assert(touch);

    uint16_t tp_x, tp_y;
    uint8_t tp_cnt = 0;

    esp_lcd_touch_read_data(touch);

    bool pressed = esp_lcd_touch_get_coordinates(touch, &tp_x, &tp_y, NULL, &tp_cnt, 1);
    if (pressed && tp_cnt > 0) {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "Touch: %d,%d", tp_x, tp_y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LVGL 定时器 & 任务 ////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "lvgl_mux not created");
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    assert(lvgl_mux && "lvgl_mux not created");
    xSemaphoreGive(lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// 主函数 ////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void app_main(void)
{
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    /* 1. NVS 初始化 (必须最先) */
    nvs_storage_init();

    /* 1.5 任务看门狗: 10s 超时, 不订阅 idle task (避免 BLE 阻塞误触发) */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,  // 不监控 idle task
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    uint8_t proto = nvs_cfg_get()->protocol;
    ESP_LOGI("NVS", "protocol=%d", proto);

    theme_cfg_t str_theme = nvs_cfg_get()->theme_cfg;
    ESP_LOGI("NVS", "theme=%d, dominant=%d, secondary=%d",
             str_theme.theme, str_theme.user_theme_domiant_color, str_theme.user_theme_secondary_color);

    const nvs_stat_t *stat = nvs_stat_get();
    ESP_LOGI("NVS", "odo=%" PRIu64 " trip=%" PRIu64 " max_spd=%d avg_spd=%d runtime=%" PRIu64,
             stat->odometer_m, stat->trip_m, stat->max_speed_kmh, stat->avg_speed_kmh, stat->run_time_s);

    /* 1.5 车辆配置初始化 (从NVS加载保存的车辆索引) */
    vehicle_profile_set_active(nvs_cfg_get()->vehicle_profile_idx);
    ESP_LOGI("NVS", "vehicle_profile=%d (%s)",
             nvs_cfg_get()->vehicle_profile_idx, vehicle_profile_get_active()->name);

    /* 2. I2C 总线 0 初始化 (供 TCA9554 IO 扩展器使用, SCL=10 SDA=11) */
    I2C_Init();

    /* 3. IO 扩展器初始化 (TCA9554PWR, I2C 地址 0x20) */
    EXIO_Init();

    /* 4. LCD + 背光 + 触摸 一体初始化
     *    LCD_Init() 内部依次调用:
     *      ST77916_Init() → TCA9554 EXIO2 复位 → QSPI SPI 总线 & ST77916 panel 驱动
     *      Backlight_Init() → LEDC PWM 背光 (GPIO 5)
     *      Touch_Init() → I2C_NUM_1 (SDA=1, SCL=3) CST816 触摸驱动
     *    完成后 panel_handle / tp 均为全局有效变量
     */
    LCD_SetFlushCallback(notify_lvgl_flush_ready, &disp_drv);
    LCD_Backlight = 0;  // 在 LCD_Init 之前设为 0，防止 Backlight_Init 点亮未初始化的屏幕
    LCD_Init();

    /* 5. LVGL 初始化 */
    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    /* 分配双缓冲 (DMA 内存)。缓冲加大 -> 全屏渲染条带减半 -> 帧率更高。
       仅影响 LVGL 渲染分块，不改 SPI 单次传输大小(仍走 max_transfer_sz 分块)，故不会花屏。
       内部 DMA RAM 不足时自动回退到原始 20 行，避免开机 OOM。 */
    size_t buf_px = LCD_H_RES * 40;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        heap_caps_free(buf1); heap_caps_free(buf2);
        buf_px = LVGL_BUFF_SIZE;   // 回退 20 行
        buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
        buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    }
    assert(buf1 && buf2);
    ESP_LOGI(TAG, "LVGL draw buffer: %d lines x2", (int)(buf_px / LCD_H_RES));
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_px);

    /* 注册显示驱动 */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;      // 来自 ST77916.h extern
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    /* LVGL tick 定时器 (2ms 周期) */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* 注册触摸输入设备 (轮询模式, 使用 Touch_Init 创建的全局 tp) */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = tp;               // 来自 CST816.h extern
    lv_indev_drv_register(&indev_drv);

    /* 6. 启动 LVGL 任务 */
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    static TaskHandle_t s_lvgl_task_handle = NULL;
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, &s_lvgl_task_handle);
    // 暴露给 ui.c 用于闪烁期间临时提优先级
    extern TaskHandle_t g_lvgl_task_handle;
    g_lvgl_task_handle = s_lvgl_task_handle;

    /* 7. 启动 UI */
    ESP_LOGI(TAG, "Start UI");
    if (lvgl_lock(-1)) {
        ui_init();
        ui_ext_init();
        lvgl_unlock();
    }
    app_event_init();

    /* 7.5 提前挂载 bootmedia SPIFFS (省 ~300ms 黑屏) */
    boot_media_mount();

    /* 8. 按角色分支: 主表(连 ELM327 读数 + ESP-NOW 广播) / 从表(只收主表数据显示) */
    uint8_t dev_role = nvs_cfg_get()->device_role;
#ifdef ESPNOW_FORCE_SLAVE
    dev_role = ESPNOW_ROLE_SLAVE;   // 步骤1测试: 强制从表
#endif

    if (dev_role == ESPNOW_ROLE_SLAVE) {
        /* ---- 从表: 不连 ELM327, 只启动 ESP-NOW 接收, UI 显示主表广播的数据 ----
           屏幕导航(未绑定主表→蓝牙配对页; 已绑定→正常开机流程)由 ui.c 的
           boot_enter_default_page() 统一处理, 这里不用管。 */
        ESP_LOGI(TAG, "Device role: SLAVE (ESP-NOW receiver, no BLE/OBD)");
        espnow_link_start_slave();
        {
            const uint8_t *bm = espnow_link_get_bound_master_mac();
            ESP_LOGI(TAG, "Bound master MAC at boot: %02x:%02x:%02x:%02x:%02x:%02x (0=unbound)",
                     bm[0], bm[1], bm[2], bm[3], bm[4], bm[5]);
        }
    } else {
        /* ---- 主表 / 单机: 都是 BLE OBD 全链路; 区别仅 ESP-NOW(=WiFi)是否启动 ----
           STANDALONE: 不启 WiFi/ESP-NOW, 现有(已选主/从)设备不受影响。 */
        bool espnow_on = (dev_role == ESPNOW_ROLE_MASTER);
        ESP_LOGI(TAG, "Device role: %s (BLE/OBD%s)",
                 espnow_on ? "MASTER" : "STANDALONE",
                 espnow_on ? " + ESP-NOW broadcast" : ", no WiFi/ESP-NOW");

        /* 8.1 启动 BLE OBD - 仅 NVS 有保存设备名时自动连; 否则等用户在扫描页手动选。
               MASTER 即使还没配置 OBD 设备, 也要把蓝牙栈启起来, 以便广播 SkyGauge 配对信号。 */
        const nvs_user_cfg_t *user_cfg = nvs_cfg_get();
        if (user_cfg->ble_device_name[0] != '\0') {
            ESP_LOGI(TAG, "BLE target device: %s", user_cfg->ble_device_name);
            elm327_ble_start_default(user_cfg->ble_device_name, user_cfg->ble_obd_mac);
        } else if (espnow_on) {
            ESP_LOGI(TAG, "No saved BLE device, but MASTER needs BLE stack for SkyGauge pairing broadcast");
            elm327_ble_ensure_stack_init();
        } else {
            ESP_LOGI(TAG, "No saved BLE device, waiting for user selection");
        }

        /* 8.5 启动 RaceChrono BLE DIY 服务（仅 BT 栈已初始化时）——
               MASTER 时该服务同时承载 SkyGauge 配对广播, 与是否已配置 OBD 设备无关。 */
        if (user_cfg->ble_device_name[0] != '\0' || espnow_on) {
            vTaskDelay(pdMS_TO_TICKS(500));
            racechrono_ble_diy_start();
        }

        /* 9. 启动 RS485 刹车温度采集 */
        rs485_brake_temp_start();

        /* 9.5 启动油压采集（ESP32 ADC 直连） */
        oil_pressure_start();

        /* 9.8 启动 ESP-NOW 广播(把本机 OBD 数据缓存发给从表) —— 仅 MASTER;
               STANDALONE 跳过, 全程不初始化 WiFi(省射频/功耗, 且不干扰 BLE)。 */
        if (espnow_on) {
            espnow_link_start_master();
        }

        /* 10. 里程统计任务(仅主表统计, 避免从表重复计) */
        vMileageDataStatisticTask();
    }
}

