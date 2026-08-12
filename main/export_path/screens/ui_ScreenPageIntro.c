// Triple-gauge boot animation screen (RACE / AS / ONE)
//  - Each device shows only the word for its own position; the three units light up in order 1→2→3, stay fully lit, then enter the page together.
//  - The word is set in real time by my_timerMain into ui_LabelIntroWord, based on s_intro_step + this unit's device_position.
//  - This screen itself only handles the layout (black background + one large centered label).

#include "../ui.h"

lv_obj_t *ui_LabelIntroWord = NULL;

void ui_ScreenPageIntro_screen_init(void)
{
    ui_ScreenPageIntro = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageIntro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageIntro, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageIntro);
    lv_obj_set_style_bg_opa(ui_ScreenPageIntro, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageIntro, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_ScreenPageIntro, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageIntro, 0, LV_PART_MAIN);

    // White border ring (consistent with the other pages)
    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageIntro, 10);

    ui_LabelIntroWord = lv_label_create(ui_ScreenPageIntro);
    lv_label_set_text(ui_LabelIntroWord, "");
    lv_obj_set_style_text_font(ui_LabelIntroWord, &ui_font_FontTypoderSize56, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelIntroWord, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(ui_LabelIntroWord, 4, LV_PART_MAIN);
    lv_obj_center(ui_LabelIntroWord);

    lv_obj_move_foreground(ring);   // bring the ring to the front (consistent with the other pages)
}
