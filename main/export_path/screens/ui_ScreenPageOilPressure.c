// Oil Pressure Page

#include "../ui.h"

lv_obj_t *ui_LabelOilPressureText = NULL;
lv_obj_t *ui_ChartOilPressure = NULL;
lv_chart_series_t *ui_OilPressureChartSeries = NULL;

void ui_ScreenPageOilPressure_screen_init(void)
{
    ui_ScreenPageOilPressure = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageOilPressure, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageOilPressure, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageOilPressure, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageOilPressure, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageOilPressure);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 8, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(title, "OIL PRESS");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFD166), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -122);

    lv_obj_t *dot = lv_obj_create(ui_ScreenPageOilPressure);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFD166), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, 255, LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_CENTER, -108, -14);

    ui_LabelOilPressureText = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(ui_LabelOilPressureText, "--.-");
    lv_obj_set_style_text_font(ui_LabelOilPressureText, &ui_font_FontTypoderSize56, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelOilPressureText, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(ui_LabelOilPressureText, 220);
    lv_obj_set_style_text_align(ui_LabelOilPressureText, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_LabelOilPressureText, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *unit = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(unit, "bar");
    lv_obj_set_style_text_font(unit, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x999999), LV_PART_MAIN);
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 30);

    lv_obj_t *trend_panel = lv_obj_create(ui_ScreenPageOilPressure);
    lv_obj_set_size(trend_panel, 272, 116);
    lv_obj_align(trend_panel, LV_ALIGN_CENTER, 0, 82);
    lv_obj_clear_flag(trend_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(trend_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_radius(trend_panel, 48, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(trend_panel, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(trend_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_ChartOilPressure = lv_chart_create(trend_panel);
    lv_obj_set_size(ui_ChartOilPressure, 272, 116);
    lv_obj_center(ui_ChartOilPressure);
    lv_obj_set_style_bg_opa(ui_ChartOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_ChartOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ChartOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_size(ui_ChartOilPressure, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartOilPressure, 3, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(ui_ChartOilPressure, true, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_ChartOilPressure, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ChartOilPressure, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_ChartOilPressure, lv_color_hex(0xFFD166), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(ui_ChartOilPressure, 1, LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(ui_ChartOilPressure, lv_color_hex(0x242424), LV_PART_TICKS | LV_STATE_DEFAULT);
    lv_chart_set_type(ui_ChartOilPressure, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui_ChartOilPressure, 30);
    lv_chart_set_range(ui_ChartOilPressure, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(ui_ChartOilPressure, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(ui_ChartOilPressure, 4, 5);
    lv_chart_set_axis_tick(ui_ChartOilPressure, LV_CHART_AXIS_PRIMARY_X, 0, 0, 0, 0, true, 0);
    lv_chart_set_axis_tick(ui_ChartOilPressure, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 0, 0, true, 0);
    ui_OilPressureChartSeries = lv_chart_add_series(ui_ChartOilPressure, lv_color_hex(0xFFD166), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(ui_ChartOilPressure, ui_OilPressureChartSeries, 0);

    lv_obj_t *ear = lv_img_create(ui_ScreenPageOilPressure);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageOilPressure, ui_event_oil_pressure_background, LV_EVENT_GESTURE, NULL);
}
