/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
// Original author: Ray.Zhai
// Date: 2025-09-14
// Adapted for Waveshare ESP32-S3-Touch-LCD-1.85 by adaptation

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"

#include "lvgl.h"
#include "cJSON.h"

/* Waveshare BSP drivers */
#include "bsp_obd_dsp/hw/hw_board.h"               /* selects V1 / V2 / V3 pins+features */
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#if HW_HAS_TCA9554
#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#endif
#include "bsp_obd_dsp/lcd_driver/ST77916.h"       // internally includes CST816.h & TCA9554PWR.h

/* Application layer */
#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/sd_card.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/racechrono_ble_diy.h"
#if HW_HAS_ADS1115_OIL_PRESSURE
#include "bsp_obd_dsp/ads1115_oil_pressure.h"
#endif
#include "app_obd_dsp/coredump_store.h"
#include "app_obd_dsp/storage_service.h"
#include "app_obd_dsp/bootmedia_serial.h"
#include "app_obd_dsp/lvgl_runtime.h"
#include "app_obd_dsp/lvgl_flush_sync.h"
#include "app_obd_dsp/signal_store.h"
#include "app_tasks.h"
#include "app_obd_dsp/app_event.h"
#include "app_obd_dsp/perf_stats.h"
#include "vehicle/vp_manager.h"
#include "export_path/ui_ext.h"

// ===== Triple-gauge roles =====
// Normally select master/slave via "Settings page -> swipe down to the MULTI-GAUGE page" (stored in NVS device_role, effective after reboot); one firmware is enough.

static const char *TAG = "obd_dsp";

static void mark_app_valid_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(15000));

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Current firmware marked valid");
    } else if (err != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to mark firmware valid: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* cJSON trees (vehicle profiles, theme manifests / sidecars, /api/v1 bodies)
 * go to PSRAM; internal RAM only when PSRAM is exhausted. */
static void *cjson_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

extern void ui_init(void);
SemaphoreHandle_t lvgl_mux = NULL; // non-static: used by BLE scan page

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LCD & LVGL configuration ///////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* Resolution comes directly from the macros in ST77916.h */
#define LCD_H_RES               EXAMPLE_LCD_WIDTH       // 360
#define LCD_V_RES               EXAMPLE_LCD_HEIGHT      // 360

/* LVGL parameters */
#define LVGL_BUFF_SIZE              (LCD_H_RES * 20)
#define LVGL_TICK_PERIOD_MS         2
#define LVGL_TASK_STACK_SIZE        (8 * 1024)
#define LVGL_TASK_PRIORITY          4   // raised (was 2): less prone to dropping frames from preemption by BLE/OBD (priority 4) during animations

/* ------------------------------------------------------------------ */
/* task policy: every firmware task's name/stack/prio/core lives here   */
/* ------------------------------------------------------------------ */

/* Priority bands:
 *   5 - startup-critical paths (boot video workers, USB boot media);
 *   4 - interaction + comms trunk (LVGL, OBD poll, links, ESP-NOW, RC);
 *   3 - theme video workers and file jobs;
 *   1-2 - background maintenance (storage, theme library, perf, OTA helpers).
 * Core map: the OBD poll and video reader stay on core 0 with the BT stack;
 * Place LVGL and the video decoder on core 1, OBD polling on core 0.
 * They still share buses and locks; device latency needs measurement. */
static const app_task_policy_t s_task_policy[APP_TASK_COUNT] = {
    [APP_TASK_LVGL]                = { "LVGL",           LVGL_TASK_STACK_SIZE, LVGL_TASK_PRIORITY,        1 },
    [APP_TASK_OBD_POLL_BLE]        = { "obd_poll",       5120,                 4,                        0 },
    [APP_TASK_VIDEO_READER_BOOT]   = { "vid_read",       4096,                 5,                        0 },
    [APP_TASK_VIDEO_DECODER_BOOT]  = { "vid_dec",        6144,                 5,                        1 },
    [APP_TASK_VIDEO_READER_THEME]  = { "vid_read",       4096,                 3,                        0 },
    [APP_TASK_VIDEO_DECODER_THEME] = { "vid_dec",        6144,                 3,                        1 },
    [APP_TASK_ESPNOW_TX]           = { "espnow_tx",      3584,                 4,                       -1 },
    [APP_TASK_ESPNOW_PRES]         = { "espnow_pres",    2560,                 4,                       -1 },
    [APP_TASK_RC_STREAM]           = { "rc_stream",      4096,                 4,                       -1 },
    [APP_TASK_OIL_PRESSURE]        = { "oil_press",      3072,                 4,                       -1 },
    [APP_TASK_THEME_BOOT]          = { "theme_boot",     6144,                 3,                       -1 },
    [APP_TASK_SD_JOB]              = { "ui_sd_job",      8192,                 3,                       -1 },
    [APP_TASK_THEME_LIB]           = { "theme_lib",      4096, tskIDLE_PRIORITY + 2,                       -1 },
    [APP_TASK_STORAGE]             = { "storage",        8192, tskIDLE_PRIORITY + 1,                       -1 },
    [APP_TASK_PERF_STATS]          = { "perf_stats",     4096, tskIDLE_PRIORITY + 1,                       -1 },
    [APP_TASK_OTA_VALID]           = { "ota_valid",      4096, tskIDLE_PRIORITY + 1,                       -1 },
    [APP_TASK_OTA_TIMEOUT]         = { "ota_timeout",    3072, tskIDLE_PRIORITY + 1,                       -1 },
    [APP_TASK_BOOTMEDIA_USB]       = { "bootmedia_usb",  8192,                 5,                       -1 },
    [APP_TASK_THEME_PAGE_PREP]     = { "theme_page",     6144,                 3,                       -1 },
};

const app_task_policy_t *app_task_policy(app_task_id_t id)
{
    if ((unsigned)id >= (unsigned)APP_TASK_COUNT) return NULL;
    const app_task_policy_t *p = &s_task_policy[id];
    return p->name ? p : NULL;
}

TaskHandle_t app_task_start(app_task_id_t id, TaskFunction_t fn, void *arg)
{
    const app_task_policy_t *p = app_task_policy(id);
    if (!p || !fn) return NULL;

    TaskHandle_t h = NULL;
    BaseType_t ok = (p->core < 0)
        ? xTaskCreate(fn, p->name, p->stack, arg, p->prio, &h)
        : xTaskCreatePinnedToCore(fn, p->name, p->stack, arg, p->prio, &h, p->core);
    return ok == pdPASS ? h : NULL;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LVGL callbacks /////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* Notify LVGL on DMA transfer complete */
static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    return lvgl_flush_complete_isr(disp_driver);
}

/* LVGL flush callback */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    PERF_BEGIN(perf_lcd_start);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color_map);
    PERF_END(PERF_LCD_SUBMIT, perf_lcd_start,
             (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1));
}

/* Coordinate alignment (even boundaries) */
static void lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Touch input callbacks //////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* Touch read callback (polling mode) */
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
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// LVGL timers & tasks ////////////////////////////////////////////////////////////////////////////////
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

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Main function ////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void app_main(void)
{
    /* Hide uninitialized panel RAM even while storage/config bring-up blocks. */
    LCD_BacklightOffEarly();
    perf_stats_start();
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    cJSON_Hooks json_hooks = { .malloc_fn = cjson_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&json_hooks);

    /* 1. Persistent state (must be first) */
    nvs_app_init();             /* nvs_app partition: survives web-flasher complete reflash */
    ESP_ERROR_CHECK(nvs_storage_init());
    signal_store_init();        /* zero all signal slots before any OBD data arrives */

    /* 1.1 SD card (no I2C/EXIO needed: D3 has a pull-up), then storage bring-up:
     *     first-boot layout migration or config mount, theme_0 resume, boot-time
     *     /OBD/IMPORT (RESET / ACTIVATE / vehicle JSON). Best-effort, never fatal. */
    esp_err_t sd_err = sd_card_init();
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (%s)", esp_err_to_name(sd_err));
    }
    storage_service_boot();
    coredump_store_boot_check();    /* after the migration, which erases the coredump */
    vp_manager_init();              /* active profile; falls back to obd2.generic */

    /* 1.2 Task watchdog: 10s timeout, idle task not subscribed (avoids false triggers when BLE blocks) */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 10000,
        .idle_core_mask = 0,  // do not monitor idle task
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    nvs_user_cfg_t user_cfg = nvs_cfg_read();
    ESP_LOGI(TAG, "cfg: proto=%u theme=%u vehicle=%s",
             user_cfg.protocol, user_cfg.theme_cfg.theme, vp_manager_active_id());

    /* 2. I2C bus init (used by the TCA9554 IO expander + CST816 touch on V1, CST816 touch only on V2/V3) */
    I2C_Init();

    /* 3. IO expander init (TCA9554PWR, I2C address 0x20) — V1 board only; V2/V3 have no expander */
#if HW_HAS_TCA9554
    EXIO_Init();
#endif

    /* 4. LCD + backlight + touch combined init
     *    LCD_Init() internally calls, in order:
     *      Backlight_Init() → LEDC PWM off (V1: GPIO 5; V2/V3: GPIO 15)
     *      ST77916_Init() → reset (V1: TCA9554 EXIO2; V2/V3: direct GPIO 47) → QSPI SPI bus & ST77916 panel driver
     *      Touch_Init() → reuses I2C bus CST816 touch driver (V1: SCL=10 SDA=11; V2/V3: SCL=8 SDA=7)
     *    After completion panel_handle / tp are both globally valid variables
     */
    lvgl_flush_sync_init();
    LCD_SetFlushCallback(notify_lvgl_flush_ready, &disp_drv);
    LCD_Backlight = 0;  // initialize PWM dark before the panel receives its display-on command
    LCD_Init();

    /* 5. LVGL init */
    lv_init();

    /* Allocate double buffers (DMA memory). Fall back to the smaller band when
       internal DMA RAM is fragmented by board-specific peripherals. */
    /* ESP-NOW starts after LCD allocation. Leave internal RAM for its radio
     * buffers and the on-demand video workers, even when 40-line buffers fit. */
    size_t buf_px = user_cfg.device_role == ESPNOW_ROLE_STANDALONE
        ? LCD_H_RES * 40 : LVGL_BUFF_SIZE;
    lv_color_t *buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        heap_caps_free(buf1); heap_caps_free(buf2);
        buf_px = LVGL_BUFF_SIZE;
        buf1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
        buf2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    }
    assert(buf1 && buf2);
    ESP_LOGI(TAG, "LCD DMA: two %u-line buffers (%u bytes total)",
             (unsigned)(buf_px / LCD_H_RES), (unsigned)(buf_px * sizeof(lv_color_t) * 2));
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_px);

    /* Register display driver */
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.wait_cb = lvgl_flush_wait_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;      // from ST77916.h extern
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    /* LVGL tick timer (2ms period) */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* Register touch input device (polling mode, uses the global tp created by Touch_Init) */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = tp;               // from CST816.h extern
    lv_indev_drv_register(&indev_drv);

    /* 6. Start LVGL task */
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    app_task_start(APP_TASK_LVGL, lvgl_runtime_task, lvgl_mux);

    /* 6.5 Bring up the BLE stack BEFORE the UI to claim internal RAM early. Every
     *     BLE user (ELM327, RaceChrono, gauge pairing) registers through ble_hub. */
    esp_err_t ble_err = ble_hub_init();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE stack init failed (%s)", esp_err_to_name(ble_err));
    }

    /* 7. Start UI - Logo displayed first, then theme loading */
    ESP_LOGI(TAG, "Creating and displaying logo page");
    if (lvgl_lock(-1)) {
        lv_disp_t * dispp = lv_disp_get_default();
        lv_theme_t * theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                                   false, LV_FONT_DEFAULT);
        lv_disp_set_theme(dispp, theme);
        extern void ui_ScreenPageLogo_screen_init(void);
        extern lv_obj_t * ui_ScreenPageLogo;
        ui_ScreenPageLogo_screen_init();
        lv_disp_load_scr(ui_ScreenPageLogo);
        // Submit the logo now and wait for its final DMA transfer, before any
        // SD/theme work. A few scheduler ticks never guaranteed a first paint.
        lv_refr_now(dispp);
        while (disp_buf.flushing) vTaskDelay(1);
        // DMA completion only updates GRAM; allow the panel to scan the Logo
        // while still dark, rather than exposing the previous/default frame.
        vTaskDelay(pdMS_TO_TICKS(50));
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
        uint8_t bright = user_cfg.brightness_day;
        if (bright < 10) bright = 100;
        Set_Backlight(bright);
#endif
        lvgl_unlock();
    }

    ESP_LOGI(TAG, "Starting theme load");
    if (lvgl_lock(-1)) {
        ui_init();
        ui_ext_init();          /* SD-only boot video starts from ui_ext_boot_video_tick */
        lvgl_unlock();
    }
    app_event_init();

    /* 8. Branch by role: master (connects to ELM327 for readings + ESP-NOW broadcast) / slave (only receives and displays the master's data) */
    uint8_t dev_role = user_cfg.device_role;

    if (dev_role == ESPNOW_ROLE_SLAVE) {
        /* ---- Slave: does not connect to ELM327, only starts ESP-NOW receive; UI displays the data broadcast by the master ----
           Screen navigation (not bound to a master → BLE pairing page; bound → normal boot flow) is handled
           centrally by boot_enter_default_page() in ui_ext.c; nothing to do here. */
        ESP_LOGI(TAG, "Device role: SLAVE (ESP-NOW receiver, no BLE/OBD)");
        espnow_link_start_slave();
        {
            uint8_t bm[6];
            espnow_link_get_bound_master_mac(bm);
            ESP_LOGD(TAG, "Bound master MAC at boot: %02x:%02x:%02x:%02x:%02x:%02x (0=unbound)",
                     bm[0], bm[1], bm[2], bm[3], bm[4], bm[5]);
        }
        /* Slave: start BLE GATTS for pairing immediately after ESP-NOW receive setup */
        racechrono_ble_diy_start(user_cfg.rc_enabled);
    } else {
        /* ---- Master / standalone: both run the full BLE OBD chain. ---- */
        bool espnow_on = (dev_role == ESPNOW_ROLE_MASTER);
        ESP_LOGI(TAG, "Device role: %s (BLE/OBD%s)",
                 espnow_on ? "MASTER" : "STANDALONE",
                 espnow_on ? " + ESP-NOW broadcast" : ", standalone");

        /* 8.1 Start BLE OBD - auto-connect only when a MAC is already bound in NVS
               (exact MAC match; legacy name-only configs must reselect the adapter). */
        bool has_obd_mac = (user_cfg.ble_obd_mac[0] | user_cfg.ble_obd_mac[1] |
                            user_cfg.ble_obd_mac[2] | user_cfg.ble_obd_mac[3] |
                            user_cfg.ble_obd_mac[4] | user_cfg.ble_obd_mac[5]) != 0;
        if (has_obd_mac) {
            ESP_LOGD(TAG, "BLE target device: %s (MAC-bound)", user_cfg.ble_device_name);
            elm327_ble_start_default(user_cfg.ble_device_name, user_cfg.ble_obd_mac);
        } else if (user_cfg.ble_device_name[0] != '\0') {
            ESP_LOGD(TAG, "Saved BLE device has no bound MAC; select it again on the scan page");
            elm327_ble_ensure_stack_init();
        } else if (espnow_on) {
            elm327_ble_ensure_stack_init();
        } else {
            ESP_LOGD(TAG, "No saved BLE device, waiting for user selection");
        }

        /* 8.2 External ADS1115 oil pressure (V1 board only). Skipped when the active
               profile reads oil pressure over OBD, so the ADC never overwrites OBD data.
               The slot is marked local: valid without an OBD link, kept on link loss. */
#if HW_HAS_ADS1115_OIL_PRESSURE
        if (!vp_manager_has_signal(SIG_OBD_OIL_PRESSURE)) {
            signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
            oil_pressure_start();
        }
#endif

        /* 8.3 Start ESP-NOW broadcast (sends this unit's OBD data cache to the slave) -- MASTER only. */
        if (espnow_on) {
            espnow_link_start_master();
        }

        /* 8.4 Start RaceChrono BLE GATTS immediately after ESP-NOW (Master/Standalone). */
        racechrono_ble_diy_start(user_cfg.rc_enabled);
    }

    /* SD insert/remove polling + background card jobs (inbox, config mirror, IMPORT scan) */
    if (storage_service_start() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start storage service");
    }
    if (bootmedia_serial_start() != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start USB boot-media receiver");
    }

    if (!app_task_start(APP_TASK_OTA_VALID, mark_app_valid_task, NULL)) {
        ESP_LOGW(TAG, "Failed to create OTA validity task");
    }
}
