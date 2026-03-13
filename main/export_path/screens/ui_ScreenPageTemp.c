// Temperature Monitor Page
// CLT / IAT / OIL(SSM 22 10 17) - 3-row layout

#include "../ui.h"

// Value labels (externally accessible from timer callback)
lv_obj_t *ui_LabelCoolantTempText = NULL;
lv_obj_t *ui_LabelOilTempText     = NULL;  // 真实机油温度 °C (SSM 22 10 17, A-40)
lv_obj_t *ui_LabelIntakeTempText  = NULL;

// Helper: colored circle dot
static lv_obj_t *create_color_dot(lv_obj_t *parent, lv_color_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, 255, LV_PART_MAIN);
    lv_obj_set_align(dot, LV_ALIGN_CENTER);
    lv_obj_set_pos(dot, x, y);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

// Helper: create one data row (dot + name + value label + unit)
static void make_row(lv_obj_t *parent, lv_obj_t **val_out,
                     lv_coord_t cy, lv_color_t color,
                     const char *name_str, const char *unit_str)
{
    create_color_dot(parent, color, -105, cy);

    lv_obj_t *lbl_name = lv_label_create(parent);
    lv_label_set_text(lbl_name, name_str);
    lv_obj_set_style_text_font(lbl_name, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_name, color, LV_PART_MAIN);
    lv_obj_align(lbl_name, LV_ALIGN_CENTER, -65, cy);

    *val_out = lv_label_create(parent);
    lv_label_set_text(*val_out, "--");
    lv_obj_set_style_text_font(*val_out, &ui_font_FontTypoderSize36, LV_PART_MAIN);
    lv_obj_set_style_text_color(*val_out, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(*val_out, 110);
    lv_obj_set_style_text_align(*val_out, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(*val_out, LV_ALIGN_CENTER, 30, cy);

    lv_obj_t *lbl_unit = lv_label_create(parent);
    lv_label_set_text(lbl_unit, unit_str);
    lv_obj_set_style_text_font(lbl_unit, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_align(lbl_unit, LV_ALIGN_CENTER, 104, cy);
}

// Helper: horizontal divider line
static void make_hdiv(lv_obj_t *parent, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, w, 1);
    lv_obj_align(div, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_bg_color(div, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div, 50, LV_PART_MAIN);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

void ui_ScreenPageTemp_screen_init(void)
{
    ui_ScreenPageTemp = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageTemp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageTemp, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageTemp, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageTemp, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    // White border ring
    lv_obj_t *spinner_ring = lv_spinner_create(ui_ScreenPageTemp, 1000, 90);
    lv_obj_set_width(spinner_ring, 360);
    lv_obj_set_height(spinner_ring, 360);
    lv_obj_set_align(spinner_ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(spinner_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(spinner_ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(spinner_ring, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(spinner_ring, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(spinner_ring, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(spinner_ring, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(spinner_ring, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Inner arc ring (decorative)
    lv_obj_t *arc_bg = lv_arc_create(ui_ScreenPageTemp);
    lv_obj_set_width(arc_bg, 340);
    lv_obj_set_height(arc_bg, 340);
    lv_obj_set_align(arc_bg, LV_ALIGN_CENTER);
    lv_obj_clear_flag(arc_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_value(arc_bg, 0);
    lv_arc_set_bg_angles(arc_bg, 0, 360);
    lv_obj_set_style_arc_color(arc_bg, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc_bg, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc_bg, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(arc_bg, false, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(arc_bg, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(arc_bg, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(arc_bg, 20, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(arc_bg, 0, LV_PART_KNOB | LV_STATE_DEFAULT);

    // ====== Row 1 (cy=-65): CLT - Blue ======
    make_row(ui_ScreenPageTemp, &ui_LabelCoolantTempText, -65, lv_color_hex(0x44AAFF), "CLT", "'C");
    make_hdiv(ui_ScreenPageTemp, -30, 220);

    // ====== Row 2 (cy=+5): IAT - Green ======
    make_row(ui_ScreenPageTemp, &ui_LabelIntakeTempText, +5, lv_color_hex(0x44FF88), "IAT", "'C");
    make_hdiv(ui_ScreenPageTemp, +40, 220);

    // ====== Row 3 (cy=+75): OIL - Amber (SSM 22 10 17) ======
    make_row(ui_ScreenPageTemp, &ui_LabelOilTempText, +75, lv_color_hex(0xFF7722), "OIL", "'C");

    // Black ear image at top
    lv_obj_t *black_ear = lv_img_create(ui_ScreenPageTemp);
    lv_img_set_src(black_ear, &ui_img_pngblackear_png);
    lv_obj_set_width(black_ear, LV_SIZE_CONTENT);
    lv_obj_set_height(black_ear, LV_SIZE_CONTENT);
    lv_obj_set_x(black_ear, 0);
    lv_obj_set_y(black_ear, -142);
    lv_obj_set_align(black_ear, LV_ALIGN_CENTER);
    lv_obj_add_flag(black_ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(black_ear, LV_OBJ_FLAG_SCROLLABLE);

    // Events
    lv_obj_add_event_cb(ui_ScreenPageTemp, ui_event_temp_background, LV_EVENT_GESTURE, NULL);
}
