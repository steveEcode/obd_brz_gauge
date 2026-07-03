// Multi-Gauge (三连表) Settings Page
// 设置页下滑进入。选择本机角色: MASTER(连ELM327读数+广播) / SLAVE(只收主表数据显示)。
// 角色写入 NVS device_role, 重启(下次点火)生效。
// 上滑/左右滑返回设置页。 (选主表 MAC 的扫描页为后续步骤)

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

static const char *mode_names = "MASTER\nSLAVE";   // 顺序须与 device_role 一致: 0=MASTER, 1=SLAVE

static lv_obj_t *s_roller_mode = NULL;

static void on_mode_roller_change(lv_event_t *e)
{
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.device_role = (uint8_t)lv_roller_get_selected(s_roller_mode); // 0=主 1=从
    nvs_cfg_set(&cfg);
}

void ui_ScreenPageMultiGauge_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageMultiGauge = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageMultiGauge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageMultiGauge, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageMultiGauge, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageMultiGauge, 255, LV_PART_MAIN);

    // White border ring (与其它页一致)
    lv_obj_t *ring = lv_obj_create(ui_ScreenPageMultiGauge);
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(title, "MULTI-GAUGE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -118);

    // Label
    lv_obj_t *label_mode = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(label_mode, "DEVICE MODE");
    lv_obj_set_style_text_font(label_mode, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_mode, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_mode, LV_ALIGN_CENTER, 0, -60);

    // Master/Slave roller
    s_roller_mode = lv_roller_create(ui_ScreenPageMultiGauge);
    lv_obj_clear_flag(s_roller_mode, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(s_roller_mode, mode_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_mode, 1);
    lv_roller_set_selected(s_roller_mode, (cfg->device_role <= 1) ? cfg->device_role : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_mode, 200);
    lv_obj_set_style_text_font(s_roller_mode, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_roller_mode, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_roller_mode, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_roller_mode, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roller_mode, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roller_mode, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_radius(s_roller_mode, 8, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_roller_mode, &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_mode, lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_roller_mode, lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller_mode, 255, LV_PART_SELECTED);
    lv_obj_align(s_roller_mode, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_event_cb(s_roller_mode, on_mode_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // Reboot hint
    lv_obj_t *hint = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(hint, "Reboot to apply");
    lv_obj_set_style_text_font(hint, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFAA33), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 28);

    // Back hint
    lv_obj_t *back = lv_label_create(ui_ScreenPageMultiGauge);
    lv_label_set_text(back, "Swipe up: back");
    lv_obj_set_style_text_font(back, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(back, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_CENTER, 0, 70);

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
