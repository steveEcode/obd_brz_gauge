// OTA Mode Page
// Shows the device ready for OTA update, with SoftAP + HTTP server running.
// The companion app connects via WiFi (password: 88888888), reads device info
// from GET /ota/info, then uploads firmware/bootmedia via HTTP POST.
// No BLE involvement.

#include "../ui.h"
#include "../ui_ext.h"
#include "app_obd_dsp/device_identity.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/rs485_brake_temp.h"
#include "esp_log.h"

#ifndef OBD_GAUGE_BUILD_TAG
#define OBD_GAUGE_BUILD_TAG "unknown"
#endif

static const char *TAG = "ota_mode";

// UI elements exposed for timer updates
lv_obj_t *ui_ScreenPageOTAMode = NULL;
lv_obj_t *ui_LabelOTAModeStatus = NULL;
lv_obj_t *ui_LabelOTAModeVersion = NULL;

static void ui_event_ota_mode_background(lv_event_t *e);
static void on_wifi_ota_status(ota_wifi_state_t state, const char *message, uint32_t received, uint32_t expected);

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

    // WiFi AP info
    lv_obj_t *label_name = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_name, "WiFi: OBD-Gauge-OTA-xxxx");
    lv_obj_set_style_text_font(label_name, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_name, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_name, LV_ALIGN_CENTER, 0, 40);

    // Password hint
    lv_obj_t *label_pass = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_pass, "Password: 88888888");
    lv_obj_set_style_text_font(label_pass, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_pass, lv_color_hex(0xFFCC00), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label_pass, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_pass, LV_ALIGN_CENTER, 0, 12);

    // Status label (hidden)
    ui_LabelOTAModeStatus = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(ui_LabelOTAModeStatus, "");
    lv_obj_add_flag(ui_LabelOTAModeStatus, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(ui_LabelOTAModeStatus, &ui_font_FontTypoderSize20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0xAAAAAA), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_LabelOTAModeStatus, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(ui_LabelOTAModeStatus, LV_ALIGN_CENTER, 0, -40);

    // Hint: slide to exit
    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageOTAMode);
    lv_label_set_text(label_hint, "Slide away to exit OTA");
    lv_obj_set_style_text_font(label_hint, &ui_font_FontTypoderSize16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_move_foreground(spinner_ring);
    lv_obj_add_event_cb(ui_ScreenPageOTAMode, ui_event_ota_mode_background, LV_EVENT_ALL, NULL);

    rs485_brake_temp_pause();

    // NOTE: do NOT call espnow_link_stop() here. It deinits the WiFi driver and
    // forces ota_wifi_server_start() through a fresh esp_wifi_init/start while
    // BLE is still up — that path fails to allocate the SoftAP beacon buffer
    // (752B) and panics in ieee80211_hostap_attach. The OTA server is designed
    // to reuse the running stack (APSTA, channel 1) and leaves ESP-NOW untouched.

    // Start WiFi SoftAP + HTTP server immediately (no BLE handshake needed)
    ESP_LOGI(TAG, "Starting WiFi OTA server from OTA mode screen");
    ota_wifi_info_t info = {0};
    if (!ota_wifi_server_start(&info, on_wifi_ota_status)) {
        ESP_LOGE(TAG, "Failed to start WiFi OTA server");
        rs485_brake_temp_resume();
        if (ui_LabelOTAModeStatus) {
            lv_label_set_text(ui_LabelOTAModeStatus, "WiFi start failed");
            lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
}

static void on_wifi_ota_status(ota_wifi_state_t state, const char *message, uint32_t received, uint32_t expected)
{
    (void)received;
    (void)expected;
    if (ui_LabelOTAModeStatus == NULL) return;

    switch (state) {
        case OTA_WIFI_STATE_READY:
            lv_label_set_text(ui_LabelOTAModeStatus, "Ready");
            lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case OTA_WIFI_STATE_RECEIVING:
            if (message) {
                lv_label_set_text(ui_LabelOTAModeStatus, message);
            } else {
                lv_label_set_text(ui_LabelOTAModeStatus, "Receiving update...");
            }
            lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case OTA_WIFI_STATE_DONE:
            lv_label_set_text(ui_LabelOTAModeStatus, "Update complete!");
            lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0x00CC66), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        case OTA_WIFI_STATE_ERROR:
            if (message) {
                lv_label_set_text_fmt(ui_LabelOTAModeStatus, "Error: %s", message);
            } else {
                lv_label_set_text(ui_LabelOTAModeStatus, "Update failed");
            }
            lv_obj_set_style_text_color(ui_LabelOTAModeStatus, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);
            break;
        default:
            break;
    }
}

// Swipe left/right/up to exit OTA mode and stop WiFi
static void ui_event_ota_mode_background(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
        if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_TOP) {
            lv_indev_wait_release(lv_indev_get_act());
            ESP_LOGI(TAG, "Exiting OTA mode, stopping WiFi server");
            ota_wifi_server_stop();
            rs485_brake_temp_resume();
            _ui_screen_change(&ui_ScreenPageEasterEgg, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, &ui_ScreenPageEasterEgg_screen_init);
        }
        // Swipe down also exits
        else if (dir == LV_DIR_BOTTOM) {
            lv_indev_wait_release(lv_indev_get_act());
            ota_wifi_server_stop();
            rs485_brake_temp_resume();
            _ui_screen_change(&ui_ScreenPageSettings, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, &ui_ScreenPageSettings_screen_init);
        }
    }

    ui_ext_tick();
}
