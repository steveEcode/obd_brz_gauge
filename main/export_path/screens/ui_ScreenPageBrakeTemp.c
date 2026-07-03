// Brake Temperature Page (RS485 probe)

#include "../ui.h"

lv_obj_t *ui_LabelBrakeTempText = NULL;
lv_obj_t *ui_ChartBrakeTemp = NULL;
lv_chart_series_t *ui_BrakeTempChartSeries = NULL;

void ui_ScreenPageBrakeTemp_screen_init(void)
{
    ui_ScreenPageBrakeTemp = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageBrakeTemp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageBrakeTemp, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageBrakeTemp, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageBrakeTemp, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_pad_all(ui_ScreenPageBrakeTemp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageBrakeTemp);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 8, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ui_ScreenPageBrakeTemp);
    lv_label_set_text(title, "BRAKE TEMP");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF5533), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -122);

    lv_obj_t *dot = lv_obj_create(ui_ScreenPageBrakeTemp);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF5533), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, 255, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_CENTER, -106, -22);

    ui_LabelBrakeTempText = lv_label_create(ui_ScreenPageBrakeTemp);
    lv_label_set_text(ui_LabelBrakeTempText, "--.-");
    lv_obj_set_style_text_font(ui_LabelBrakeTempText, &ui_font_FontTypoderSize28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelBrakeTempText, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(ui_LabelBrakeTempText, 172);
    lv_obj_set_style_text_align(ui_LabelBrakeTempText, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_LabelBrakeTempText, LV_ALIGN_CENTER, -10, -24);

    lv_obj_t *unit = lv_label_create(ui_ScreenPageBrakeTemp);
    lv_label_set_text(unit, "'C");
    lv_obj_set_style_text_font(unit, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x999999), LV_PART_MAIN);
    lv_obj_align(unit, LV_ALIGN_CENTER, 88, -26);

    lv_obj_t *trend_panel = lv_obj_create(ui_ScreenPageBrakeTemp);
    lv_obj_set_size(trend_panel, 272, 116);
    lv_obj_align(trend_panel, LV_ALIGN_CENTER, 0, 62);
    lv_obj_clear_flag(trend_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(trend_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_radius(trend_panel, 48, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(trend_panel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_ChartBrakeTemp = lv_chart_create(trend_panel);
    lv_obj_set_size(ui_ChartBrakeTemp, 272, 116);
    lv_obj_center(ui_ChartBrakeTemp);
    lv_obj_set_style_bg_opa(ui_ChartBrakeTemp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_ChartBrakeTemp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartBrakeTemp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ChartBrakeTemp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_size(ui_ChartBrakeTemp, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartBrakeTemp, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui_ChartBrakeTemp, true, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_ChartBrakeTemp, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ChartBrakeTemp, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_ChartBrakeTemp, lv_color_hex(0xFF6B3D), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartBrakeTemp, 1, LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_ChartBrakeTemp, lv_color_hex(0x242424), LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_chart_set_type(ui_ChartBrakeTemp, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui_ChartBrakeTemp, 30);
    lv_chart_set_range(ui_ChartBrakeTemp, LV_CHART_AXIS_PRIMARY_Y, 0, 1200);
    lv_chart_set_update_mode(ui_ChartBrakeTemp, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(ui_ChartBrakeTemp, 4, 5);
    lv_chart_set_axis_tick(ui_ChartBrakeTemp, LV_CHART_AXIS_PRIMARY_X, 0, 0, 0, 0, true, 0);
    lv_chart_set_axis_tick(ui_ChartBrakeTemp, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 0, 0, true, 0);
    ui_BrakeTempChartSeries = lv_chart_add_series(ui_ChartBrakeTemp, lv_color_hex(0xFF6B3D), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(ui_ChartBrakeTemp, ui_BrakeTempChartSeries, 0);

    lv_obj_t *black_ear = lv_img_create(ui_ScreenPageBrakeTemp);
    lv_img_set_src(black_ear, &ui_img_pngblackear_png);
    lv_obj_set_width(black_ear, LV_SIZE_CONTENT);
    lv_obj_set_height(black_ear, LV_SIZE_CONTENT);
    lv_obj_set_align(black_ear, LV_ALIGN_CENTER);
    lv_obj_set_pos(black_ear, 0, -142);
    lv_obj_add_flag(black_ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(black_ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageBrakeTemp, ui_event_brake_temp_background, LV_EVENT_GESTURE, NULL);
}
