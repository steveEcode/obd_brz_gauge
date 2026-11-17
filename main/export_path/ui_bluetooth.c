#include "ui_bluetooth.h"
#include "ui.h"
#include "bsp_obd_dsp/skyhub_client.h"
#include "bsp_obd_dsp/tpms_ble_client.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/espnow_link.h"
#include <stdio.h>
#include <string.h>

lv_obj_t *ui_ScreenPageBluetooth;
lv_obj_t *ui_ScreenPageTPMSPairing;
static const char *const k_wheel[] = {"FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT"};
static lv_obj_t *s_body, *s_status, *s_list;
static lv_obj_t *s_rows[TPMS_SCAN_MAX];
static lv_obj_t *s_elm_label, *s_hub_menu;
static tpms_scan_result_t s_candidates[TPMS_SCAN_MAX];
static size_t s_row_count;
static unsigned s_wheel;
static enum { WHEELS, DETAIL, SCAN } s_view;
static lv_timer_t *s_timer;
static bool s_loaded, s_pending;
static uint32_t s_scan_since;

static void show_wheels(void);
static void show_detail(void);
static void scan_begin(lv_event_t *e);

static lv_obj_t *label(lv_obj_t *parent, const char *text, int y, const lv_font_t *font)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_width(obj, 270);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, int y, int height,
                        lv_event_cb_t cb, uintptr_t arg)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 248, height);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x252525), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_text_font(btn, &ui_font_FontTypoderSize16, 0);
    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, text);
    lv_obj_center(txt);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)arg);
    return btn;
}

static void screen_init(lv_obj_t **screen)
{
    *screen = lv_obj_create(NULL);
    lv_obj_clear_flag(*screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(*screen, 0, 0);
    ui_helpers_style_screen_bg(*screen);
    ui_helpers_create_ring(*screen, 10);
}

static void menu_select(lv_event_t *e)
{
    if ((uintptr_t)lv_event_get_user_data(e) == 0)
        _ui_screen_change(&ui_ScreenPageBLEScan, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                          ui_ScreenPageBLEScan_screen_init);
    else if ((uintptr_t)lv_event_get_user_data(e) == 2) {
        if (nvs_cfg_read().device_role != ESPNOW_ROLE_SLAVE)
            _ui_screen_change(&ui_ScreenPageSkyHub, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageSkyHub_screen_init);
    } else
        _ui_screen_change(&ui_ScreenPageTPMSPairing, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                          ui_ScreenPageTPMSPairing_screen_init);
}

static void menu_back(lv_event_t *e)
{
    (void)e;
    _ui_screen_change(&ui_ScreenPageEasterEgg, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      ui_ScreenPageEasterEgg_screen_init);
}

static void menu_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        lv_indev_wait_release(indev);
        menu_back(e);
    }
}

static void menu_loaded(lv_event_t *e)
{
    (void)e;
    bool slave = nvs_cfg_read().device_role == ESPNOW_ROLE_SLAVE;
    if (slave) lv_obj_add_flag(s_hub_menu, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_hub_menu, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_elm_label, slave ? "GAUGE MASTER PAIRING" : "ELM327 PAIRING");
}

void ui_ScreenPageBluetooth_screen_init(void)
{
    screen_init(&ui_ScreenPageBluetooth);
    label(ui_ScreenPageBluetooth, "CONNECTIONS", 49, &ui_font_FontTypoderSize20);
    lv_obj_t *elm = button(ui_ScreenPageBluetooth, "ELM327 PAIRING", 94, 44, menu_select, 0);
    s_elm_label = lv_obj_get_child(elm, 0);
    lv_obj_add_event_cb(ui_ScreenPageBluetooth, menu_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    button(ui_ScreenPageBluetooth, "TPMS PAIRING", 152, 44, menu_select, 1);
    s_hub_menu = button(ui_ScreenPageBluetooth, "SKY HUB PAIRING", 210, 44, menu_select, 2);
    lv_obj_add_event_cb(ui_ScreenPageBluetooth, menu_gesture, LV_EVENT_GESTURE, NULL);
}

static void stop_scan(void)
{
    s_pending = false;
    tpms_ble_scan_stop();
}

static void clear_body(const char *title)
{
    stop_scan();
    lv_obj_clean(s_body);
    s_status = NULL;
    s_list = NULL;
    s_row_count = 0;
    memset(s_rows, 0, sizeof(s_rows));
    label(s_body, title, 40, &ui_font_FontTypoderSize20);
}

static void back(lv_event_t *e)
{
    (void)e;
    if (s_view == SCAN) show_detail();
    else if (s_view == DETAIL) show_wheels();
    else _ui_screen_change(&ui_ScreenPageBluetooth, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                           ui_ScreenPageBluetooth_screen_init);
}

static void wheel_select(lv_event_t *e)
{
    s_wheel = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    show_detail();
}

static bool legacy_binding(const nvs_tpms_binding_t *binding)
{
    for (unsigned i = 0; i < 6; ++i) if (binding->mac[i]) return true;
    return false;
}

static void show_wheels(void)
{
    s_view = WHEELS;
    clear_body("TPMS PAIRING");
    nvs_user_cfg_t cfg = nvs_cfg_read();
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i) {
        char text[48];
        snprintf(text, sizeof(text), "%s  %s", k_wheel[i],
                 tpms_ble_is_bound(cfg.tpms_sensor_id[i]) ? cfg.tpms_sensor_id[i] :
                 (legacy_binding(&cfg.tpms[i]) ? "RE-PAIR" : "--"));
        button(s_body, text, 90 + (int)i * 43, 36, wheel_select, i);
    }
}

static void remove_binding(lv_event_t *e)
{
    (void)e;
    esp_err_t err = tpms_ble_unbind(s_wheel);
    if (err == ESP_OK) show_detail();
    else lv_label_set_text(s_status, "Save failed. Try again.");
}

static void show_detail(void)
{
    s_view = DETAIL;
    clear_body(k_wheel[s_wheel]);
    nvs_user_cfg_t cfg = nvs_cfg_read();
    const char *id = cfg.tpms_sensor_id[s_wheel];
    bool paired = tpms_ble_is_bound(id);
    bool legacy = legacy_binding(&cfg.tpms[s_wheel]);
    label(s_body, paired ? id : (legacy ? "LEGACY RECORD" : "NOT PAIRED"),
          85, &ui_font_FontTypoderSize20);
    s_status = label(s_body, paired ? "ID SAVED - waiting for broadcasts" :
                     (legacy ? "Re-pair to save sensor ID" : "Select one sensor below"),
                     112, &lv_font_montserrat_12);
    button(s_body, (paired || legacy) ? "RE-PAIR" : "PAIR SENSOR", 153, 40, scan_begin, 0);
    if (paired || legacy) button(s_body, "DELETE PAIRING", 204, 40, remove_binding, 0);
}

static void device_select(lv_event_t *e)
{
    size_t i = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (i >= s_row_count) return;
    esp_err_t err = tpms_ble_bind(s_wheel, s_candidates[i].data.sensor_id);
    if (err == ESP_OK) show_detail();
    else lv_label_set_text(s_status, err == ESP_ERR_INVALID_STATE
                           ? "Already paired to another wheel"
                           : "Save failed. Try again.");
}

static void scan_begin(lv_event_t *e)
{
    (void)e;
    s_view = SCAN;
    clear_body(k_wheel[s_wheel]);
    s_status = label(s_body, "Preparing scan...", 70, &lv_font_montserrat_12);
    s_list = lv_list_create(s_body);
    lv_obj_set_size(s_list, 270, 142);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x111111), 0);
    lv_obj_set_style_pad_all(s_list, 3, 0);
    /* Scrolling the results must not trigger page navigation. */
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    label(s_body, "Tap sensor ID to save this wheel", 244, &lv_font_montserrat_12);
    button(s_body, "SCAN AGAIN", 265, 30, scan_begin, 0);
    s_pending = true;
    s_scan_since = lv_tick_get();
}

static void poll_scan(lv_timer_t *timer)
{
    (void)timer;
    if (!s_loaded || s_view != SCAN) return;
    uint32_t elapsed = lv_tick_elaps(s_scan_since);
    if (s_pending) {
        /* Let any previous controller stop complete before changing params. */
        if (elapsed < 500) return;
        esp_err_t err = tpms_ble_scan_start();
        if (err == ESP_OK) {
            s_pending = false;
            s_scan_since = lv_tick_get();
        } else if (elapsed < 5000 && ble_hub_is_ready()) {
            lv_label_set_text(s_status, "Waiting for Bluetooth scanner...");
            return;
        } else {
            s_pending = false;
            lv_label_set_text(s_status, "Scan unavailable. Tap SCAN AGAIN.");
            return;
        }
    }
    size_t count;
    tpms_scan_state_t state = tpms_ble_scan_snapshot(s_candidates, TPMS_SCAN_MAX, &count);
    for (size_t i = 0; i < count; ++i) {
        const char *text = s_candidates[i].data.sensor_id;
        if (!s_rows[i]) {
            s_rows[i] = lv_list_add_btn(s_list, NULL, text);
            lv_obj_set_style_text_font(s_rows[i], &ui_font_FontTypoderSize20, 0);
            lv_obj_set_style_text_color(s_rows[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(0x252525), 0);
            lv_obj_add_event_cb(s_rows[i], device_select, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        } else if (strcmp(lv_list_get_btn_text(s_list, s_rows[i]), text)) {
            lv_label_set_text(lv_obj_get_child(s_rows[i], 0), text);
        }
    }
    s_row_count = count;
    if (state == TPMS_SCAN_FAILED || !ble_hub_is_ready())
        lv_label_set_text(s_status, "Scan failed. Tap SCAN AGAIN.");
    else if (state == TPMS_SCAN_DONE)
        lv_label_set_text(s_status, count ? "Tap your sensor ID to save" : "No sensors. Wake sensor and retry.");
    else if (state == TPMS_SCAN_RUNNING) {
        /* Hub also releases ownership on an immediate controller start error. */
        if (lv_tick_elaps(s_scan_since) > 1500 && !ble_hub_scan_active(NULL))
            lv_label_set_text(s_status, "Scan stopped. Tap SCAN AGAIN.");
        else lv_label_set_text_fmt(s_status, "Scanning... %u found", (unsigned)count);
    }
}

static void visibility(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_LOADED) {
        s_loaded = true;
        tpms_ble_background_pause(true);
        elm327_ble_yield_scanner(true);
        show_wheels();
        lv_timer_resume(s_timer);
    } else if (code == LV_EVENT_SCREEN_UNLOADED || code == LV_EVENT_DELETE) {
        if (s_loaded) {
            s_loaded = false;
            stop_scan();
            elm327_ble_yield_scanner(false);
            tpms_ble_background_pause(false);
        }
        if (s_timer) lv_timer_pause(s_timer);
        if (code == LV_EVENT_DELETE) {
            if (s_timer) lv_timer_del(s_timer);
            s_timer = NULL;
            ui_ScreenPageTPMSPairing = NULL;
        }
    }
}

static void pairing_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        back(e);
    }
}

void ui_ScreenPageTPMSPairing_screen_init(void)
{
    screen_init(&ui_ScreenPageTPMSPairing);
    s_body = lv_obj_create(ui_ScreenPageTPMSPairing);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 360, 360);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    s_timer = lv_timer_create(poll_scan, 250, NULL);
    lv_timer_pause(s_timer);
    lv_obj_add_event_cb(ui_ScreenPageTPMSPairing, visibility, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMSPairing, visibility, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMSPairing, visibility, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(ui_ScreenPageTPMSPairing, pairing_gesture, LV_EVENT_GESTURE, NULL);
    show_wheels();
}


/* Sky Hub discovery is ESP-NOW, not BLE. Snapshot polling and all LVGL calls
 * remain on the LVGL task; the radio worker handles pairing/NVS asynchronously. */
lv_obj_t *ui_ScreenPageSkyHub;
static lv_obj_t *s_hub_bound, *s_hub_status, *s_hub_fields, *s_hub_list;
static lv_timer_t *s_hub_timer;
static skyhub_candidate_t s_hub_candidates[SKYHUB_SCAN_MAX];
static size_t s_hub_count;
static bool s_hub_loaded;

static void hub_back(lv_event_t *e)
{
    (void)e;
    _ui_screen_change(&ui_ScreenPageBluetooth, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      ui_ScreenPageBluetooth_screen_init);
}
static void hub_scan(lv_event_t *e)
{
    (void)e;
    if (!skyhub_client_scan()) lv_label_set_text(s_hub_status,"RADIO UNAVAILABLE");
}
static void hub_forget(lv_event_t *e)
{
    (void)e;
    if (!skyhub_client_unbind()) lv_label_set_text(s_hub_status,"RADIO UNAVAILABLE");
}
static void hub_select(lv_event_t *e)
{
    size_t i=(size_t)(uintptr_t)lv_event_get_user_data(e);
    if (i<s_hub_count && !s_hub_candidates[i].busy && skyhub_client_bind(s_hub_candidates[i].mac))
        lv_label_set_text(s_hub_status,"PAIRING...");
}
static void hub_poll(lv_timer_t *timer)
{
    (void)timer;
    if (!s_hub_loaded) return;
    skyhub_status_t status; skyhub_client_snapshot(&status);
    char text[100];
    const uint8_t *m=status.bound_mac;
    if (sh_mac_valid(m)) snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);
    else snprintf(text,sizeof(text),"NO SAVED HUB");
    lv_label_set_text(s_hub_bound,text);
    lv_label_set_text(s_hub_status,!status.available ? "RADIO UNAVAILABLE" :
                      status.pairing ? "PAIRING..." : status.scanning ? "SEARCHING..." : status.status);
    snprintf(text,sizeof(text),"%s%s%s", status.active_mask&SH_COOLANT ? "CLT " : "",
             status.active_mask&SH_OIL_TEMP ? "OIL TEMP " : "",
             status.active_mask&SH_OIL_PRESSURE ? "OIL PRESS" : "");
    lv_label_set_text(s_hub_fields,text[0] ? text : "NO HUB DATA");
    if (status.count==s_hub_count && !memcmp(s_hub_candidates,status.candidates,status.count*sizeof(*s_hub_candidates))) return;
    s_hub_count=status.count;
    memcpy(s_hub_candidates,status.candidates,sizeof(s_hub_candidates));
    lv_obj_clean(s_hub_list);
    int row_y=0;
    for (size_t i=0;i<s_hub_count;i++) {
        const skyhub_candidate_t *c=&s_hub_candidates[i]; m=c->mac;
        // A bound Hub is shown as saved/paired; worker refuses a Hub owned by
        // another gauge. Hold its BOOT button 3s to physically release it.
        /* Friendly label only; hub_select still binds the full candidate MAC. */
        snprintf(text,sizeof(text),"SKY HUB %02X%02X%s",m[4],m[5],c->busy ? "\nBUSY" : "");
        int height=c->busy ? 58 : 40;
        lv_obj_t *row=button(s_hub_list,text,row_y,height,hub_select,i);
        row_y+=height+4;
        lv_obj_set_style_text_align(row,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_width(row,248);
        if (c->busy) lv_obj_add_state(row,LV_STATE_DISABLED);
    }
}
static void hub_visibility(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);
    if (code==LV_EVENT_SCREEN_LOADED) {
        s_hub_loaded=true; lv_timer_resume(s_hub_timer); hub_poll(NULL);
        skyhub_client_scan();
    } else if (code==LV_EVENT_SCREEN_UNLOADED || code==LV_EVENT_DELETE) {
        s_hub_loaded=false;
        if (s_hub_timer) lv_timer_pause(s_hub_timer);
        if (code==LV_EVENT_DELETE) {
            if (s_hub_timer) lv_timer_del(s_hub_timer);
            s_hub_timer=NULL; ui_ScreenPageSkyHub=NULL;
        }
    }
}
static void hub_gesture(lv_event_t *e)
{
    lv_indev_t *indev=lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir=lv_indev_get_gesture_dir(indev);
    if (dir==LV_DIR_LEFT || dir==LV_DIR_RIGHT) { lv_indev_wait_release(indev); hub_back(e); }
}
void ui_ScreenPageSkyHub_screen_init(void)
{
    screen_init(&ui_ScreenPageSkyHub);
    lv_obj_t *title=label(ui_ScreenPageSkyHub,"SKY HUB PAIRING",55,&ui_font_FontTypoderSize20);
    lv_obj_set_width(title,LV_SIZE_CONTENT);
    s_hub_bound=label(ui_ScreenPageSkyHub,"NO SAVED HUB",82,&ui_font_FontTypoderSize16);
    s_hub_status=label(ui_ScreenPageSkyHub,"SEARCHING...",108,&ui_font_FontTypoderSize16);
    lv_obj_set_width(s_hub_status,284);
    lv_label_set_long_mode(s_hub_status,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hub_status,LV_TEXT_ALIGN_CENTER,0);
    s_hub_fields=label(ui_ScreenPageSkyHub,"NO HUB DATA",148,&ui_font_FontTypoderSize16);
    s_hub_list=lv_obj_create(ui_ScreenPageSkyHub);
    lv_obj_remove_style_all(s_hub_list); lv_obj_set_size(s_hub_list,270,77);
    lv_obj_align(s_hub_list,LV_ALIGN_TOP_MID,0,174);
    lv_obj_set_scroll_dir(s_hub_list,LV_DIR_VER);
    lv_obj_t *scan=button(ui_ScreenPageSkyHub,"SCAN",263,34,hub_scan,0);
    lv_obj_set_width(scan,112); lv_obj_align(scan,LV_ALIGN_TOP_MID,-62,263);
    lv_obj_t *forget=button(ui_ScreenPageSkyHub,"FORGET",263,34,hub_forget,0);
    lv_obj_set_width(forget,112); lv_obj_align(forget,LV_ALIGN_TOP_MID,62,263);
    s_hub_count=0; memset(s_hub_candidates,0,sizeof(s_hub_candidates)); s_hub_loaded=false;
    s_hub_timer=lv_timer_create(hub_poll,250,NULL); lv_timer_pause(s_hub_timer);
    lv_obj_add_event_cb(ui_ScreenPageSkyHub,hub_visibility,LV_EVENT_SCREEN_LOADED,NULL);
    lv_obj_add_event_cb(ui_ScreenPageSkyHub,hub_visibility,LV_EVENT_SCREEN_UNLOADED,NULL);
    lv_obj_add_event_cb(ui_ScreenPageSkyHub,hub_visibility,LV_EVENT_DELETE,NULL);
    lv_obj_add_event_cb(ui_ScreenPageSkyHub,hub_gesture,LV_EVENT_GESTURE,NULL);
}
