#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

extern lv_obj_t *ui_ScreenPageInfoCustom;
static lv_obj_t *s_info_rollers[5] = {NULL, NULL, NULL, NULL, NULL};

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

static void on_info_map_changed(lv_event_t *e)
{
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx >= 5) return;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.info_display_map[idx] = (uint8_t)lv_roller_get_selected(s_info_rollers[idx]);
    nvs_cfg_set(&cfg);
}

static void create_row(lv_obj_t *parent, int idx, const char *title, lv_coord_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, -100, y);

    s_info_rollers[idx] = lv_roller_create(parent);
    lv_obj_clear_flag(s_info_rollers[idx], LV_OBJ_FLAG_GESTURE_BUBBLE); // 滚动选值时不触发页面手势
    lv_roller_set_options(s_info_rollers[idx], k_data_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_info_rollers[idx], 1);
    lv_obj_set_width(s_info_rollers[idx], 170);
    lv_obj_set_height(s_info_rollers[idx], 38);
    lv_obj_set_style_text_font(s_info_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_info_rollers[idx], &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_info_rollers[idx], lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_info_rollers[idx], 255, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_info_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_info_rollers[idx], lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_info_rollers[idx], lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_align(s_info_rollers[idx], LV_ALIGN_CENTER, 30, y);
    lv_obj_add_event_cb(s_info_rollers[idx], on_info_map_changed, LV_EVENT_VALUE_CHANGED, (void *)idx);
}

void ui_ScreenPageInfoCustom_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageInfoCustom = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageInfoCustom, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageInfoCustom, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageInfoCustom, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageInfoCustom, 255, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageInfoCustom, 10);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口

    lv_obj_t *title = lv_label_create(ui_ScreenPageInfoCustom);
    lv_label_set_text(title, "INFO CUSTOM");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -132);

    create_row(ui_ScreenPageInfoCustom, 0, "SLOT 1", -82);
    create_row(ui_ScreenPageInfoCustom, 1, "SLOT 2", -38);
    create_row(ui_ScreenPageInfoCustom, 2, "SLOT 3", 6);
    create_row(ui_ScreenPageInfoCustom, 3, "SLOT 4", 50);
    create_row(ui_ScreenPageInfoCustom, 4, "SLOT 5", 94);

    for (int i = 0; i < 5; ++i) {
        lv_roller_set_selected(s_info_rollers[i], cfg->info_display_map[i], LV_ANIM_OFF);
    }

    lv_obj_t *hint = lv_label_create(ui_ScreenPageInfoCustom);
    lv_label_set_text(hint, "Swipe up/left/right to return");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageInfoCustom);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_align(ear, LV_ALIGN_CENTER, 0, -142);

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageInfoCustom, ui_event_info_custom_background, LV_EVENT_GESTURE, NULL);
}
