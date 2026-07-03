// Oil pressure warning threshold page

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

static lv_obj_t *s_slider_oil_warn = NULL;
static lv_obj_t *s_label_oil_warn_val = NULL;

static void on_oil_warn_slider_change(lv_event_t *e)
{
    (void)e;

    int32_t val = lv_slider_get_value(s_slider_oil_warn);
    if (val < 0) val = 0;
    if (val > 100) val = 100;

    lv_slider_set_value(s_slider_oil_warn, val, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_label_oil_warn_val, "%ld.%ldbar", val / 10, val % 10);

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.oil_pressure_warn_x10 = (uint16_t)val;
    nvs_cfg_set(&cfg);
}

void ui_ScreenPageOilWarn_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageOilWarn = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageOilWarn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageOilWarn, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageOilWarn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageOilWarn, 255, LV_PART_MAIN);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageOilWarn);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ui_ScreenPageOilWarn);
    lv_label_set_text(title, "OIL WARN");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -122);

    lv_obj_t *sub = lv_label_create(ui_ScreenPageOilWarn);
    lv_label_set_text(sub, "High pressure alert");
    lv_obj_set_style_text_font(sub, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -88);

    s_label_oil_warn_val = lv_label_create(ui_ScreenPageOilWarn);
    lv_label_set_text_fmt(s_label_oil_warn_val, "%d.%dbar", cfg->oil_pressure_warn_x10 / 10, cfg->oil_pressure_warn_x10 % 10);
    lv_obj_set_style_text_font(s_label_oil_warn_val, &ui_font_FontTypoderSize44, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_oil_warn_val, lv_color_hex(0xFFD166), LV_PART_MAIN);
    lv_obj_align(s_label_oil_warn_val, LV_ALIGN_CENTER, 0, -24);

    s_slider_oil_warn = lv_slider_create(ui_ScreenPageOilWarn);
    lv_slider_set_range(s_slider_oil_warn, 0, 100);
    lv_slider_set_value(s_slider_oil_warn, cfg->oil_pressure_warn_x10, LV_ANIM_OFF);
    lv_obj_set_width(s_slider_oil_warn, 220);
    lv_obj_set_height(s_slider_oil_warn, 12);
    lv_obj_align(s_slider_oil_warn, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_style_bg_color(s_slider_oil_warn, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_oil_warn, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_oil_warn, lv_color_hex(0xFFD166), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_oil_warn, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_oil_warn, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_oil_warn, 6, LV_PART_KNOB);
    lv_obj_clear_flag(s_slider_oil_warn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_slider_oil_warn, on_oil_warn_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *range = lv_label_create(ui_ScreenPageOilWarn);
    lv_label_set_text(range, "0.0bar - 10.0bar");
    lv_obj_set_style_text_font(range, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(range, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_align(range, LV_ALIGN_CENTER, 0, 58);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageOilWarn);
    lv_label_set_text(hint, "Swipe up to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 110);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageOilWarn);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageOilWarn, ui_event_oil_warn_background, LV_EVENT_GESTURE, NULL);
}
