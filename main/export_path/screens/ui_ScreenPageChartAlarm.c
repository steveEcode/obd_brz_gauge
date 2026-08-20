// Chart alarm threshold settings page (entered by swiping up from the chart page)
//  - Sets the alarm threshold for the data item currently shown on the chart (independent per item, stored in NVS chart_alarm)
//  - Slider range = the item's natural range; pulling to the max step = OFF (alarm disabled)
//  - When value >= threshold, that item's value turns red on all pages (see disp_item_set_value_color)
//  - A gesture in any direction returns to the chart page (see ui_event_chart_alarm_background)

#include "../ui.h"
#include "bsp_obd_dsp/nvs_storage.h"

#define CHART_ALARM_OFF 32767   // convention with nvs: 32767 = off

static lv_obj_t *s_alarm_slider = NULL;
static lv_obj_t *s_alarm_value  = NULL;
static uint8_t  s_alarm_item = 0;
static int32_t  s_alarm_nmin = 0, s_alarm_nmax = 100, s_alarm_div = 1;

static void alarm_update_value_label(int32_t sv)
{
    if (!s_alarm_value) return;
    if (sv > s_alarm_nmax) {
        lv_label_set_text(s_alarm_value, "OFF");
    } else {
        lv_label_set_text_fmt(s_alarm_value, "%ld %s", (long)sv, ui_disp_item_unit(s_alarm_item));
    }
}

static void on_alarm_slider_change(lv_event_t *e)
{
    LV_UNUSED(e);
    int32_t sv = lv_slider_get_value(s_alarm_slider);
    alarm_update_value_label(sv);
    int16_t raw = (sv > s_alarm_nmax) ? CHART_ALARM_OFF : (int16_t)(sv * s_alarm_div);
    nvs_chart_alarm_set(s_alarm_item, raw);
}

void ui_ScreenPageChartAlarm_screen_init(void)
{
    s_alarm_item = nvs_cfg_get()->chart_source_idx;
    ui_disp_item_range(s_alarm_item, &s_alarm_nmin, &s_alarm_nmax, &s_alarm_div);
    if (s_alarm_div < 1) s_alarm_div = 1;

    ui_ScreenPageChartAlarm = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageChartAlarm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageChartAlarm, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageChartAlarm);
    lv_obj_set_style_bg_opa(ui_ScreenPageChartAlarm, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageChartAlarm, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ui_ScreenPageChartAlarm, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageChartAlarm, 0, LV_PART_MAIN);

    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageChartAlarm, 10);   // white ring: static circular border, consistent with the other pages

    // Title: "<item> ALARM"
    lv_obj_t *title = lv_label_create(ui_ScreenPageChartAlarm);
    lv_label_set_text_fmt(title, "%s ALARM", ui_disp_item_name(s_alarm_item));
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(ui_disp_item_color(s_alarm_item)), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -96);

    // Current threshold value
    s_alarm_value = lv_label_create(ui_ScreenPageChartAlarm);
    lv_obj_set_style_text_font(s_alarm_value, &ui_font_FontTypoderSize36, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_alarm_value, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_alarm_value, LV_ALIGN_CENTER, 0, -30);

    // Slider: range = [nmin, nmax+1], the nmax+1 step = OFF
    s_alarm_slider = lv_slider_create(ui_ScreenPageChartAlarm);
    lv_obj_set_style_clip_corner(s_alarm_slider, true, 0);
    lv_slider_set_range(s_alarm_slider, s_alarm_nmin, s_alarm_nmax + 1);
    int16_t cur = nvs_chart_alarm_get(s_alarm_item);
    int32_t sv = (cur >= CHART_ALARM_OFF) ? (s_alarm_nmax + 1) : ((int32_t)cur / s_alarm_div);
    if (sv < s_alarm_nmin) sv = s_alarm_nmin;
    if (sv > s_alarm_nmax + 1) sv = s_alarm_nmax + 1;
    lv_slider_set_value(s_alarm_slider, sv, LV_ANIM_OFF);
    lv_obj_set_width(s_alarm_slider, 200);
    lv_obj_set_height(s_alarm_slider, 10);
    lv_obj_align(s_alarm_slider, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(s_alarm_slider, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_alarm_slider, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_alarm_slider, lv_color_hex(ui_disp_item_color(s_alarm_item)), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_alarm_slider, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_alarm_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_alarm_slider, 5, LV_PART_KNOB);
    lv_obj_clear_flag(s_alarm_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);   // dragging must not trigger the page back gesture
    lv_obj_add_event_cb(s_alarm_slider, on_alarm_slider_change, LV_EVENT_VALUE_CHANGED, NULL);
    alarm_update_value_label(sv);

    lv_obj_t *hint = lv_label_create(ui_ScreenPageChartAlarm);
    lv_label_set_text(hint, "Max = OFF   Swipe to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 110);

    lv_obj_move_foreground(ring);   // bring the ring to the front
    lv_obj_add_event_cb(ui_ScreenPageChartAlarm, ui_event_chart_alarm_background, LV_EVENT_GESTURE, NULL);
}
