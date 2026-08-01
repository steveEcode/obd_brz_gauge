// 通用曲线页 (原机油压力页改造): 数据项由 NVS chart_source_idx 决定
//  - 下滑 → 数据源选择页; 标题/圆点/单位/颜色/量程由 ui_chart_apply_source() 按数据项设置

#include "../ui.h"

lv_obj_t *ui_LabelOilPressureText = NULL;
lv_obj_t *ui_ChartOilPressure = NULL;
lv_chart_series_t *ui_OilPressureChartSeries = NULL;
lv_obj_t *ui_LabelChartTitle = NULL;
lv_obj_t *ui_ChartDot = NULL;
lv_obj_t *ui_LabelChartUnit = NULL;

void ui_ScreenPageOilPressure_screen_init(void)
{
    ui_ScreenPageOilPressure = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageOilPressure, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageOilPressure, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageOilPressure, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageOilPressure, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_ScreenPageOilPressure, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageOilPressure, 8);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口

    ui_LabelChartTitle = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(ui_LabelChartTitle, "");        // 文本/颜色由 ui_chart_apply_source() 设置
    lv_obj_set_style_text_font(ui_LabelChartTitle, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_align(ui_LabelChartTitle, LV_ALIGN_CENTER, 0, -122);

    ui_ChartDot = lv_obj_create(ui_ScreenPageOilPressure);
    lv_obj_remove_style_all(ui_ChartDot);
    lv_obj_set_size(ui_ChartDot, 10, 10);
    lv_obj_set_style_radius(ui_ChartDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ChartDot, 255, LV_PART_MAIN);   // 颜色由 apply 设置
    lv_obj_align(ui_ChartDot, LV_ALIGN_CENTER, -106, -22);

    // 与刹车温页统一布局: 数值 Size28 + 单位同行, 曲线上移到 +62
    ui_LabelOilPressureText = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(ui_LabelOilPressureText, "--.-");
    lv_obj_set_style_text_font(ui_LabelOilPressureText, &ui_font_FontTypoderSize36, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelOilPressureText, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(ui_LabelOilPressureText, 172);
    lv_obj_set_style_text_align(ui_LabelOilPressureText, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_LabelOilPressureText, LV_ALIGN_CENTER, -10, -24);

    ui_LabelChartUnit = lv_label_create(ui_ScreenPageOilPressure);
    lv_label_set_text(ui_LabelChartUnit, "");         // 文本由 ui_chart_apply_source() 设置
    lv_obj_set_style_text_font(ui_LabelChartUnit, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_LabelChartUnit, lv_color_hex(0x999999), LV_PART_MAIN);
    lv_obj_align(ui_LabelChartUnit, LV_ALIGN_CENTER, 88, -26);

    lv_obj_t *trend_panel = lv_obj_create(ui_ScreenPageOilPressure);
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

    ui_chart_apply_source();   // 按当前数据项设置标题/圆点/单位/颜色/Y量程

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageOilPressure, ui_event_oil_pressure_background, LV_EVENT_GESTURE, NULL);
}
