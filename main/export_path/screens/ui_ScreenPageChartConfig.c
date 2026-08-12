// Chart data source selection page (entered by swiping down from the chart page)
//  - The roller lists all displayable data items (boost appears only for turbo vehicle profiles)
//  - Selecting writes NVS chart_source_idx and applies to the chart page immediately
//  - A gesture in any direction returns to the chart page (see ui_event_chart_config_background)

#include "../ui.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "app_obd_dsp/vehicle_profiles.h"

// Selectable chart data sources (disp_item_t values). DISP_ITEM_BOOST=10 is appended only for turbo profiles, DISP_ITEM_AFR=11 for all profiles.
#define CHART_ITEM_BOOST 10
#define CHART_ITEM_AFR   11
static const uint8_t k_chart_sources_base[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, CHART_ITEM_AFR}; // CLT,IAT,OIL,LOD,TPS,RPM,SPD,BAT,OIP,BKT,AFR
#define CHART_BASE_COUNT (sizeof(k_chart_sources_base) / sizeof(k_chart_sources_base[0]))

static uint8_t s_chart_sources[CHART_BASE_COUNT + 1];
static uint8_t s_chart_source_count = 0;
static lv_obj_t *s_roller_chart_src = NULL;

static void build_chart_sources(void)
{
    s_chart_source_count = 0;
    for (uint8_t i = 0; i < CHART_BASE_COUNT; i++) {
        s_chart_sources[s_chart_source_count++] = k_chart_sources_base[i];
    }
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->has_boost) {
        s_chart_sources[s_chart_source_count++] = CHART_ITEM_BOOST;
    }
}

static uint16_t chart_source_to_pos(uint8_t src)
{
    for (uint16_t i = 0; i < s_chart_source_count; i++) {
        if (s_chart_sources[i] == src) return i;
    }
    return 0;
}

static void on_chart_source_changed(lv_event_t *e)
{
    LV_UNUSED(e);
    uint16_t pos = lv_roller_get_selected(s_roller_chart_src);
    if (pos >= s_chart_source_count) pos = 0;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.chart_source_idx = s_chart_sources[pos];
    nvs_cfg_set(&cfg);

    ui_chart_apply_source();   // apply to the chart page immediately (title/color/unit/range)
}

void ui_ScreenPageChartConfig_screen_init(void)
{
    ui_ScreenPageChartConfig = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageChartConfig, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageChartConfig, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageChartConfig);
    lv_obj_set_style_bg_opa(ui_ScreenPageChartConfig, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageChartConfig, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_ScreenPageChartConfig, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageChartConfig, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(ui_ScreenPageChartConfig);
    lv_label_set_text(title, "CHART SOURCE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    build_chart_sources();
    char options[176] = {0};
    for (uint16_t i = 0; i < s_chart_source_count; i++) {
        if (i > 0) strlcat(options, "\n", sizeof(options));
        strlcat(options, ui_disp_item_name(s_chart_sources[i]), sizeof(options));
    }

    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    s_roller_chart_src = lv_roller_create(ui_ScreenPageChartConfig);
    lv_obj_clear_flag(s_roller_chart_src, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(s_roller_chart_src, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_chart_src, 3);
    lv_roller_set_selected(s_roller_chart_src, chart_source_to_pos(cfg->chart_source_idx), LV_ANIM_OFF);
    lv_obj_set_width(s_roller_chart_src, 160);
    ui_helpers_style_dark_roller(s_roller_chart_src, &ui_font_FontTypoderSize24);
    lv_obj_align(s_roller_chart_src, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_event_cb(s_roller_chart_src, on_chart_source_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageChartConfig);
    lv_label_set_text(hint, "Swipe to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 120);

    lv_obj_add_event_cb(ui_ScreenPageChartConfig, ui_event_chart_config_background, LV_EVENT_GESTURE, NULL);
}
