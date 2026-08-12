// RPM warning threshold + animation toggle page (swipe down from RPM page)

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"

static lv_obj_t *s_slider_rpm_warn = NULL;
static lv_obj_t *s_label_rpm_warn_val = NULL;
static lv_obj_t *s_btn_anim_toggle = NULL;
static lv_obj_t *s_label_anim_state = NULL;
static lv_obj_t *s_label_linked_state = NULL;

static void on_rpm_warn_slider_change(lv_event_t *e)
{
    (void)e;
    int32_t val = lv_slider_get_value(s_slider_rpm_warn);
    // Step 500: 1000~8000
    val = ((val + 250) / 500) * 500;
    if (val < 1000) val = 1000;
    if (val > 8000) val = 8000;
    lv_slider_set_value(s_slider_rpm_warn, val, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_label_rpm_warn_val, "%ld", val);

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.rpm_warn_threshold = (uint16_t)val;
    nvs_cfg_set(&cfg);

    // Linked mode: changing the threshold on this unit → broadcast to the other gauges (relayed via the master, keeping the threshold consistent across all three gauges)
    if (cfg.rpm_warn_linked_en && cfg.device_role != ESPNOW_ROLE_STANDALONE) {
        espnow_link_broadcast_threshold((uint16_t)val);
    }
}

// FLASH ANIM and LINKED FLASH are mutually exclusive: at most one can be on (or both off); turning one on automatically turns the other off
static void on_anim_toggle_click(lv_event_t *e)
{
    (void)e;
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.rpm_warn_anim_en = cfg.rpm_warn_anim_en ? 0 : 1;
    if (cfg.rpm_warn_anim_en && cfg.rpm_warn_linked_en) {
        cfg.rpm_warn_linked_en = 0;
        if (s_label_linked_state) {
            lv_label_set_text(s_label_linked_state, "OFF");
            lv_obj_set_style_text_color(s_label_linked_state, lv_color_hex(0x666666), LV_PART_MAIN);
        }
    }
    nvs_cfg_set(&cfg);
    lv_label_set_text(s_label_anim_state, cfg.rpm_warn_anim_en ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_label_anim_state,
        cfg.rpm_warn_anim_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
}

static void on_linked_toggle_click(lv_event_t *e)
{
    (void)e;
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.rpm_warn_linked_en = cfg.rpm_warn_linked_en ? 0 : 1;
    if (cfg.rpm_warn_linked_en && cfg.rpm_warn_anim_en) {
        cfg.rpm_warn_anim_en = 0;
        if (s_label_anim_state) {
            lv_label_set_text(s_label_anim_state, "OFF");
            lv_obj_set_style_text_color(s_label_anim_state, lv_color_hex(0x666666), LV_PART_MAIN);
        }
    }
    nvs_cfg_set(&cfg);
    lv_label_set_text(s_label_linked_state, cfg.rpm_warn_linked_en ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_label_linked_state,
        cfg.rpm_warn_linked_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
}

static void on_test_click(lv_event_t *e)
{
    (void)e;
    const nvs_user_cfg_t *cfg = nvs_cfg_get();
    bool linked_active = cfg->rpm_warn_linked_en && (cfg->device_role != ESPNOW_ROLE_STANDALONE);
    if (linked_active) {
        // Linked: the master drives the RPM climb (0→peak→fallback) and all three gauges render synchronously via ESP-NOW.
        // If this unit is the master → start directly; if it is the slave → send a request to the master, so all gauges test together either way.
        espnow_link_trigger_linked_test();
    } else {
        ui_rpm_flash_test_start();         // normal: ~2s flash
    }
}

// Refresh this page after another gauge syncs the threshold/toggles (called when the UI task receives APP_EVT_ESPNOW_THRESH_SYNC)
void ui_rpm_warn_refresh_from_nvs(void)
{
    if (!ui_ScreenPageRpmWarn) return;
    const nvs_user_cfg_t *cfg = nvs_cfg_get();
    if (s_label_rpm_warn_val) lv_label_set_text_fmt(s_label_rpm_warn_val, "%d", cfg->rpm_warn_threshold);
    if (s_slider_rpm_warn) lv_slider_set_value(s_slider_rpm_warn, cfg->rpm_warn_threshold, LV_ANIM_OFF);
    if (s_label_anim_state) {
        lv_label_set_text(s_label_anim_state, cfg->rpm_warn_anim_en ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_label_anim_state,
            cfg->rpm_warn_anim_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
    }
    if (s_label_linked_state) {
        lv_label_set_text(s_label_linked_state, cfg->rpm_warn_linked_en ? "ON" : "OFF");
        lv_obj_set_style_text_color(s_label_linked_state,
            cfg->rpm_warn_linked_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
    }
}

void ui_ScreenPageRpmWarn_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageRpmWarn = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageRpmWarn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageRpmWarn, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageRpmWarn);
    lv_obj_set_style_bg_opa(ui_ScreenPageRpmWarn, 255, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageRpmWarn, 10);

    lv_obj_t *title = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(title, "RPM WARN");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -122);

    lv_obj_t *sub = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(sub, "Over-rev alert");
    lv_obj_set_style_text_font(sub, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -88);

    // Threshold value (already clamped in nvs_storage_init(), just read it here)
    uint16_t thresh = cfg->rpm_warn_threshold;
    s_label_rpm_warn_val = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text_fmt(s_label_rpm_warn_val, "%d", thresh);
    lv_obj_set_style_text_font(s_label_rpm_warn_val, &ui_font_FontTypoderSize44, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_rpm_warn_val, lv_color_hex(0xFF4D4D), LV_PART_MAIN);
    lv_obj_align(s_label_rpm_warn_val, LV_ALIGN_CENTER, 0, -30);

    // Slider (1000~8000)
    s_slider_rpm_warn = lv_slider_create(ui_ScreenPageRpmWarn);
    lv_slider_set_range(s_slider_rpm_warn, 1000, 8000);
    lv_slider_set_value(s_slider_rpm_warn, thresh, LV_ANIM_OFF);
    lv_obj_set_width(s_slider_rpm_warn, 220);
    lv_obj_set_height(s_slider_rpm_warn, 12);
    lv_obj_align(s_slider_rpm_warn, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_bg_color(s_slider_rpm_warn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_rpm_warn, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_rpm_warn, lv_color_hex(0xFF4D4D), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_rpm_warn, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_rpm_warn, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_rpm_warn, 6, LV_PART_KNOB);
    lv_obj_clear_flag(s_slider_rpm_warn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_slider_rpm_warn, on_rpm_warn_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *range = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(range, "1000 - 8000 rpm");
    lv_obj_set_style_text_font(range, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(range, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_align(range, LV_ALIGN_CENTER, 0, 48);

    bool is_standalone = (cfg->device_role == ESPNOW_ROLE_STANDALONE);

    // Animation toggle
    lv_obj_t *anim_label = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(anim_label, "FLASH ANIM");
    lv_obj_set_style_text_font(anim_label, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(anim_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(anim_label, LV_ALIGN_CENTER, -50, 70);

    s_btn_anim_toggle = lv_btn_create(ui_ScreenPageRpmWarn);
    lv_obj_set_size(s_btn_anim_toggle, 70, 32);
    lv_obj_align(s_btn_anim_toggle, LV_ALIGN_CENTER, 60, 70);
    lv_obj_set_style_bg_color(s_btn_anim_toggle, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_btn_anim_toggle, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_anim_toggle, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_btn_anim_toggle, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_clear_flag(s_btn_anim_toggle, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_btn_anim_toggle, on_anim_toggle_click, LV_EVENT_CLICKED, NULL);

    s_label_anim_state = lv_label_create(s_btn_anim_toggle);
    lv_label_set_text(s_label_anim_state, cfg->rpm_warn_anim_en ? "ON" : "OFF");
    lv_obj_set_style_text_font(s_label_anim_state, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_anim_state,
        cfg->rpm_warn_anim_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_center(s_label_anim_state);

    // Multi-gauge linked toggle (shown only in triple-gauge mode): gauges 1/2/3 turn red one by one as RPM rises → stay red, and all gauges flash at the threshold
    if (!is_standalone) {
        lv_obj_t *linked_label = lv_label_create(ui_ScreenPageRpmWarn);
        lv_label_set_text(linked_label, "LINKED FLASH");
        lv_obj_set_style_text_font(linked_label, &ui_font_FontTypoderSize16, LV_PART_MAIN);
        lv_obj_set_style_text_color(linked_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
        lv_obj_align(linked_label, LV_ALIGN_CENTER, -50, 102);

        lv_obj_t *btn_linked = lv_btn_create(ui_ScreenPageRpmWarn);
        lv_obj_set_size(btn_linked, 70, 32);
        lv_obj_align(btn_linked, LV_ALIGN_CENTER, 60, 102);
        lv_obj_set_style_bg_color(btn_linked, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn_linked, 255, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn_linked, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn_linked, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_clear_flag(btn_linked, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btn_linked, on_linked_toggle_click, LV_EVENT_CLICKED, NULL);

        s_label_linked_state = lv_label_create(btn_linked);
        lv_label_set_text(s_label_linked_state, cfg->rpm_warn_linked_en ? "ON" : "OFF");
        lv_obj_set_style_text_font(s_label_linked_state, &ui_font_FontTypoderSize16, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_label_linked_state,
            cfg->rpm_warn_linked_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
        lv_obj_center(s_label_linked_state);
    }

    // TEST button: one tap triggers the ~2s flash test
    lv_obj_t *btn_test = lv_btn_create(ui_ScreenPageRpmWarn);
    lv_obj_set_size(btn_test, 100, 32);
    lv_obj_align(btn_test, LV_ALIGN_CENTER, 0, is_standalone ? 118 : 134);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(0xFF4D4D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_test, 255, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_test, 8, LV_PART_MAIN);
    lv_obj_clear_flag(btn_test, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn_test, on_test_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_test = lv_label_create(btn_test);
    lv_label_set_text(lbl_test, "TEST");
    lv_obj_set_style_text_font(lbl_test, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_test, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(lbl_test);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(hint, "Swipe up to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, is_standalone ? 148 : 156);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageRpmWarn);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageRpmWarn, ui_event_rpm_warn_background, LV_EVENT_GESTURE, NULL);
}
