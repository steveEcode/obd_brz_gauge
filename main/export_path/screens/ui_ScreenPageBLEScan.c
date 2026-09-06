// BLE Scan & Select Page
// Shows saved device (with delete) + a list of discovered BLE devices
//
// Two use cases depending on device_role:
//  - MASTER/STANDALONE: scan and connect to an OBD ELM327 adapter (original logic unchanged)
//  - SLAVE: scan and pair with the triple-gauge master ("SkyGauge-XXYY" broadcast), see gauge_pair_ble_client.c

#include "../ui.h"
#include "../ui_ext.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/gauge_pair_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/wired_can_obd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG_BLE_UI = "ble_scan_ui";

// UI elements (local)
static lv_obj_t *s_list = NULL;             // scanned device list
static lv_obj_t *s_label_status = NULL;     // status label
static lv_obj_t *s_spinner = NULL;          // scan spinner
static lv_obj_t *s_saved_panel = NULL;      // saved device panel
static lv_obj_t *s_label_saved_hdr = NULL;  // "SAVED" sub-header
static lv_obj_t *s_saved_name_lbl = NULL;   // saved device name label
static bool s_scanning = false;
static bool s_slave_mode = false;           // true=slave pairing with a master, false=OBD device scan (original logic)

// Slave mode: parallel table mapping scan list button index -> corresponding device MAC (one-to-one with the s_list child order)
static uint8_t s_gauge_macs[GAUGE_PAIR_SCAN_MAX_DEVICES][6];
static int s_gauge_mac_count = 0;

// OBD device scan: same as above, records the MAC of each list item; after selection connects by exact MAC (avoids misconnecting to a same-name device)
static uint8_t s_obd_macs[BLE_SCAN_MAX_DEVICES][6];
static int s_obd_mac_count = 0;

// Forward declarations
static void start_scan(void);
static void on_device_selected(lv_event_t *e);
static void on_saved_device_delete(lv_event_t *e);
static void on_pair_result(bool ok, const char *name, const uint8_t mac[6]);

static void on_source_selected(lv_event_t *e) {
    uint8_t source = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    if (cfg.obd_source == source) return;

    if (s_scanning) elm327_ble_scan_only_stop();
    s_scanning = false;
    if (source == OBD_SOURCE_WIRED_CAN) {
        elm327_ble_pause_for_ota();
    } else {
        wired_can_obd_stop();
        ui_ext_sweep_suppress_next_connection();
        elm327_ble_resume_for_source_selection();
    }
    cfg.obd_source = source;
    nvs_cfg_set(&cfg);
    if (source == OBD_SOURCE_WIRED_CAN) wired_can_obd_start();

    /* Rebuild this one page so the selected tab/content changes immediately;
       the rest of the UI and the gauge data stay alive. */
    lv_obj_t *old_screen = ui_ScreenPageBLEScan;
    ui_ScreenPageBLEScan = NULL;
    ui_ScreenPageBLEScan_screen_init();
    lv_scr_load_anim(ui_ScreenPageBLEScan, LV_SCR_LOAD_ANIM_FADE_ON, 180, 0, false);
    if (old_screen) lv_obj_del_delayed(old_screen, 220);
}

static lv_obj_t *create_source_button(lv_obj_t *parent, const char *text,
                                      uint8_t source, bool selected, int x) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 106, 36);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x, 72);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, ui_theme_color_lv(UI_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, selected ? 255 : 150, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, selected ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, selected ? ui_theme_color_lv(UI_COLOR_ARC_INDICATOR)
                                                     : lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_source_selected, LV_EVENT_CLICKED, (void *)(uintptr_t)source);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, selected ? ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY)
                                                : ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

// Mutex for LVGL (defined in main)
extern SemaphoreHandle_t lvgl_mux;
static inline bool lvgl_lock_ui(int timeout_ms) {
    return xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
static inline void lvgl_unlock_ui(void) {
    xSemaphoreGive(lvgl_mux);
}

// BLE scan callback (called in the BT thread, LVGL must be updated thread-safely) -- OBD device scan (MASTER/STANDALONE)
static void scan_result_cb(const ble_scan_result_t *dev, int total_count) {
    if (!s_list) return;

    if (lvgl_lock_ui(100)) {
        // Check whether a device with the same name is already in the list
        uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(s_list, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl && strcmp(lv_label_get_text(lbl), dev->name) == 0) {
                lvgl_unlock_ui();
                return; // already exists
            }
        }
        if (s_obd_mac_count >= BLE_SCAN_MAX_DEVICES) {
            lvgl_unlock_ui();
            return;
        }

        // Add new device button
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

// BLE scan callback -- slave pairing with a master (SLAVE); only devices with the "SkyGauge" prefix are received (see the filter in gauge_pair_ble_client.c)
static void scan_result_cb_gauge(const gauge_pair_scan_result_t *dev, int total_count) {
    if (!s_list) return;

    if (lvgl_lock_ui(100)) {
        uint32_t child_cnt = lv_obj_get_child_cnt(s_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(s_list, i);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl && strcmp(lv_label_get_text(lbl), dev->name) == 0) {
                lvgl_unlock_ui();
                return; // already exists
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

// A device was tapped/selected
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

    // Refresh the saved device panel immediately
    if (s_saved_name_lbl) lv_label_set_text(s_saved_name_lbl, name);
    if (s_saved_panel)    lv_obj_clear_flag(s_saved_panel,    LV_OBJ_FLAG_HIDDEN);
    if (s_label_saved_hdr) lv_obj_clear_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(s_label_status, "Connecting: %s", name);
    if (s_spinner) lv_obj_clear_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

    ui_ext_sweep_suppress_next_connection();
    elm327_ble_connect_by_addr(mac, name);
    _ui_screen_change(&ui_ScreenPageTemp, LV_SCR_LOAD_ANIM_FADE_ON, 300, 500, &ui_ScreenPageTemp_screen_init);
}

// BLE pairing result callback (called in the BT task context, lvgl_lock required)
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
        // When the native BLE scan window (15s) expires, no callback notifies here, so s_scanning stays true.
        // This is a place where a forced rescan is intended, so reset it before calling to avoid being blocked by the dedup check in start_scan().
        s_scanning = false;
        start_scan();
    }

    lvgl_unlock_ui();
}

// Delete the saved device
static void on_saved_device_delete(lv_event_t *e) {
    if (s_slave_mode) {
        espnow_link_unbind_master();
        nvs_user_cfg_t cfg = *nvs_cfg_get();
        cfg.ble_device_name[0] = '\0';
        nvs_cfg_set(&cfg);
        ESP_LOGI(TAG_BLE_UI, "Unbound saved master");
    } else {
        // If currently connected, disconnect BLE first
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
        s_scanning = false;   // the native scan window expiry does not reset via callback; reset before forcing a rescan (same reason as on_pair_result)
        start_scan();         // Slave: rescan immediately after deleting the binding, so a new master can be paired
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
    const nvs_user_cfg_t *saved_cfg = nvs_cfg_get();
    s_slave_mode = (saved_cfg->device_role == ESPNOW_ROLE_SLAVE);
    bool bluetooth_selected = (saved_cfg->obd_source == OBD_SOURCE_BLUETOOTH);
    s_scanning = false;
    s_list = NULL;

    ui_ScreenPageBLEScan = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageBLEScan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageBLEScan, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    ui_helpers_style_screen_bg(ui_ScreenPageBLEScan);
    lv_obj_set_style_bg_opa(ui_ScreenPageBLEScan, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageBLEScan, 0, LV_PART_MAIN);

    // White border ring
    lv_obj_t *spinner_ring = ui_helpers_create_ring(ui_ScreenPageBLEScan, 10);

    // Title
    lv_obj_t *label_title = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_title, s_slave_mode ? "FIND MASTER" : "OBD SOURCE");
    lv_obj_set_style_text_font(label_title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, s_slave_mode ? 30 : 34);

    if (!s_slave_mode) {
        create_source_button(ui_ScreenPageBLEScan, "CAN", OBD_SOURCE_WIRED_CAN,
                             !bluetooth_selected, -57);
        create_source_button(ui_ScreenPageBLEScan, "BLE", OBD_SOURCE_BLUETOOTH,
                             bluetooth_selected, 57);
    }

    // Scanning spinner (animated)
    s_spinner = lv_spinner_create(ui_ScreenPageBLEScan, 1000, 60);
    lv_obj_set_size(s_spinner, 24, 24);
    lv_obj_align(s_spinner, LV_ALIGN_TOP_MID, 108, s_slave_mode ? 20 : 110);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spinner, 3, LV_PART_MAIN);

    // Status label
    s_label_status = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(s_label_status, s_slave_mode ? "Scanning..."
                         : bluetooth_selected ? (elm327_ble_is_connected() ? "BLUETOOTH CONNECTED" : "BLUETOOTH DISCONNECTED")
                         : (wired_can_obd_has_fresh_data() ? "CAN CONNECTED" : "WAITING FOR CAN"));
    lv_obj_set_style_text_font(s_label_status, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_status,
        (!s_slave_mode && bluetooth_selected && !elm327_ble_is_connected()) ? lv_color_hex(0xD89B42)
        : (!s_slave_mode && !bluetooth_selected && wired_can_obd_has_fresh_data()) ? lv_color_hex(0x25D89A)
        : lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, s_slave_mode ? 50 : 116);

    if (!s_slave_mode && !bluetooth_selected) {
        lv_obj_add_flag(s_spinner, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *can_panel = lv_obj_create(ui_ScreenPageBLEScan);
        lv_obj_set_size(can_panel, 230, 120);
        lv_obj_align(can_panel, LV_ALIGN_CENTER, 0, 24);
        lv_obj_set_style_radius(can_panel, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(can_panel, ui_theme_color_lv(UI_COLOR_PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(can_panel, 210, LV_PART_MAIN);
        lv_obj_set_style_border_width(can_panel, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(can_panel, lv_color_hex(0x444444), LV_PART_MAIN);
        lv_obj_clear_flag(can_panel, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *can_title = lv_label_create(can_panel);
        lv_label_set_text(can_title, "TX GPIO43    RX GPIO44");
        lv_obj_set_width(can_title, 204);
        lv_label_set_long_mode(can_title, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(can_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(can_title, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(can_title, ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
        lv_obj_align(can_title, LV_ALIGN_TOP_MID, 0, 14);

        lv_obj_t *can_detail = lv_label_create(can_panel);
        lv_label_set_text(can_detail, wired_can_obd_has_fresh_data() ? "OBD DATA ACTIVE" : "WAITING FOR DATA");
        lv_obj_set_width(can_detail, 204);
        lv_label_set_long_mode(can_detail, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(can_detail, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(can_detail, wired_can_obd_has_fresh_data() ? lv_color_hex(0x25D89A)
                                                                               : ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
        lv_obj_set_style_text_align(can_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(can_detail, LV_ALIGN_BOTTOM_MID, 0, -22);

        lv_obj_t *label_hint = lv_label_create(ui_ScreenPageBLEScan);
        lv_label_set_text(label_hint, "Swipe to return");
        lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, LV_PART_MAIN);
        lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, -34);

        lv_obj_move_foreground(spinner_ring);
        lv_obj_add_event_cb(ui_ScreenPageBLEScan, ui_event_ble_scan_background, LV_EVENT_GESTURE, NULL);
        return;
    }

    // ==== SAVED DEVICE SECTION ====
    bool has_saved;
    if (s_slave_mode) {
        const uint8_t *bound_mac = espnow_link_get_bound_master_mac();
        has_saved = (bound_mac[0] | bound_mac[1] | bound_mac[2] | bound_mac[3] | bound_mac[4] | bound_mac[5]) != 0;
    } else {
        has_saved = (saved_cfg->ble_device_name[0] != '\0');
    }

    s_label_saved_hdr = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(s_label_saved_hdr, s_slave_mode ? "SAVED DEVICE" : "SAVED");
    lv_obj_set_style_text_font(s_label_saved_hdr, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_saved_hdr, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_label_saved_hdr, LV_ALIGN_TOP_MID, 0, s_slave_mode ? 72 : 134);
    if (!has_saved) lv_obj_add_flag(s_label_saved_hdr, LV_OBJ_FLAG_HIDDEN);

    // Saved device row: name + delete button
    s_saved_panel = lv_obj_create(ui_ScreenPageBLEScan);
    lv_obj_remove_style_all(s_saved_panel);
    lv_obj_set_size(s_saved_panel, s_slave_mode ? 264 : 230, 32);
    lv_obj_align(s_saved_panel, LV_ALIGN_TOP_MID, 0, s_slave_mode ? 90 : 151);
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
    if (!s_slave_mode) {
        lv_obj_set_width(s_saved_name_lbl, 180);
        lv_label_set_long_mode(s_saved_name_lbl, LV_LABEL_LONG_DOT);
    }
    lv_obj_align(s_saved_name_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    // Delete button inside panel
    lv_obj_t *del_btn = lv_btn_create(s_saved_panel);
    lv_obj_set_style_clip_corner(del_btn, true, 0);
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
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0,
                 s_slave_mode ? 128 : (has_saved ? 189 : 153));
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, 255, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ==== NEARBY SCAN SECTION ====
    lv_obj_t *label_nearby = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_nearby, s_slave_mode ? "NEARBY" : "NEARBY DEVICES");
    lv_obj_set_style_text_font(label_nearby, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_nearby, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_nearby, LV_ALIGN_TOP_MID, 0,
                 s_slave_mode ? 134 : (has_saved ? 194 : 160));

    // Device list (scan results)
    s_list = lv_list_create(ui_ScreenPageBLEScan);
    lv_obj_set_size(s_list, s_slave_mode ? 264 : 230, s_slave_mode ? 145 : 76);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0,
                 s_slave_mode ? 152 : (has_saved ? 213 : 181));
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_list, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_list, 8, LV_PART_MAIN);

    // Hint text at bottom
    lv_obj_t *label_hint = lv_label_create(ui_ScreenPageBLEScan);
    lv_label_set_text(label_hint, s_slave_mode ? "Tap to connect  Slide to back" : "Tap a device to connect");
    lv_obj_set_style_text_font(label_hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(label_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(label_hint, LV_ALIGN_BOTTOM_MID, 0, s_slave_mode ? -15 : -34);

    // Gesture event for navigation
    lv_obj_move_foreground(spinner_ring);   // bring the ring to the front
    lv_obj_add_event_cb(ui_ScreenPageBLEScan, ui_event_ble_scan_background, LV_EVENT_GESTURE, NULL);

    // Start scanning
    start_scan();
}
