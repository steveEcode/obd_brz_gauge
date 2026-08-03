// BLE Scan & Select Page
// Shows saved device (with delete) + a list of discovered BLE devices
//
// 按 device_role 分两种用途:
//  - MASTER/STANDALONE: 扫描并连接 OBD ELM327 适配器(原有逻辑不变)
//  - SLAVE: 扫描并配对三连表主表("SkyGauge-XXYY" 广播), 见 gauge_pair_ble_client.c

#include "../ui.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/gauge_pair_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
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
static bool s_slave_mode = false;           // true=从表配对主表, false=OBD 设备扫描(原有逻辑)

// 从表模式: 扫描列表按钮 index → 对应设备 MAC 的并行表(与 s_list 子控件顺序一一对应)
static uint8_t s_gauge_macs[GAUGE_PAIR_SCAN_MAX_DEVICES][6];
static int s_gauge_mac_count = 0;

// OBD 设备扫描: 同上, 记录每个列表项的 MAC, 选中后按 MAC 精确连接(不会被同名设备误连)
static uint8_t s_obd_macs[BLE_SCAN_MAX_DEVICES][6];
static int s_obd_mac_count = 0;

// 前向声明
static void start_scan(void);
static void on_device_selected(lv_event_t *e);
static void on_saved_device_delete(lv_event_t *e);
static void on_pair_result(bool ok, const char *name, const uint8_t mac[6]);

// Mutex for LVGL (defined in main)
extern SemaphoreHandle_t lvgl_mux;
static inline bool lvgl_lock_ui(int timeout_ms) {
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void lvgl_unlock_ui(void) {
    xSemaphoreGive(lvgl_mux);
}

// BLE 扫描回调（在 BT 线程中调用，需要线程安全地更新 LVGL）—— OBD 设备扫描(MASTER/STANDALONE)
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
        if (s_obd_mac_count >= BLE_SCAN_MAX_DEVICES) {
            lvgl_unlock_ui();
            return;
        }

        // 添加新设备按钮
        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, dev->name);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &ui_font_FontTypoderSize20, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, on_device_selected, LV_EVENT_CLICKED, NULL);

        memcpy(s_obd_macs[s_obd_mac_count], dev->addr, 6);
        s_obd_mac_count++;

        lv_label_set_text_fmt(s_label_status, "Found %d devices", total_count);
        lvgl_unlock_ui();
    }
}

// BLE 扫描回调 —— 从表配对主表(SLAVE), 只收到 "SkyGauge" 前缀的设备(见 gauge_pair_ble_client.c 过滤)
static void scan_result_cb_gauge(const gauge_pair_scan_result_t *dev, int total_count) {
    if (!s_list) return;

    if (lvgl_lock_ui(100)) {
        uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(s_list, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl && strcmp(lv_label_get_text(lbl), dev->name) == 0) {
                lvgl_unlock_ui();
                return; // 已存在
            }
        }
        if (s_gauge_mac_count >= GAUGE_PAIR_SCAN_MAX_DEVICES) {
            lvgl_unlock_ui();
            return;
        }

        lv_obj_t *btn = lv_list_add_btn(s_list, NULL, dev->name);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, 255, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &ui_font_FontTypoderSize20, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, on_device_selected, LV_EVENT_CLICKED, NULL);

        memcpy(s_gauge_macs[s_gauge_mac_count], dev->addr, 6);
        s_gauge_mac_count++;

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

    if (s_slave_mode) {
        uint32_t idx = lv_obj_get_index(btn);
        if (idx >= (uint32_t)s_gauge_mac_count) return;
        uint8_t mac[6];
        memcpy(mac, s_gauge_macs[idx], 6);

        ESP_LOGI(TAG_BLE_UI, "Selected master: %s", name);
        gauge_pair_ble_scan_stop();
        s_scanning = false;

        lv_label_set_text(s_label_status, "Pairing...");
        if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);
        gauge_pair_ble_connect(mac, name, on_pair_result);
        return;
    }

    ESP_LOGI(TAG_BLE_UI, "Selected BLE device: %s", name);

    uint32_t idx = lv_obj_get_index(btn);
    if (idx >= (uint32_t)s_obd_mac_count) return;
    uint8_t mac[6];
    memcpy(mac, s_obd_macs[idx], 6);

    elm327_ble_scan_only_stop();
    s_scanning = false;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    strncpy(cfg.ble_device_name, name, sizeof(cfg.ble_device_name) - 1);
    cfg.ble_device_name[sizeof(cfg.ble_device_name) - 1] = '\0';
    memcpy(cfg.ble_obd_mac, mac, 6);
    nvs_cfg_set(&cfg);

    // 立即刷新已保存设备面板
    if (s_saved_name_lbl) lv_label_set_text(s_saved_name_lbl, name);
    if (s_saved_panel)    lv_obj_clear_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_clear_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(s_label_status, "Connecting: %s", name);
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    elm327_ble_connect_by_addr(mac, name);
    _ui_screen_change(&ui_ScreenPageTemp, LV_SCR_LOAD_ANIM_FADE_ON, 300, 500, &ui_ScreenPageTemp_screen_init);
}

// 蓝牙配对结果回调(BT 任务上下文调用, 需要 lvgl_lock)
static void on_pair_result(bool ok, const char *name, const uint8_t mac[6]) {
    if (!lvgl_lock_ui(200)) return;

    if (ok) {
        espnow_link_bind_master(mac);

        nvs_user_cfg_t cfg = *nvs_cfg_get();
        strncpy(cfg.ble_device_name, name ? name : "", sizeof(cfg.ble_device_name) - 1);
        cfg.ble_device_name[sizeof(cfg.ble_device_name) - 1] = '\0';
        nvs_cfg_set(&cfg);
        ESP_LOGI(TAG_BLE_UI, "Paired with master: %s", cfg.ble_device_name);

        if (s_saved_name_lbl) lv_label_set_text(s_saved_name_lbl, cfg.ble_device_name);
        if (s_saved_panel)    lv_obj_clear_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
        if (s_label_saved_hdr) lv_obj_clear_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(s_label_status, "Paired!");
        _ui_screen_change(&ui_ScreenPageTemp, LV_SCR_LOAD_ANIM_FADE_ON, 300, 500, &ui_ScreenPageTemp_screen_init);
    } else {
        ESP_LOGW(TAG_BLE_UI, "Pairing failed, rescanning");
        lv_label_set_text(s_label_status, "Pair failed, retrying...");
        // 原生 BLE 扫描窗口(15s)到期后并不会回调通知这里, s_scanning 会一直停留在 true,
        // 这里是明确要强制重新扫描的地方, 先复位再调用, 避免被 start_scan() 的去重判断挡住。
        s_scanning = false;
        start_scan();
    }

    lvgl_unlock_ui();
}

// 删除已保存设备
static void on_saved_device_delete(lv_event_t *e) {
    if (s_slave_mode) {
        espnow_link_unbind_master();
        nvs_user_cfg_t cfg = *nvs_cfg_get();
        cfg.ble_device_name[0] = '\0';
        nvs_cfg_set(&cfg);
        ESP_LOGI(TAG_BLE_UI, "Unbound saved master");
    } else {
        // 若当前已连接，先断开BLE
        if (elm327_ble_is_connected()) {
            elm327_ble_disconnect();
        }
        nvs_user_cfg_t cfg = *nvs_cfg_get();
        cfg.ble_device_name[0] = '\0';
        memset(cfg.ble_obd_mac, 0, sizeof(cfg.ble_obd_mac));
        nvs_cfg_set(&cfg);
        ESP_LOGI(TAG_BLE_UI, "Saved BLE device cleared");
    }

    if (s_saved_panel)    lv_obj_add_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);
    if (s_label_status)   lv_label_set_text(s_label_status, "Saved device removed");

    if (s_slave_mode) {
        s_scanning = false;   // 原生扫描窗口到期不会回调复位, 强制重扫前先复位(理由同 on_pair_result)
        start_scan();         // 从表: 删除绑定后立即重新扫描, 方便配对新主表
    }
}

static void start_scan(void) {
    if (s_scanning) return;
    s_scanning = true;

    if (s_list) lv_obj_clean(s_list);
    if (s_label_status) lv_label_set_text(s_label_status, "Scanning...");
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    if (s_slave_mode) {
        s_gauge_mac_count = 0;
        gauge_pair_ble_scan_start(15, scan_result_cb_gauge);
    } else {
        s_obd_mac_count = 0;
        elm327_ble_scan_only_start(15, scan_result_cb);
    }
}

void ui_ScreenPageBLEScan_screen_init(void)
{
    s_slave_mode = (nvs_cfg_get()->device_role == ESPNOW_ROLE_SLAVE);

    ui_ScreenPageBLEScan = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageBLEScan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageBLEScan, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageBLEScan, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageBLEScan, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageBLEScan, 0, LV_PART_MAIN);

    // White border ring
    lv_obj_t *spinner_ring = ui_helpers_create_ring(ui_ScreenPageBLEScan, 10);

    // Title
    lv_obj_t *label_title = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_title, s_slave_mode ? "FIND MASTER" : "BLE SCAN");
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
    bool has_saved;
    if (s_slave_mode) {
        const uint8_t *bound_mac = espnow_link_get_bound_master_mac();
        has_saved = (bound_mac[0] | bound_mac[1] | bound_mac[2] | bound_mac[3] | bound_mac[4] | bound_mac[5]) != 0;
    } else {
        has_saved = (saved_cfg->ble_device_name[0] != '\0');
    }

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
    lv_obj_move_foreground(spinner_ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageBLEScan, ui_event_ble_scan_background, LV_EVENT_GESTURE, NULL);

    // Start scanning
    start_scan();
}

