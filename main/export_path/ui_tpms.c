#include "ui_tpms.h"
#include "ui.h"
#include "app_obd_dsp/tpms_theme_source.h"
#include <stdio.h>
#include <string.h>

lv_obj_t *ui_ScreenPageTPMS;
static const char *const short_names[] = {"FL", "FR", "RL", "RR"};
static const char *const names[] = {"FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT"};
static lv_obj_t *s_body, *s_title, *s_values[4], *s_metrics[4], *s_details;
static lv_timer_t *s_timer;
static int s_selected = -1;

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int y, int width, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_width(obj, width);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_white(), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(obj, value);
    return obj;
}

static const char *status(const tpms_wheel_data_t *v)
{
    if (!v->bound) return "NOT PAIRED";
    if (!v->received) return "WAITING";
    return v->fresh ? "LIVE" : "STALE";
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    tpms_wheel_data_t wheels[NVS_TPMS_WHEELS];
    tpms_ble_snapshot(wheels);
    char buf[420];
    if (s_selected < 0) {
        for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w) {
            const tpms_wheel_data_t *v = &wheels[w];
            const tpms_fbb0_data_t *d = &v->sample.data;
            if (v->fresh)
                snprintf(buf, sizeof(buf), "%.2f", d->pressure_pa / 100000.0);
            else
                snprintf(buf, sizeof(buf), "--");
            set_text(s_values[w], buf);
            if (v->fresh)
                snprintf(buf, sizeof(buf), "%d C   %u%%", (int)d->temperature_c,
                         (unsigned)tpms_battery_percent(d->voltage_mv));
            else
                snprintf(buf, sizeof(buf), "-- C   --%%");
            set_text(s_metrics[w], buf);
            lv_obj_set_style_text_color(s_values[w], lv_color_hex(v->fresh ? 0xFFFFFF : 0x888888), 0);
        }
    } else {
        const tpms_wheel_data_t *v = &wheels[s_selected];
        const tpms_scan_result_t *sample = &v->sample;
        const tpms_fbb0_data_t *d = &sample->data;
        if (!v->received) {
            snprintf(buf, sizeof(buf), "ID: %s\n%s\n\nNo received measurements\nPair via version > TPMS\n\nPressure / Temp / Voltage: --\nState / Versions / RSSI: --",
                     v->bound ? v->sensor_id : "------", status(v));
        } else {
            const uint8_t *m = sample->mac;
            snprintf(buf, sizeof(buf),
                     "ID: %s   %s\nLast RX: %lu s ago%s\n%.2f bar  /  %lu Pa\n%d C   /   %.2f V\nState: %u %s\nVersion: %u   BLE: V%.1f\nRSSI: %d dBm   Type: %u\n%02X:%02X:%02X:%02X:%02X:%02X",
                     v->sensor_id, status(v), (unsigned long)(v->age_ms / 1000),
                     v->fresh ? "" : " (cached)", d->pressure_pa / 100000.0, (unsigned long)d->pressure_pa,
                     (int)d->temperature_c, d->voltage_mv / 1000.0, (unsigned)d->state,
                     tpms_state_name(d->state), (unsigned)d->version, d->ble_version_code / 10.0,
                     sample->rssi, (unsigned)sample->addr_type, m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        set_text(s_details, buf);
    }
}

static void render(void);
static void select_wheel(lv_event_t *event)
{
    s_selected = (int)(uintptr_t)lv_event_get_user_data(event);
    render();
}
static void back(lv_event_t *event) { (void)event; s_selected = -1; render(); }

/* Match INFO's unobtrusive grid rather than drawing filled cards. */
static void divider(lv_obj_t *parent, int width, int height, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, width, height);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void render(void)
{
    lv_obj_clean(s_body);
    set_text(s_title, s_selected < 0 ? "TIRE PRESSURE" : names[s_selected]);
    if (s_selected < 0) {
        divider(s_body, 258, 1, 0);
        divider(s_body, 1, 218, -1);
        for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w) {
            lv_obj_t *card = lv_btn_create(s_body);
            lv_obj_remove_style_all(card);
            lv_obj_set_pos(card, 46 + (int)(w % 2) * 136, 74 + (int)(w / 2) * 108);
            lv_obj_set_size(card, 132, 110);
            lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(card, select_wheel, LV_EVENT_CLICKED, (void *)(uintptr_t)w);
            lv_obj_t *wheel = text(card, short_names[w], 0, 120, &ui_font_FontTypoderSize20);
            lv_obj_set_style_text_color(wheel, lv_color_hex(0x44AAFF), 0);
            s_values[w] = text(card, "", 22, 130, &ui_font_FontTypoderSize40);
            lv_obj_t *unit = text(card, "bar", 64, 120, &ui_font_FontTypoderSize16);
            lv_obj_set_style_text_color(unit, lv_color_hex(0x888888), 0);
            s_metrics[w] = text(card, "", 84, 132, &ui_font_FontTypoderSize16);
            lv_obj_set_style_text_color(s_metrics[w], lv_color_hex(0x888888), 0);
        }
    } else {
        s_details = text(s_body, "", 80, 306, &ui_font_FontTypoderSize16);
        lv_obj_set_style_text_line_space(s_details, 9, 0);
        lv_obj_t *button = lv_btn_create(s_body);
        lv_obj_set_size(button, 126, 30);
        lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 292);
        lv_obj_add_event_cb(button, back, LV_EVENT_CLICKED, NULL);
        text(button, "BACK", 0, 110, &ui_font_FontTypoderSize16);
    }
    refresh(NULL);
}

static void gesture(lv_event_t *event)
{
    (void)event;
    lv_indev_t *input = lv_indev_get_act();
    if (!input) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(input);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT && dir != LV_DIR_TOP) return;
    lv_indev_wait_release(input);
    if (s_selected >= 0) { s_selected = -1; render(); return; }
    if (dir == LV_DIR_LEFT)
        _ui_screen_change(&ui_ScreenPageEasterEgg, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, ui_ScreenPageEasterEgg_screen_init);
    else if (dir == LV_DIR_RIGHT)
        _ui_screen_change(&ui_ScreenPageOilPressure, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, ui_ScreenPageOilPressure_screen_init);
}

static void visibility(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_selected = -1; render(); lv_timer_resume(s_timer);
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        lv_timer_pause(s_timer);
    } else if (code == LV_EVENT_DELETE) {
        lv_timer_del(s_timer); s_timer = NULL; ui_ScreenPageTPMS = NULL;
    }
}

void ui_ScreenPageTPMS_screen_init(void)
{
    ui_ScreenPageTPMS = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageTPMS, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ui_ScreenPageTPMS, 0, 0);
    ui_helpers_style_screen_bg(ui_ScreenPageTPMS);
    lv_obj_set_style_radius(ui_ScreenPageTPMS, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageTPMS, 8);
    s_body = lv_obj_create(ui_ScreenPageTPMS);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 360, 360);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_title = text(ui_ScreenPageTPMS, "TIRE PRESSURE", 45, 200, &ui_font_FontTypoderSize16);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0x666666), 0);
    s_selected = -1;
    s_timer = lv_timer_create(refresh, 250, NULL);
    lv_timer_pause(s_timer);
    render();
    lv_obj_move_foreground(ring);
    lv_obj_add_event_cb(ui_ScreenPageTPMS, gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMS, visibility, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMS, visibility, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMS, visibility, LV_EVENT_DELETE, NULL);
}
