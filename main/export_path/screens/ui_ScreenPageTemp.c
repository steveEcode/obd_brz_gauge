// Temperature Monitor Page
// CLT / IAT / OIL(SSM 22 10 17) - 3-row layout

#include "../ui.h"

// Value labels (externally accessible from timer callback)
lv_obj_t *ui_LabelCoolantTempText = NULL;
lv_obj_t *ui_LabelOilTempText     = NULL;  // 真实机油温度 °C (SSM 22 10 17, A-40)
lv_obj_t *ui_LabelIntakeTempText  = NULL;
lv_obj_t *ui_LabelTempValue[3]    = {NULL, NULL, NULL};
lv_obj_t *ui_LabelTempName[3]     = {NULL, NULL, NULL};
lv_obj_t *ui_LabelTempUnit[3]     = {NULL, NULL, NULL};
lv_obj_t *ui_LabelTempDot[3]      = {NULL, NULL, NULL};  // 行首彩色圆点(颜色随数据项刷新)

// Helper: colored circle dot
static lv_obj_t *create_color_dot(lv_obj_t *parent, lv_color_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, 255, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, x, y);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

// Helper: create one data row (dot + name + value label + unit)
static void make_row(lv_obj_t *parent, lv_obj_t **name_out, lv_obj_t **val_out, lv_obj_t **unit_out,
                     lv_obj_t **dot_out, lv_coord_t cy, lv_color_t color,
                     const char *name_str, const char *unit_str)
{
    // Left column: value
    // Left boundary = 70px (matches divider line edge, safe for all row Y positions)
    *val_out = lv_label_create(parent);
    lv_label_set_long_mode(*val_out, LV_LABEL_LONG_CLIP);   // 数值过长不换行
    lv_label_set_text(*val_out, "--");
    lv_obj_set_style_text_font(*val_out, &ui_font_FontTypoderSize40, LV_PART_MAIN);
    lv_obj_set_style_text_color(*val_out, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(*val_out, 110);
    lv_obj_set_style_text_align(*val_out, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(*val_out, LV_ALIGN_LEFT_MID, 70, cy);

    // Right column: dot + name + unit
    // Right boundary = 290px (x=360-70, matches divider line edge)
    // dot left=185, name left=200..244, unit right=290
    *dot_out = create_color_dot(parent, color, 185, cy);

    *name_out = lv_label_create(parent);
    lv_label_set_long_mode(*name_out, LV_LABEL_LONG_CLIP);   // 不换行
    lv_label_set_text(*name_out, name_str);
    lv_obj_set_style_text_font(*name_out, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(*name_out, color, LV_PART_MAIN);
    lv_obj_set_width(*name_out, LV_SIZE_CONTENT);            // 宽度随文字, 长名(BOOST/SPEED)不换行
    lv_obj_set_style_text_align(*name_out, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(*name_out, LV_ALIGN_LEFT_MID, 200, cy);

    *unit_out = lv_label_create(parent);
    lv_label_set_long_mode(*unit_out, LV_LABEL_LONG_CLIP);   // 不换行
    lv_label_set_text(*unit_out, unit_str);
    lv_obj_set_style_text_font(*unit_out, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(*unit_out, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_width(*unit_out, LV_SIZE_CONTENT);            // 宽度随文字 (km/h 等)
    lv_obj_set_style_text_align(*unit_out, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(*unit_out, LV_ALIGN_RIGHT_MID, -70, cy);
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

    // White border ring (恢复原始 spinner@360)
    lv_obj_t *spinner_ring = lv_obj_create(ui_ScreenPageTemp);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(spinner_ring, 360, 360);
    lv_obj_set_align(spinner_ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(spinner_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(spinner_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(spinner_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(spinner_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(spinner_ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(spinner_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(spinner_ring, 255, LV_PART_MAIN);


    // ====== Row 1 (cy=-65): CLT - Blue ======
    make_row(ui_ScreenPageTemp, &ui_LabelTempName[0], &ui_LabelTempValue[0], &ui_LabelTempUnit[0], &ui_LabelTempDot[0], -65, lv_color_hex(0x44AAFF), "CLT", "'C");
    make_hdiv(ui_ScreenPageTemp, -30, 220);

    // ====== Row 2 (cy=+5): IAT - Green ======
    make_row(ui_ScreenPageTemp, &ui_LabelTempName[1], &ui_LabelTempValue[1], &ui_LabelTempUnit[1], &ui_LabelTempDot[1], +5, lv_color_hex(0x44FF88), "IAT", "'C");
    make_hdiv(ui_ScreenPageTemp, +40, 220);

    // ====== Row 3 (cy=+75): OIL - Amber (SSM 22 10 17) ======
    make_row(ui_ScreenPageTemp, &ui_LabelTempName[2], &ui_LabelTempValue[2], &ui_LabelTempUnit[2], &ui_LabelTempDot[2], +75, lv_color_hex(0xFF7722), "OIL", "'C");

    // Backward compatibility for existing update code
    ui_LabelCoolantTempText = ui_LabelTempValue[0];
    ui_LabelIntakeTempText = ui_LabelTempValue[1];
    ui_LabelOilTempText = ui_LabelTempValue[2];

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
    lv_obj_move_foreground(spinner_ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageTemp, ui_event_temp_background, LV_EVENT_GESTURE, NULL);
}
