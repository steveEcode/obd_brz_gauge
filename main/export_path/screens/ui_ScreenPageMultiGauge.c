// Multi-Gauge (triple-gauge) Settings Page  (entered by swiping down from the settings page)
//  - MODE: MASTER (connects to ELM327 + broadcasts) / SLAVE (receives the master's data) / STANDALONE (standalone, WiFi not started) → NVS device_role
//  - POSITION: this unit's position 1/2/3 (RACE/AS/ONE) in the boot animation     → NVS device_position
//  - INTRO: boot animation on/off (played only in multi-gauge mode)                        → NVS intro_enable
//  Changes take effect after reboot (next ignition). Swipe up/left/right to return to the settings page.

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"   // ESPNOW_ROLE_STANDALONE

static const char *mode_names = "MASTER\nSLAVE\nSTANDALONE";   // index=device_role: 0=MASTER,1=SLAVE,2=STANDALONE
static const char *pos_names  = "1\n2\n3";          // index 0/1/2 → position 1/2/3
static const char *intro_names = "OFF\nRACE\nREI\nSHINJI\nASUKA";  // 0=OFF, 1=RACE, 2/3/4=VIDEO A/B/C

static lv_obj_t *s_roller_mode = NULL;
static lv_obj_t *s_roller_pos  = NULL;
static lv_obj_t *s_roller_intro = NULL;
static lv_obj_t *s_lbl_pos  = NULL;
static lv_obj_t *s_lbl_intro = NULL;

// Standalone hides POS, multi-gauge shows all
static void mg_update_visibility(uint8_t role)
{
    bool is_standalone = (role == ESPNOW_ROLE_STANDALONE);
    if (s_lbl_pos)    { if (is_standalone) lv_obj_add_flag(s_lbl_pos, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_lbl_pos, LV_OBJ_FLAG_HIDDEN); }
    if (s_roller_pos) { if (is_standalone) lv_obj_add_flag(s_roller_pos, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(s_roller_pos, LV_OBJ_FLAG_HIDDEN); }
}

static void on_mode_roller_change(lv_event_t *e)
{
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.device_role = (uint8_t)lv_roller_get_selected(s_roller_mode);
    nvs_cfg_set(&cfg);
    mg_update_visibility(cfg.device_role);
}
static void on_pos_roller_change(lv_event_t *e)
{
    nvs_device_position_set((uint8_t)lv_roller_get_selected(s_roller_pos) + 1); // index→1/2/3
}
static void on_intro_roller_change(lv_event_t *e)
{
    nvs_intro_enable_set((uint8_t)lv_roller_get_selected(s_roller_intro));
}

// Shared roller style
static void style_mg_roller(lv_obj_t *r)
{
    lv_obj_clear_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_visible_row_count(r, 1);
    lv_obj_set_width(r, 160);
    ui_helpers_style_dark_roller(r, &ui_font_FontTypoderSize20);
}

static void make_mg_label(lv_obj_t *parent, const char *txt, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
}

void ui_ScreenPageMultiGauge_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageMultiGauge = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageMultiGauge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageMultiGauge, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageMultiGauge);
    lv_obj_set_style_bg_opa(ui_ScreenPageMultiGauge, 255, LV_PART_MAIN);

    // White border ring
    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageMultiGauge, 10);

    lv_obj_t *title = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(title, "MULTI-GAUGE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -130);

    // Row 1: MODE
    make_mg_label(ui_ScreenPageMultiGauge, "MODE", -105);
    s_roller_mode = lv_roller_create(ui_ScreenPageMultiGauge);
    style_mg_roller(s_roller_mode);
    lv_roller_set_options(s_roller_mode, mode_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_roller_mode, (cfg->device_role <= 2) ? cfg->device_role : ESPNOW_ROLE_STANDALONE, LV_ANIM_OFF);
    lv_obj_align(s_roller_mode, LV_ALIGN_CENTER, 0, -85);
    lv_obj_add_event_cb(s_roller_mode, on_mode_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // Row 2: POS (RACE/AS/ONE position)
    s_lbl_pos = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(s_lbl_pos, "POS");
    lv_obj_set_style_text_font(s_lbl_pos, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_pos, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_lbl_pos, LV_ALIGN_CENTER, 0, -55);
    s_roller_pos = lv_roller_create(ui_ScreenPageMultiGauge);
    style_mg_roller(s_roller_pos);
    lv_roller_set_options(s_roller_pos, pos_names, LV_ROLLER_MODE_NORMAL);
    {
        uint8_t p = nvs_device_position_get();
        if (p < 1 || p > 3) p = 1;
        lv_roller_set_selected(s_roller_pos, p - 1, LV_ANIM_OFF);
    }
    lv_obj_align(s_roller_pos, LV_ALIGN_CENTER, 0, -35);
    lv_obj_add_event_cb(s_roller_pos, on_pos_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // Row 3: INTRO (multi-gauge: OFF/RACE/VIDEO)
    s_lbl_intro = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(s_lbl_intro, "INTRO");
    lv_obj_set_style_text_font(s_lbl_intro, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_intro, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(s_lbl_intro, LV_ALIGN_CENTER, 0, -5);
    s_roller_intro = lv_roller_create(ui_ScreenPageMultiGauge);
    style_mg_roller(s_roller_intro);
    lv_roller_set_options(s_roller_intro, intro_names, LV_ROLLER_MODE_NORMAL);
    {
        uint8_t ie = nvs_intro_enable_get();
        if (ie > 4) ie = 0;
        lv_roller_set_selected(s_roller_intro, ie, LV_ANIM_OFF);
    }
    lv_obj_align(s_roller_intro, LV_ALIGN_CENTER, 0, 15);
    lv_obj_add_event_cb(s_roller_intro, on_intro_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // Hide irrelevant rows based on role
    mg_update_visibility(cfg->device_role);

    // Hint
    lv_obj_t *hint = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(hint, "Reboot to apply  ·  swipe to back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFAA33), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 100);

    // Black ear image at top
    lv_obj_t *ear = lv_img_create(ui_ScreenPageMultiGauge);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageMultiGauge, ui_event_multi_gauge_background, LV_EVENT_GESTURE, NULL);
}
