#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

extern lv_obj_t *ui_ScreenPageTempCustom;
static lv_obj_t *s_temp_rollers[3] = {NULL, NULL, NULL};

static const char *k_data_options =
    "CLT\n"
    "IAT\n"
    "OIL\n"
    "LOAD\n"
    "TPS\n"
    "RPM\n"
    "SPEED\n"
    "BAT\n"
    "OILP\n"
    "BKT\n"
    "BOOST";

static void on_temp_map_changed(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 3) return;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.temp_display_map[idx] = (uint8_t)lv_roller_get_selected(s_temp_rollers[idx]);
    nvs_cfg_set(&cfg);
}

static void create_row(lv_obj_t *parent, int idx, const char *title, lv_coord_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, -92, y);

    s_temp_rollers[idx] = lv_roller_create(parent);
    lv_obj_clear_flag(s_temp_rollers[idx], LV_OBJ_FLAG_GESTURE_BUBBLE); // 滚动选值时不触发页面手势
    lv_roller_set_options(s_temp_rollers[idx], k_data_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_temp_rollers[idx], 1);
    lv_obj_set_width(s_temp_rollers[idx], 170);
    lv_obj_set_height(s_temp_rollers[idx], 42);
    lv_obj_set_style_text_font(s_temp_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_temp_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_temp_rollers[idx], lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_temp_rollers[idx], 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_temp_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_temp_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_temp_rollers[idx], lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_align(s_temp_rollers[idx], LV_ALIGN_CENTER, 34, y);
    lv_obj_add_event_cb(s_temp_rollers[idx], on_temp_map_changed, LV_EVENT_VALUE_CHANGED, (void *)idx);
}

void ui_ScreenPageTempCustom_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageTempCustom = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageTempCustom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageTempCustom, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageTempCustom, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageTempCustom, 255, LV_PART_MAIN);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageTempCustom);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ui_ScreenPageTempCustom);
    lv_label_set_text(title, "TEMP CUSTOM");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -132);

    create_row(ui_ScreenPageTempCustom, 0, "ROW 1", -72);
    create_row(ui_ScreenPageTempCustom, 1, "ROW 2", -18);
    create_row(ui_ScreenPageTempCustom, 2, "ROW 3", 36);

    lv_roller_set_selected(s_temp_rollers[0], cfg->temp_display_map[0], LV_ANIM_OFF);
    lv_roller_set_selected(s_temp_rollers[1], cfg->temp_display_map[1], LV_ANIM_OFF);
    lv_roller_set_selected(s_temp_rollers[2], cfg->temp_display_map[2], LV_ANIM_OFF);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageTempCustom);
    lv_label_set_text(hint, "Swipe up/left/right to return");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 92);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageTempCustom);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_align(ear, LV_ALIGN_CENTER, 0, -142);

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageTempCustom, ui_event_temp_custom_background, LV_EVENT_GESTURE, NULL);
}
