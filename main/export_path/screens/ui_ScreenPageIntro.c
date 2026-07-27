// 三连表开机动画屏 (RACE / AS / ONE)
//  - 每台设备只显示"本机位置"对应的那个词; 三台按 1→2→3 顺序点亮, 全亮保持后一起进页面。
//  - 词由 my_timerMain 按 s_intro_step + 本机 device_position 实时设置到 ui_LabelIntroWord。
//  - 本屏本身只负责布局(黑底 + 一个大号居中标签)。

#include "../ui.h"

lv_obj_t *ui_LabelIntroWord = NULL;

void ui_ScreenPageIntro_screen_init(void)
{
    ui_ScreenPageIntro = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageIntro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageIntro, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageIntro, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageIntro, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageIntro, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_ScreenPageIntro, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageIntro, 0, LV_PART_MAIN);

    // White border ring (与其它页面一致)
    lv_obj_t *ring = lv_obj_create(ui_ScreenPageIntro);
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    ui_LabelIntroWord = lv_label_create(ui_ScreenPageIntro);
    lv_label_set_text(ui_LabelIntroWord, "");
    lv_obj_set_style_text_font(ui_LabelIntroWord, &ui_font_FontTypoderSize56, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelIntroWord, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_LabelIntroWord, 4, LV_PART_MAIN);
    lv_obj_center(ui_LabelIntroWord);

    lv_obj_move_foreground(ring);   // 圆环置顶(与其它页一致)
}
