// OTA Mode Page
// Shows the device ready for OTA update, with SoftAP + HTTP server running.
// The companion app connects via WiFi (password: 88888888), reads device info
// from GET /ota/info, then uploads firmware/bootmedia via HTTP POST.
// No BLE involvement.

#include "../ui.h"
#include "../ui_ext.h"
#include "app_obd_dsp/device_identity.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>

#ifndef OBD_GAUGE_BUILD_TAG
#define OBD_GAUGE_BUILD_TAG "unknown"
#endif

static const char *TAG = "ota_mode";

// UI elements exposed for timer updates
lv_obj_t *ui_ScreenPageOTAMode = NULL;
lv_obj_t *ui_LabelOTAModeVersion = NULL;

// Internal reference to WiFi SSID label for dynamic updates
static lv_obj_t *s_label_wifi_ssid = NULL;

static lv_timer_t *s_start_timer;
static void start_ota_when_ready(lv_timer_t *timer);
static void ui_event_ota_mode_background(lv_event_t *e);
static lv_obj_t *s_format_button, *s_format_status, *s_format_dialog, *s_format_close;
static lv_timer_t *s_format_timer;
static bool s_format_busy;
static portMUX_TYPE s_format_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_format_finished, s_format_reboot;
static esp_err_t s_format_result;
static char s_format_detail[480];

/* Worker never calls LVGL. Reboot is worker-owned even if the screen is deleted. */
static void format_sd_worker(void *arg)
{
    (void)arg;
    char detail[sizeof(s_format_detail)];
    bool reboot = false;
    esp_err_t result = ota_wifi_server_format_sd(detail, sizeof(detail), &reboot);
    portENTER_CRITICAL(&s_format_mux);
    snprintf(s_format_detail, sizeof(s_format_detail), "%s", detail);
    s_format_result = result;
    s_format_reboot = reboot;
    s_format_finished = true;
    portEXIT_CRITICAL(&s_format_mux);
    if (reboot) {
        ESP_LOGI(TAG, "SD format result: %s %s", esp_err_to_name(result), detail);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    vTaskDelete(NULL);
}

static void format_sd_progress(lv_timer_t *timer)
{
    char detail[sizeof(s_format_detail)];
    portENTER_CRITICAL(&s_format_mux);
    bool finished = s_format_finished;
    bool reboot = s_format_reboot;
    esp_err_t result = s_format_result;
    memcpy(detail, s_format_detail, sizeof(detail));
    portEXIT_CRITICAL(&s_format_mux);
    if (!finished) return;
    if (s_format_status) {
        char text[180];
        if (result == ESP_OK) snprintf(text, sizeof(text), "SD formatted successfully\nRebooting in 5 seconds...");
        else snprintf(text, sizeof(text), "%s\n%.110s", reboot ? "Failed; rebooting in 5s" : "Not formatted", detail[0] ? detail : esp_err_to_name(result));
        lv_label_set_text(s_format_status, text);
    }
    if (!reboot) {
        s_format_busy = false;
        if (s_format_button) lv_obj_clear_state(s_format_button, LV_STATE_DISABLED);
        if (s_format_close) lv_obj_clear_flag(s_format_close, LV_OBJ_FLAG_HIDDEN);
    }
    s_format_timer = NULL;
    lv_timer_del(timer);
}

static void confirm_format_sd(lv_event_t *event)
{
    bool erase = lv_event_get_user_data(event) != NULL;
    if (s_format_busy) return;
    if (!erase) {
        lv_obj_del(s_format_dialog);
        s_format_dialog = s_format_status = s_format_close = NULL;
        return;
    }
    /* Keep progress/results on a modal panel: the native screen is 360x360,
     * and a status label below the main button would fall outside the circle. */
    lv_obj_clean(s_format_dialog);
    lv_obj_t *box = lv_obj_create(s_format_dialog);
    lv_obj_set_size(box, 300, 240);
    lv_obj_center(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_format_status = lv_label_create(box);
    lv_obj_set_width(s_format_status, 260);
    lv_label_set_long_mode(s_format_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_format_status, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_format_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_format_status, LV_ALIGN_CENTER, 0, -20);
    s_format_close = lv_btn_create(box);
    lv_obj_set_size(s_format_close, 96, 36);
    lv_obj_align(s_format_close, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *close_text = lv_label_create(s_format_close);
    lv_label_set_text(close_text, "OK");
    lv_obj_set_style_text_font(close_text, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_center(close_text);
    lv_obj_add_event_cb(s_format_close, confirm_format_sd, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_format_close, LV_OBJ_FLAG_HIDDEN);
    s_format_busy = true;
    s_format_finished = false;
    s_format_reboot = false;
    s_format_timer = lv_timer_create(format_sd_progress, 100, NULL);
    if (!s_format_timer) {
        s_format_busy = false;
        lv_label_set_text(s_format_status, "Cannot start format timer");
        lv_obj_clear_flag(s_format_close, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_state(s_format_button, LV_STATE_DISABLED);
    lv_label_set_text(s_format_status, "Preparing / formatting SD...\nDo NOT disconnect power");
    /* xTaskCreate uses internal RAM: SD/flash operations must not run on a
     * PSRAM stack. No blocking IO or HTTP shutdown while holding lvgl_mux. */
    if (xTaskCreate(format_sd_worker, "sd_format", 8192, NULL, 2, NULL) != pdPASS) {
        lv_timer_del(s_format_timer);
        s_format_timer = NULL;
        s_format_busy = false;
        lv_obj_clear_state(s_format_button, LV_STATE_DISABLED);
        lv_label_set_text(s_format_status, "Cannot start format worker");
        lv_obj_clear_flag(s_format_close, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_format_sd(lv_event_t *event)
{
    (void)event;
    if (s_format_busy || s_format_dialog) return;
    /* Use the existing enabled LVGL widgets; LV_USE_MSGBOX is disabled. */
    s_format_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_format_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_format_dialog);
    lv_obj_clear_flag(s_format_dialog, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(s_format_dialog, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_format_dialog, LV_OPA_90, LV_PART_MAIN);
    lv_obj_t *box = lv_obj_create(s_format_dialog);
    lv_obj_set_size(box, 300, 280);
    lv_obj_center(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *message = lv_label_create(box);
    lv_obj_set_width(message, 264);
    lv_obj_set_style_text_font(message, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_label_set_text(message, "ERASE SD CARD?\n\nDeletes ALL SD files,\nthemes and animations.\n"
        "FAT32; 32 KiB on large cards.\nFirmware / internal settings stay.\n\nKeep power connected.");
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 0);
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *button = lv_btn_create(box);
        lv_obj_set_size(button, 116, 40);
        lv_obj_align(button, i ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(i ? 0xA63A28 : 0x333333), LV_PART_MAIN);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, i ? "ERASE" : "Cancel");
        lv_obj_set_style_text_font(label, &ui_font_FontTypoderSize16, LV_PART_MAIN);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, confirm_format_sd, LV_EVENT_CLICKED, i ? (void *)1 : NULL);
    }
}


void ui_ScreenPageOTAMode_screen_init(void)
{
    ui_ScreenPageOTAMode = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageOTAMode, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageOTAMode, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_helpers_style_screen_bg(ui_ScreenPageOTAMode);
    lv_obj_set_style_bg_opa(ui_ScreenPageOTAMode, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // White border ring
    lv_obj_t *spinner_ring = ui_helpers_create_ring(ui_ScreenPageOTAMode, 10);

    // Title: "OTA MODE"
    lv_obj_t *label_title = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_title, "OTA MODE");
    lv_obj_set_style_text_font(label_title, &ui_font_FontTypoderSize20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 40);

    // Pulse icon (animated) to show the device is discoverable
    lv_obj_t *pulse = lv_spinner_create(ui_ScreenPageOTAMode, 1000, 90);
    lv_obj_set_size(pulse, 32, 32);
    lv_obj_align(pulse, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_arc_color(pulse, lv_color_hex(0x00CC66), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(pulse, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pulse, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(pulse, 3, LV_PART_MAIN);

    // Version label (short git hash)
    ui_LabelOTAModeVersion = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(ui_LabelOTAModeVersion, OBD_GAUGE_BUILD_TAG);
    lv_obj_set_style_text_font(ui_LabelOTAModeVersion, &ui_font_FontTypoderSize20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelOTAModeVersion, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_LabelOTAModeVersion, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(ui_LabelOTAModeVersion, LV_ALIGN_TOP_MID, 0, 120);

    // WiFi AP info (will be updated with real SSID when OTA server starts)
    s_label_wifi_ssid = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(s_label_wifi_ssid, "WiFi: Starting...");
    lv_obj_set_style_text_font(s_label_wifi_ssid, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_label_wifi_ssid, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_label_wifi_ssid, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_label_wifi_ssid, LV_ALIGN_CENTER, 0, 40);

    // Password hint
    lv_obj_t *label_pass = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_pass, "Password: 88888888");
    lv_obj_set_style_text_font(label_pass, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_pass, lv_color_hex(0xFFCC00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label_pass, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_pass, LV_ALIGN_CENTER, 0, 12);

    s_format_button = lv_btn_create(ui_ScreenPageOTAMode);
    lv_obj_set_size(s_format_button, 196, 42);
    lv_obj_align(s_format_button, LV_ALIGN_CENTER, 0, 94);
    lv_obj_set_style_bg_color(s_format_button, lv_color_hex(0xA63A28), LV_PART_MAIN);
    lv_obj_t *format_text = lv_label_create(s_format_button);
    lv_label_set_text(format_text, "Format SD card");
    lv_obj_set_style_text_font(format_text, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_center(format_text);
    lv_obj_add_event_cb(s_format_button, show_format_sd, LV_EVENT_CLICKED, NULL);
    // Hint: slide to exit
    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_hint, "Slide away to exit OTA");
    lv_obj_set_style_text_font(label_hint, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_move_foreground(spinner_ring);
    lv_obj_add_event_cb(ui_ScreenPageOTAMode, ui_event_ota_mode_background, LV_EVENT_ALL, NULL);

    // Drop the ELM327 BLE link for the duration of the OTA: the GATT client keeps
    // re-scanning/reconnecting in the background and steals the single 2.4GHz
    // radio from the SoftAP, stalling the upload mid-transfer.
    elm327_ble_pause_for_ota();

    start_ota_when_ready(NULL);
}

static void start_ota_when_ready(lv_timer_t *timer)
{
    (void)timer;
    // ESP-NOW workers finish their current operations and exit themselves.
    // Retry on LVGL ticks rather than waiting while holding the display lock.
    if (!espnow_link_pause_for_ota()) {
        if (!s_start_timer) {
            s_start_timer = lv_timer_create(start_ota_when_ready, 100, NULL);
            if (!s_start_timer) {
                ESP_LOGE(TAG, "Failed to schedule OTA startup retry");
                if (s_label_wifi_ssid) {
                    lv_label_set_text(s_label_wifi_ssid, "WiFi: Failed to start");
                    lv_obj_set_style_text_color(s_label_wifi_ssid, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
                }
            }
        }
        return;
    }
    if (s_start_timer) {
        lv_timer_del(s_start_timer);
        s_start_timer = NULL;
    }

    // ota_wifi_server_start() releases BLE (ble_hub_shutdown), video buffers and
    // theme_0 image caches. Theme page objects must be gone before the caches are
    // freed; OTA is entered from the version page, so the theme page is not on screen.
    if (ui_ScreenPageThemeGauge && ui_ScreenPageThemeGauge != lv_scr_act()) {
        lv_obj_del(ui_ScreenPageThemeGauge);
        ui_ScreenPageThemeGauge = NULL;
    }

    // Start WiFi SoftAP + HTTP server immediately
    ESP_LOGI(TAG, "Starting WiFi OTA server from OTA mode screen");
    ota_wifi_info_t info = {0};
    if (!ota_wifi_server_start(&info)) {
        ESP_LOGE(TAG, "Failed to start WiFi OTA server");
        if (s_label_wifi_ssid) {
            lv_label_set_text(s_label_wifi_ssid, "WiFi: Failed to start");
            lv_obj_set_style_text_color(s_label_wifi_ssid, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    } else {
        // Update SSID label with real WiFi AP name (REFACTOR_PLAN.md P3 #235)
        if (s_label_wifi_ssid) {
            char ssid_text[64];
            snprintf(ssid_text, sizeof(ssid_text), "WiFi: %s", info.ssid);
            lv_label_set_text(s_label_wifi_ssid, ssid_text);
        }
        ESP_LOGI(TAG, "WiFi OTA server started: SSID=%s, IP=%s:%d", info.ssid, info.ip, info.port);
    }
}


// Leaving OTA mode: entering it released the BT controller and quiesced
// ESP-NOW. That is irreversible for this session, so BLE — and with it the OBD
// link — can only come back through a reset.
// Uploads already end in esp_restart(); this covers the "looked and swiped away"
// case so the gauge is never left BLE-dead.
static void exit_ota_mode(void)
{
    if (s_format_busy || s_format_dialog) return;
    ESP_LOGI(TAG, "Exiting OTA mode, stopping WiFi server");
    if (!ota_wifi_server_stop()) {
        ESP_LOGW(TAG, "WiFi OTA start/stop busy; retry exit after it finishes");
        return;
    }

    if (s_start_timer) {
        lv_timer_del(s_start_timer);
        s_start_timer = NULL;
    }
    ESP_LOGI(TAG, "Rebooting to restore BLE (BT controller was released for the OTA radio)");
    // Give the WiFi teardown and the log line a moment to drain.
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

// Any swipe exits OTA mode.  Both former destinations (EasterEgg / Settings)
// are replaced by a reboot, which lands on the normal boot flow.
static void ui_event_ota_mode_background(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_DELETE) {
        if (s_format_dialog) { lv_obj_del(s_format_dialog); s_format_dialog = NULL; }
        if (s_format_timer) { lv_timer_del(s_format_timer); s_format_timer = NULL; }
        s_format_button = s_format_status = s_format_close = s_label_wifi_ssid = NULL;
    }
    if (event_code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT ||
            dir == LV_DIR_TOP  || dir == LV_DIR_BOTTOM) {
            lv_indev_wait_release(lv_indev_get_act());
            exit_ota_mode();
        }
    }

}
