/* Production objects/timers/navigation; radio and persistence are controlled. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "export_path/ui.h"
#include "export_path/ui_bluetooth.h"
#include "export_path/ui_tpms.h"
#include "bsp_obd_dsp/tpms_ble_client.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/espnow_link.h"

lv_obj_t *ui_ScreenPageEasterEgg, *ui_ScreenPageBLEScan, *ui_ScreenPageOilPressure;
static tpms_wheel_data_t live_wheels[4];
void tpms_ble_snapshot(tpms_wheel_data_t out[4]) { memcpy(out, live_wheels, sizeof(live_wheels)); }
void tpms_ble_background_pause(bool pause) { (void)pause; }
void ui_ScreenPageOilPressure_screen_init(void) { ui_ScreenPageOilPressure = lv_obj_create(NULL); }
static nvs_user_cfg_t config;
static bool yielded, active, ready = true;
static unsigned starts, stops;
static esp_err_t start_error, save_error;
static tpms_scan_state_t state;
static tpms_scan_result_t devices[2];
static size_t device_count;
static lv_color_t frame[360 * 360];

void ui_helpers_style_screen_bg(lv_obj_t *screen)
{ lv_obj_set_style_bg_color(screen, lv_color_black(), 0); }
lv_obj_t *ui_helpers_create_ring(lv_obj_t *parent, uint8_t width)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 360, 360);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return ring;
}
void ui_ScreenPageBLEScan_screen_init(void) { ui_ScreenPageBLEScan = lv_obj_create(NULL); }
void ui_ScreenPageEasterEgg_screen_init(void) { ui_ScreenPageEasterEgg = lv_obj_create(NULL); }
void _ui_screen_change(lv_obj_t **target, lv_scr_load_anim_t anim, int speed, int delay, void (*init)(void))
{
    (void)anim; (void)speed; (void)delay;
    if (!*target) init();
    lv_scr_load(*target);
}
nvs_user_cfg_t nvs_cfg_read(void) { return config; }
void elm327_ble_yield_scanner(bool value) { yielded = value; }
bool ble_hub_is_ready(void) { return ready; }
bool ble_hub_scan_active(ble_hub_app_t *owner)
{ if (owner) *owner = BLE_HUB_APP_TPMS; return active; }
esp_err_t tpms_ble_scan_start(void)
{
    assert(yielded);
    ++starts;
    if (start_error) return start_error;
    active = true; state = TPMS_SCAN_RUNNING;
    return ESP_OK;
}
void tpms_ble_scan_stop(void) { ++stops; active = false; state = TPMS_SCAN_IDLE; }
tpms_scan_state_t tpms_ble_scan_snapshot(tpms_scan_result_t *out, size_t cap, size_t *count)
{
    assert(cap >= device_count);
    memcpy(out, devices, sizeof(*out) * device_count); *count = device_count;
    return state;
}
bool tpms_ble_is_bound(const char *id)
{ return id && strlen(id) == 6; }
esp_err_t tpms_ble_bind(unsigned wheel, const char *id)
{
    assert(wheel < 4);
    if (save_error) return save_error;
    memcpy(config.tpms_sensor_id[wheel], id, 7);
    memset(&config.tpms[wheel], 0, sizeof(config.tpms[wheel]));
    return ESP_OK;
}
esp_err_t tpms_ble_unbind(unsigned wheel)
{
    assert(wheel < 4);
    if (save_error) return save_error;
    memset(&config.tpms[wheel], 0, sizeof(config.tpms[wheel]));
    memset(config.tpms_sensor_id[wheel], 0, 7);
    return ESP_OK;
}
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *data)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) frame[y * 360 + x] = *data++;
    lv_disp_flush_ready(drv);
}
static void advance(unsigned ms)
{ for (unsigned t = 0; t < ms; t += 10) { lv_tick_inc(10); lv_timer_handler(); } }
static lv_obj_t *find_text_mode(lv_obj_t *root, const char *text, bool clickable)
{
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), text) &&
        (!clickable || lv_obj_has_class(lv_obj_get_parent(root), &lv_btn_class))) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i) {
        lv_obj_t *found = find_text_mode(lv_obj_get_child(root, (int32_t)i), text, clickable);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{ return find_text_mode(root, text, false); }
static void assert_inside_bezel(lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const int xs[] = {area.x1, area.x2}, ys[] = {area.y1, area.y2};
    for (unsigned i = 0; i < 2; ++i)
        for (unsigned j = 0; j < 2; ++j)
            assert((xs[i] - 180) * (xs[i] - 180) +
                   (ys[j] - 180) * (ys[j] - 180) < 171 * 171);
}

static void click(const char *text)
{
    lv_obj_t *label = find_text_mode(lv_scr_act(), text, true);
    assert(label);
    lv_event_send(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
    advance(20);
}
static void capture(const char *name)
{
    const char *dir = getenv("TPMS_UI_CAPTURE_DIR");
    if (!dir) return;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    char path[512]; snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb"); assert(f);
    fprintf(f, "P6\n360 360\n255\n");
    for (size_t i = 0; i < 360u * 360u; ++i) {
        lv_color32_t c = {.full = lv_color_to32(frame[i])};
        unsigned char rgb[] = {c.ch.red, c.ch.green, c.ch.blue};
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    fclose(f);
}
int main(void)
{
    lv_init();
    static lv_color_t draw[360 * 20];
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, draw, NULL, 360 * 20);
    lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res = 360; driver.ver_res = 360; driver.draw_buf = &buffer; driver.flush_cb = flush;
    lv_disp_drv_register(&driver);
    ui_ScreenPageBluetooth_screen_init(); lv_scr_load(ui_ScreenPageBluetooth);
    capture("menu");
    assert(find_text(lv_scr_act(), "ELM327 PAIRING"));
    click("TPMS PAIRING");
    assert(yielded && !active && starts == 0);
    capture("wheels");
    const char *wheels[] = {"FRONT LEFT", "FRONT RIGHT", "REAR LEFT", "REAR RIGHT"};
    for (unsigned i = 0; i < 4; ++i) {
        click(wheels[i]); click("PAIR SENSOR"); advance(800);
        assert(active);
        const char *ids[] = {"18010F", "000ABC", "000000", "FFFFFF"};
        strcpy(devices[0].data.sensor_id, ids[i]);
        devices[0].rssi = -55; memset(devices[0].mac, 0xAB, 6);
        device_count = 1; advance(300);
        assert(!tpms_ble_is_bound(config.tpms_sensor_id[i])); /* discovery never binds automatically */
        if (i == 0) capture("scan");
        /* Row text is exactly the protocol ID: no MAC, RSSI or generic name. */
        lv_obj_t *row = find_text(lv_scr_act(), ids[i]);
        assert(row && !strcmp(lv_label_get_text(row), ids[i]));
        assert(!find_text(lv_scr_act(), "AB:AB:") && !find_text(lv_scr_act(), "dBm"));
        devices[0].rssi = -90; advance(300);
        click(ids[i]);
        assert(!strcmp(config.tpms_sensor_id[i], ids[i]) && !active);
        assert(find_text(lv_scr_act(), "ID SAVED - waiting for broadcasts"));
        if (i == 0) capture("paired");
        click("BACK");
    }
    click("FRONT LEFT"); click("RE-PAIR"); advance(800);
    click("BACK"); assert(!strcmp(config.tpms_sensor_id[0], "18010F") && !active);
    click("RE-PAIR"); advance(800);
    save_error = ESP_ERR_NO_MEM;
    click("FFFFFF"); assert(find_text(lv_scr_act(), "Save failed"));
    assert(!strcmp(config.tpms_sensor_id[0], "18010F"));
    save_error = 0;
    click("BACK"); click("DELETE PAIRING");
    assert(!tpms_ble_is_bound(config.tpms_sensor_id[0]) && !strcmp(config.tpms_sensor_id[1], "000ABC"));
    click("PAIR SENSOR"); advance(800);
    unsigned previous = starts;
    click("SCAN AGAIN"); advance(800); assert(starts > previous && active);
    click("BACK"); click("BACK"); click("BACK");
    assert(lv_scr_act() == ui_ScreenPageBluetooth && !yielded && !active);
    previous = starts; advance(1000); assert(previous == starts);
    config.device_role = ESPNOW_ROLE_SLAVE;
    click("BACK"); lv_scr_load(ui_ScreenPageBluetooth);
    assert(find_text(lv_scr_act(), "GAUGE MASTER PAIRING"));
    click("TPMS PAIRING");
    assert(find_text(lv_scr_act(), "REAR RIGHT  FFFFFF"));
    click("REAR RIGHT");
    start_error = ESP_ERR_INVALID_STATE;
    click("RE-PAIR"); advance(5500);
    assert(find_text(lv_scr_act(), "Scan unavailable"));
    start_error = 0;
    click("SCAN AGAIN"); advance(800); assert(active);
    lv_scr_load(ui_ScreenPageBluetooth);
    assert(!yielded && !active);
    lv_obj_del(ui_ScreenPageTPMSPairing);
    assert(!ui_ScreenPageTPMSPairing && !yielded && stops > 0);
    advance(1000);
    config.tpms[0].mac[0] = 0x18; /* legacy record, no fabricated 180000 ID */
    click("TPMS PAIRING"); click("FRONT LEFT");
    assert(find_text(lv_scr_act(), "LEGACY RECORD"));
    click("DELETE PAIRING"); assert(!config.tpms[0].mac[0]);
    lv_scr_load(ui_ScreenPageBluetooth);
    lv_obj_del(ui_ScreenPageTPMSPairing);
    ui_ScreenPageTPMS_screen_init(); lv_scr_load(ui_ScreenPageTPMS);
    assert(find_text(lv_scr_act(), "-- C   --%"));
    assert(!find_text(lv_scr_act(), "Tap tire for all data"));
    lv_obj_t *title = find_text(lv_scr_act(), "TIRE PRESSURE");
    assert(title && lv_obj_get_style_text_font(title, 0) == &ui_font_FontTypoderSize16);
    assert(lv_obj_get_style_text_color(title, 0).full == lv_color_hex(0x666666).full);
    lv_obj_update_layout(lv_scr_act());
    assert(lv_obj_get_height(title) == ui_font_FontTypoderSize16.line_height);
    /* Check the whole label box, not only its currently illuminated glyphs. */
    assert_inside_bezel(title);
    lv_obj_t *wheel_label = find_text(lv_scr_act(), "FL");
    lv_obj_t *tile = lv_obj_get_parent(wheel_label);
    assert(lv_obj_get_style_bg_opa(tile, 0) == LV_OPA_TRANSP);
    assert(lv_obj_get_style_border_width(tile, 0) == 0);
    tpms_wheel_data_t *v = &live_wheels[0];
    memcpy(v->sensor_id, "18010F", 7); v->bound = true;
    advance(300); assert(find_text(lv_scr_act(), "-- C   --%"));
    v->received = v->fresh = true;
    v->sample.data = (tpms_fbb0_data_t){.pressure_pa=251520, .temperature_c=25,
        .voltage_mv=3000, .state=0, .version=1, .ble_version_code=10};
    v->sample.rssi = -67;
    advance(300); assert(find_text(lv_scr_act(), "2.52"));
    assert(find_text(lv_scr_act(), "25 C   100%"));
    assert(!find_text(lv_scr_act(), "18010F"));
    assert(!find_text(lv_scr_act(), "NORMAL"));
    v->sample.data.pressure_pa = 210000; advance(300);
    assert(find_text(lv_scr_act(), "2.10"));
    v->sample.data.pressure_pa = 251520; advance(300);
    capture("live-overview");
    const char *ids[] = {"18010F", "000000", "123456", "A1B2C3"};
    for (unsigned w = 0; w < 4; ++w) {
        live_wheels[w] = *v;
        memcpy(live_wheels[w].sensor_id, ids[w], 7);
        live_wheels[w].sample.data.pressure_pa = 210000 + w * 10000;
        live_wheels[w].sample.data.temperature_c = 25 + w;
    }
    advance(300);
    assert(find_text(lv_scr_act(), "2.10")); assert(find_text(lv_scr_act(), "2.20"));
    assert(find_text(lv_scr_act(), "2.30")); assert(find_text(lv_scr_act(), "2.40"));
    for (unsigned w = 0; w < 4; ++w) assert(!find_text(lv_scr_act(), ids[w]));
    const char *metrics[] = {"25 C   100%", "26 C   100%", "27 C   100%", "28 C   100%"};
    const char *values[] = {"2.10", "2.20", "2.30", "2.40"};
    lv_obj_update_layout(lv_scr_act());
    for (unsigned w = 0; w < 4; ++w) {
        assert_inside_bezel(find_text(lv_scr_act(), metrics[w]));
        lv_obj_t *value = find_text(lv_scr_act(), values[w]);
        assert(lv_obj_get_style_text_font(value, 0) == &ui_font_FontTypoderSize40);
        assert(lv_obj_get_style_text_color(value, 0).full == lv_color_white().full);
    }
    capture("four-wheel-overview");
    v->sample.data.pressure_pa = 251520; advance(300);
    click("FL");
    assert(find_text(lv_scr_act(), "2.52 bar  /  251520 Pa"));
    assert(find_text(lv_scr_act(), "State: 0 NORMAL"));
    assert(find_text(lv_scr_act(), "BLE: V1.0"));
    capture("live-details");
    v->fresh = false; v->age_ms = 120000; advance(300);
    assert(find_text(lv_scr_act(), "STALE"));
    capture("stale-details");
    click("BACK"); assert(find_text(lv_scr_act(), "-- C   --%"));
    assert(!find_text(lv_scr_act(), "Tap tire for all data"));
    lv_scr_load(ui_ScreenPageBluetooth);
    lv_obj_del(ui_ScreenPageTPMS); assert(!ui_ScreenPageTPMS);
    advance(1000);
    lv_obj_del(ui_ScreenPageBluetooth);
    lv_obj_del(ui_ScreenPageEasterEgg);
    return 0;
}
