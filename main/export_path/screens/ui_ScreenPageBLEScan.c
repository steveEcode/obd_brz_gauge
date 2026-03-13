// BLE Scan & Select Page
// Shows saved device (with delete) + a list of discovered BLE devices

#include "../ui.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG_BLE_UI = "ble_scan_ui";

// UI elements (local)
static lv_obj_t *s_list = NULL;             // 扫描设备列表
static lv_obj_t *s_label_status = NULL;     // 状态标签
static lv_obj_t *s_spinner = NULL;          // 扫描 spinner
static lv_obj_t *s_saved_panel = NULL;      // 已保存设备面板
static lv_obj_t *s_label_saved_hdr = NULL;  // "SAVED" 小标题
static lv_obj_t *s_saved_name_lbl = NULL;   // 已保存设备名称标签
static bool s_scanning = false;

// 前向声明
static void start_scan(void);
static void on_device_selected(lv_event_t *e);
static void on_saved_device_delete(lv_event_t *e);

// Mutex for LVGL (defined in main)
extern SemaphoreHandle_t lvgl_mux;
static inline bool lvgl_lock_ui(int timeout_ms) {
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void lvgl_unlock_ui(void) {
    xSemaphoreGive(lvgl_mux);
}

// BLE 扫描回调（在 BT 线程中调用，需要线程安全地更新 LVGL）
static void scan_result_cb(const ble_scan_result_t *dev, int total_count) {
    if (!s_list) return;

    if (lvgl_lock_ui(100)) {
        // 检查列表中是否已有同名设备
        uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(s_list, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl && strcmp(lv_label_get_text(lbl), dev->name) == 0) {
                lvgl_unlock_ui();
                return; // 已存在
            }
        }

        // 添加新设备按钮
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, dev->name);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &ui_font_FontTypoderSize20, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, on_device_selected, LV_EVENT_CLICKED, NULL);

        lv_label_set_text_fmt(s_label_status, "Found %d devices", total_count);
        lvgl_unlock_ui();
    }
}

// 设备被点击选中
static void on_device_selected(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (!lbl) return;

    const char *name = lv_label_get_text(lbl);
    ESP_LOGI(TAG_BLE_UI, "Selected BLE device: %s", name);

    elm327_ble_scan_only_stop();
    s_scanning = false;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    strncpy(cfg.ble_device_name, name, sizeof(cfg.ble_device_name) - 1);
    cfg.ble_device_name[sizeof(cfg.ble_device_name) - 1] = '\0';
    nvs_cfg_set(&cfg);

    // 立即刷新已保存设备面板
    if (s_saved_name_lbl) lv_label_set_text(s_saved_name_lbl, name);
    if (s_saved_panel)    lv_obj_clear_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_clear_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(s_label_status, "Connecting: %s", name);
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    elm327_ble_connect_by_name(name);
    _ui_screen_change(&ui_ScreenPageMain, LV_SCR_LOAD_ANIM_FADE_ON, 300, 500, &ui_ScreenPageMain_screen_init);
}

// 删除已保存设备
static void on_saved_device_delete(lv_event_t *e) {
    // 若当前已连接，先断开BLE
    if (elm327_ble_is_connected()) {
        elm327_ble_disconnect();
    }
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.ble_device_name[0] = '\0';
    nvs_cfg_set(&cfg);
    ESP_LOGI(TAG_BLE_UI, "Saved BLE device cleared");

    if (s_saved_panel)    lv_obj_add_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);
    if (s_label_status)   lv_label_set_text(s_label_status, "Saved device removed");
}

static void start_scan(void) {
    if (s_scanning) return;
    s_scanning = true;

    if (s_list) lv_obj_clean(s_list);
    if (s_label_status) lv_label_set_text(s_label_status, "Scanning...");
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    elm327_ble_scan_only_start(15, scan_result_cb);
}

void ui_ScreenPageBLEScan_screen_init(void)
{
    ui_ScreenPageBLEScan = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageBLEScan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageBLEScan, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageBLEScan, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageBLEScan, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageBLEScan, 0, LV_PART_MAIN);

    // White border ring
    lv_obj_t *spinner_ring = lv_spinner_create(ui_ScreenPageBLEScan, 1000, 90);
    lv_obj_set_size(spinner_ring, 360, 360);
    lv_obj_set_align(spinner_ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(spinner_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(spinner_ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spinner_ring, 255, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(spinner_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner_ring, 10, LV_PART_INDICATOR);

    // Title
    lv_obj_t *label_title = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_title, "BLE SCAN");
    lv_obj_set_style_text_font(label_title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 18);

    // Scanning spinner (animated)
    s_spinner = lv_spinner_create(ui_ScreenPageBLEScan, 1000, 60);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_align(s_spinner, LV_ALIGN_TOP_MID, 72, 20);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);

    // Status label
    s_label_status = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(s_label_status, "Scanning...");
    lv_obj_set_style_text_font(s_label_status, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_status, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, 50);

    // ==== SAVED DEVICE SECTION ====
    const nvs_user_cfg_t *saved_cfg = nvs_cfg_get();
    bool has_saved = (saved_cfg->ble_device_name[0] != '\0');

    s_label_saved_hdr = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(s_label_saved_hdr, "SAVED DEVICE");
    lv_obj_set_style_text_font(s_label_saved_hdr, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_saved_hdr, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_label_saved_hdr, LV_ALIGN_TOP_MID, 0, 72);
    if (!has_saved) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    // Saved device row: name + delete button
    s_saved_panel = lv_obj_create(ui_ScreenPageBLEScan);
    lv_obj_remove_style_all(s_saved_panel);
    lv_obj_set_size(s_saved_panel, 264, 32);
    lv_obj_align(s_saved_panel, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(s_saved_panel, lv_color_hex(0x222222), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_saved_panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_saved_panel, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_saved_panel, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_saved_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (!has_saved) lv_obj_add_flag(s_saved_panel, LV_OBJ_FLAG_HIDDEN);

    // Device name inside panel
    s_saved_name_lbl = lv_label_create(s_saved_panel);
    lv_label_set_text(s_saved_name_lbl, has_saved ? saved_cfg->ble_device_name : "");
    lv_obj_set_style_text_font(s_saved_name_lbl, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_saved_name_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_saved_name_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    // Delete button inside panel
    lv_obj_t *del_btn = lv_btn_create(s_saved_panel);
    lv_obj_set_size(del_btn, 30, 24);
    lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xBB2222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(del_btn, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(del_btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(del_btn, 2, LV_PART_MAIN);
    lv_obj_t *del_lbl = lv_label_create(del_btn);
    lv_label_set_text(del_lbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(del_lbl);
    lv_obj_add_event_cb(del_btn, on_saved_device_delete, LV_EVENT_CLICKED, NULL);

    // Thin divider
    lv_obj_t *divider = lv_obj_create(ui_ScreenPageBLEScan);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 240, 1);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, 255, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ==== NEARBY SCAN SECTION ====
    lv_obj_t *label_nearby = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_nearby, "NEARBY");
    lv_obj_set_style_text_font(label_nearby, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_nearby, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_nearby, LV_ALIGN_TOP_MID, 0, 134);

    // Device list (scan results)
    s_list = lv_list_create(ui_ScreenPageBLEScan);
    lv_obj_set_size(s_list, 264, 145);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 8, LV_PART_MAIN);

    // Hint text at bottom
    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_hint, "Tap to connect  Slide to back");
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(label_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -15);

    // Gesture event for navigation
    lv_obj_add_event_cb(ui_ScreenPageBLEScan, ui_event_ble_scan_background, LV_EVENT_GESTURE, NULL);

    // Start scanning
    start_scan();
}

