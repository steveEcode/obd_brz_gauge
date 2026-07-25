// RPM warning threshold + animation toggle page (swipe down from RPM page)

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

static lv_obj_t *s_slider_rpm_warn = NULL;
static lv_obj_t *s_label_rpm_warn_val = NULL;
static lv_obj_t *s_btn_anim_toggle = NULL;
static lv_obj_t *s_label_anim_state = NULL;

static void on_rpm_warn_slider_change(lv_event_t *e)
{
    (void)e;
    int32_t val = lv_slider_get_value(s_slider_rpm_warn);
    // 步进 500: 1000~8000
    val = ((val + 250) / 500) * 500;
    if (val < 1000) val = 1000;
    if (val > 8000) val = 8000;
    lv_slider_set_value(s_slider_rpm_warn, val, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_label_rpm_warn_val, "%ld", val);

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.rpm_warn_threshold = (uint16_t)val;
    nvs_cfg_set(&cfg);
}

static void on_anim_toggle_click(lv_event_t *e)
{
    (void)e;
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.rpm_warn_anim_en = cfg.rpm_warn_anim_en ? 0 : 1;
    nvs_cfg_set(&cfg);
    lv_label_set_text(s_label_anim_state, cfg.rpm_warn_anim_en ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_label_anim_state,
        cfg.rpm_warn_anim_en ? lv_color_hex(0x06D6A0) : lv_color_hex(0x666666), LV_PART_MAIN);
}

static void on_test_click(lv_event_t *e)
{
    (void)e;
    ui_rpm_flash_test_start();
}

void ui_ScreenPageRpmWarn_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageRpmWarn = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageRpmWarn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageRpmWarn, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageRpmWarn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageRpmWarn, 255, LV_PART_MAIN);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageRpmWarn);
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

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

    // 阈值数值
    uint16_t thresh = cfg->rpm_warn_threshold;
    if (thresh < 1000) thresh = 6000;
    s_label_rpm_warn_val = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text_fmt(s_label_rpm_warn_val, "%d", thresh);
    lv_obj_set_style_text_font(s_label_rpm_warn_val, &ui_font_FontTypoderSize44, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_rpm_warn_val, lv_color_hex(0xFF4D4D), LV_PART_MAIN);
    lv_obj_align(s_label_rpm_warn_val, LV_ALIGN_CENTER, 0, -30);

    // 滑块 (1000~8000)
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

    // 动画开关
    lv_obj_t *anim_label = lv_label_create(ui_ScreenPageRpmWarn);
    lv_label_set_text(anim_label, "FLASH ANIM");
    lv_obj_set_style_text_font(anim_label, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(anim_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(anim_label, LV_ALIGN_CENTER, -50, 82);

    s_btn_anim_toggle = lv_btn_create(ui_ScreenPageRpmWarn);
    lv_obj_set_size(s_btn_anim_toggle, 70, 32);
    lv_obj_align(s_btn_anim_toggle, LV_ALIGN_CENTER, 60, 82);
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

    // TEST 按钮: 一键触发 ~2s 闪烁测试
    lv_obj_t *btn_test = lv_btn_create(ui_ScreenPageRpmWarn);
    lv_obj_set_size(btn_test, 100, 32);
    lv_obj_align(btn_test, LV_ALIGN_CENTER, 0, 118);
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
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 148);

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
