/* Real discovery and persistent wheel binding, deterministic GAP/NVS boundary. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "bsp_obd_dsp/tpms_ble_client.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "freertos/FreeRTOS.h"

static ble_hub_client_t client;
static bool scanning, ready = true;
static uint32_t last_duration;
static ble_hub_app_t owner;
static esp_err_t register_error, scan_error, save_error;
static nvs_user_cfg_t config;
static unsigned saves;

static unsigned lock_depth, device_logs, full_logs;
static int64_t now_us;
static char last_log[768];
void ble_test_enter(portMUX_TYPE *mux)
{
    assert(!mux->locked); mux->locked = true; ++lock_depth;
}
void ble_test_exit(portMUX_TYPE *mux)
{
    assert(mux->locked && lock_depth); mux->locked = false; --lock_depth;
}
int64_t esp_timer_get_time(void) { assert(!lock_depth); return now_us; }
void ble_test_log(const char *tag, const char *fmt, ...)
{
    assert(!lock_depth && !strcmp(tag, "tpms_ble"));
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(last_log, sizeof(last_log), fmt, args);
    va_end(args);
    assert(n > 0 && (size_t)n < sizeof(last_log));
    assert(!strchr(last_log, '\n') && !strchr(last_log, '\r'));
    if (!strncmp(last_log, "sensor=", 7)) ++device_logs;
    if (strstr(last_log, "scan list full")) ++full_logs;
}
bool ble_hub_is_ready(void) { return ready; }
static esp_err_t start_scan_test(void) { now_us += 1000000; return tpms_ble_scan_start(); }
esp_err_t ble_hub_register(ble_hub_app_t app, const ble_hub_client_t *c)
{
    assert(app == BLE_HUB_APP_TPMS && !c->gattc_cb && !c->gatts_cb);
    if (register_error) return register_error;
    client = *c;
    return ESP_OK;
}
bool ble_hub_scan_active(ble_hub_app_t *app)
{
    if (app) *app = owner;
    return scanning;
}
esp_err_t ble_hub_scan_start(ble_hub_app_t app, const esp_ble_scan_params_t *p, uint32_t duration)
{
    assert(app == BLE_HUB_APP_TPMS && (duration == 20 || duration == 0));
    last_duration = duration;
    assert(p->scan_type == BLE_SCAN_TYPE_ACTIVE && p->scan_window <= p->scan_interval);
    if (scan_error) return scan_error;
    assert(!scanning);
    scanning = true; owner = app;
    return ESP_OK;
}
esp_err_t ble_hub_scan_stop(ble_hub_app_t app)
{
    if (scanning && owner == app) scanning = false;
    return ESP_OK;
}
nvs_user_cfg_t nvs_cfg_read(void) { return config; }
esp_err_t nvs_cfg_update(const nvs_user_cfg_t *base, const nvs_user_cfg_t *next)
{
    assert(!memcmp(base, &config, sizeof(config)));
    ++saves;
    if (save_error) return save_error;
    config = *next;
    return ESP_OK;
}
/* These packets are synthetic protocol fixtures, not physical sensor captures.
 * Test CRC uses an independent bit-at-a-time implementation. */
static uint16_t reference_crc(const uint8_t *bytes, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        for (unsigned mask = 0x80; mask; mask >>= 1) {
            bool top = (crc & 0x8000) != 0;
            crc = (uint16_t)(crc << 1);
            if (top != ((bytes[i] & mask) != 0)) crc ^= 0x1021;
        }
    }
    return crc;
}
static void set_crc(uint8_t *raw)
{
    uint8_t input[15]; size_t n = 0;
    for (unsigned i = 9; i < 26; ++i)
        if (i != 16 && i != 18) input[n++] = raw[i];
    assert(n == sizeof(input));
    uint16_t crc = reference_crc(input, sizeof(input));
    raw[16] = (uint8_t)(crc >> 8); raw[18] = (uint8_t)crc;
}
static const uint8_t example[] = {
    0x02,0x01,0x06,0x03,0x03,0xB0,0xFB,0x12,0xFF,0,0,0xB2,0x50,
    0x50,0,1,0xF0,0,0x63,0x0A,0,0,0,0x56,0x34,0x12
};
static esp_ble_gap_cb_param_t packet(unsigned id)
{
    esp_ble_gap_cb_param_t p = {0};
    p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
    p.scan_rst.bda[0] = 0xA0; p.scan_rst.bda[5] = (uint8_t)id;
    p.scan_rst.rssi = -67;
    p.scan_rst.adv_data_len = sizeof(example);
    memcpy(p.scan_rst.ble_adv, example, sizeof(example));
    for (unsigned i = 0; i < 3; ++i) p.scan_rst.ble_adv[23 + i] = (uint8_t)(id >> (8 * i));
    set_crc(p.scan_rst.ble_adv);
    return p;
}
static void emit(esp_ble_gap_cb_param_t *p)
{ client.gap_cb(ESP_GAP_BLE_SCAN_RESULT_EVT, p); }
static size_t snapshot(tpms_scan_result_t *results)
{
    size_t n;
    tpms_ble_scan_snapshot(results, TPMS_SCAN_MAX, &n);
    return n;
}
static void test_protocol(void)
{
    assert(reference_crc((const uint8_t *)"123456789", 9) == 0x31C3);
    tpms_fbb0_data_t data;
    assert(tpms_fbb0_decode(example, sizeof(example), &data));
    assert(!strcmp(data.sensor_id, "123456"));
    assert(data.pressure_pa == 251520 && data.temperature_c == 25 && data.voltage_mv == 3000);
    assert(data.state == 0 && data.version == 1 && data.ble_version_code == 10);
    assert(!tpms_fbb0_decode(NULL, 26, &data));
    assert(!tpms_fbb0_decode(example, sizeof(example), NULL));
    for (size_t n = 0; n < 26; ++n) assert(!tpms_fbb0_decode(example, n, &data));
    uint8_t raw[31];
    for (unsigned i = 0; i < 26; ++i) {
        memcpy(raw, example, sizeof(example)); raw[i] ^= 1;
        /* bytes 7 and 8 are not constrained by the recovered path. */
        bool accepted = tpms_fbb0_decode(raw, sizeof(example), &data);
        assert(accepted == (i == 7 || i == 8));
    }
    memcpy(raw, example, sizeof(example)); memset(raw + 26, 0xA5, 5);
    assert(tpms_fbb0_decode(raw, sizeof(raw), &data));
    esp_ble_gap_cb_param_t p = packet(0x18010F);
    assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data));
    assert(!strcmp(data.sensor_id, "18010F"));
    p = packet(0); assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data));
    assert(!strcmp(data.sensor_id, "000000"));
    p = packet(0x000ABC); assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data));
    assert(!strcmp(data.sensor_id, "000ABC"));
    for (unsigned flag = 0; flag < 3; ++flag) {
        p.scan_rst.ble_adv[17] = (uint8_t)flag; set_crc(p.scan_rst.ble_adv);
        assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data));
        assert(data.pressure_pa == (80u + (flag == 1 ? 256u : 0u)) * 3144u);
    }
    p.scan_rst.ble_adv[13] = 0; p.scan_rst.ble_adv[14] = 255; set_crc(p.scan_rst.ble_adv);
    assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data));
    assert(data.temperature_c == -55 && data.state == 255);
    p.scan_rst.ble_adv[13] = 255; set_crc(p.scan_rst.ble_adv);
    assert(tpms_fbb0_decode(p.scan_rst.ble_adv, 26, &data) && data.temperature_c == 200);
    const char *bad[] = {NULL,"","12345","1234567","18g10F","18010f"," 18010F"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); ++i)
        assert(!tpms_ble_is_bound(bad[i]));
    assert(tpms_ble_is_bound("000000") && tpms_ble_is_bound("18010F"));
}
static void test_scanning(void)
{
    register_error = ESP_ERR_NO_MEM;
    assert(start_scan_test() == ESP_ERR_NO_MEM);
    register_error = 0;
    scanning = true; owner = BLE_HUB_APP_ELM327;
    assert(start_scan_test() == ESP_ERR_INVALID_STATE);
    tpms_ble_scan_stop(); assert(scanning);
    scanning = false; scan_error = ESP_ERR_INVALID_STATE;
    assert(start_scan_test() == scan_error);
    assert(tpms_ble_scan_snapshot(NULL, 0, NULL) == TPMS_SCAN_FAILED);
    scan_error = 0;
    assert(start_scan_test() == ESP_OK);
    tpms_scan_result_t results[TPMS_SCAN_MAX];
    for (unsigned i = 0; i < 1000; ++i) {
        esp_ble_gap_cb_param_t p = packet(i);
        p.scan_rst.ble_adv[5] = 0; emit(&p); /* unrelated advertisements never consume slots */
    }
    assert(snapshot(results) == 0);
    esp_ble_gap_cb_param_t p = packet(0x18010F); emit(&p);
    assert(snapshot(results) == 1 && !strcmp(results[0].data.sensor_id, "18010F"));
    memset(p.scan_rst.bda, 0xBB, 6); p.scan_rst.ble_addr_type = 1; emit(&p);
    assert(snapshot(results) == 1 && results[0].mac[0] == 0xBB); /* MAC rotation same ID */
    p = packet(2); memset(p.scan_rst.bda, 0xBB, 6); emit(&p);
    assert(snapshot(results) == 2); /* same MAC different ID */
    p = packet(3); p.scan_rst.ble_adv[12] ^= 1; emit(&p);
    assert(snapshot(results) == 2); /* invalid CRC rejected */
    p = packet(3); p.scan_rst.adv_data_len = 255; emit(&p);
    p = packet(3); p.scan_rst.scan_rsp_len = 255; emit(&p);
    assert(snapshot(results) == 2);
    /* Split records must not be concatenated; independent complete RSP is safe. */
    p = packet(3); p.scan_rst.adv_data_len = 13; p.scan_rst.scan_rsp_len = 13; emit(&p);
    assert(snapshot(results) == 2);
    p = packet(3); memmove(p.scan_rst.ble_adv + 3, p.scan_rst.ble_adv, 26);
    memset(p.scan_rst.ble_adv, 0, 3); p.scan_rst.adv_data_len = 3; p.scan_rst.scan_rsp_len = 26; emit(&p);
    assert(snapshot(results) == 3);
    for (unsigned i = 4; i < 50; ++i) { p = packet(i); emit(&p); }
    assert(snapshot(results) == TPMS_SCAN_MAX && full_logs == 1);
    p = packet(0x18010F); p.scan_rst.ble_adv[12] = 99; set_crc(p.scan_rst.ble_adv); emit(&p);
    assert(snapshot(results) == TPMS_SCAN_MAX && results[0].data.pressure_pa == 99u * 3144);
    size_t n; tpms_ble_scan_snapshot(results, 1, &n); assert(n == 1);
    memset(&p, 0, sizeof(p)); p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
    scanning = false; emit(&p);
    assert(tpms_ble_scan_snapshot(NULL, 0, NULL) == TPMS_SCAN_DONE);
    assert(strstr(last_log, "scan done status=0 devices=20"));
    p = packet(4); unsigned logged = device_logs; emit(&p); assert(device_logs == logged);
    assert(start_scan_test() == ESP_OK); tpms_ble_scan_stop();
    emit(&p); assert(snapshot(results) == 0);
    assert(start_scan_test() == ESP_OK);
    p.scan_start_cmpl.status = 1;
    client.gap_cb(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    assert(tpms_ble_scan_snapshot(NULL, 0, NULL) == TPMS_SCAN_FAILED);
    scanning = false;
    assert(start_scan_test() == ESP_OK); client.shutdown_cb();
    assert(tpms_ble_scan_snapshot(NULL, 0, NULL) == TPMS_SCAN_FAILED);
    scanning = false;
}
static void test_diagnostic_logs(void)
{
    device_logs = 0;
    assert(start_scan_test() == ESP_OK);
    esp_ble_gap_cb_param_t p = packet(0x18010F); emit(&p);
    assert(device_logs == 1 && strstr(last_log, "sensor=18010F device=A0:00:00:00:00:0F"));
    assert(strstr(last_log, "pressure_pa=251520 temp_c=25 voltage_mv=3000 state=0"));
    now_us += 1000; emit(&p); assert(device_logs == 1);
    p.scan_rst.scan_rsp_len = 3; emit(&p); assert(device_logs == 2);
    memset(p.scan_rst.bda, 0xCC, 6); emit(&p); assert(device_logs == 2);
    now_us += 4999999; emit(&p); assert(device_logs == 2);
    ++now_us; emit(&p); assert(device_logs == 3);
    p.scan_rst.adv_data_len = p.scan_rst.scan_rsp_len = 31;
    memset(p.scan_rst.ble_adv + 26, 0xAB, 5); memset(p.scan_rst.ble_adv + 31, 0xCD, 31);
    now_us += 5000000; emit(&p);
    assert(device_logs == 4 && strstr(last_log, "ABABABABAB RSP[31]=CDCDCD"));
    tpms_ble_scan_stop(); emit(&p); assert(device_logs == 4);
    assert(start_scan_test() == ESP_OK); emit(&p); assert(device_logs == 5);
    tpms_ble_scan_stop();
}
static void test_binding(void)
{
    _Static_assert(offsetof(nvs_user_cfg_t, tpms) == 96, "legacy config boundary");
    _Static_assert(sizeof(nvs_tpms_binding_t) == 39, "legacy record must not grow");
    _Static_assert(offsetof(nvs_user_cfg_t, tpms_sensor_id) == 252, "old config complete size");
    nvs_user_cfg_t migrated = {0}; memset(&migrated, 0xA5, 252);
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i)
        assert(!tpms_ble_is_bound(migrated.tpms_sensor_id[i]));
    /* Old MAC/name cannot manufacture a protocol identity. */
    config = migrated;
    assert(tpms_ble_bind(0, NULL) == ESP_ERR_INVALID_ARG);
    assert(tpms_ble_bind(4, "18010F") == ESP_ERR_INVALID_ARG);
    const char *ids[] = {"18010F", "000ABC", "000000", "FFFFFF"};
    for (unsigned i = 0; i < 4; ++i) {
        assert(tpms_ble_bind(i, ids[i]) == ESP_OK);
        assert(!strcmp(config.tpms_sensor_id[i], ids[i]));
        assert(!config.tpms[i].mac[0]);
    }
    assert(saves == 4);
    assert(tpms_ble_bind(1, "18010F") == ESP_ERR_INVALID_STATE);
    nvs_user_cfg_t saved = config;
    save_error = ESP_ERR_NO_MEM;
    assert(tpms_ble_bind(0, "123456") == save_error);
    assert(!memcmp(&config, &saved, sizeof(config)));
    assert(tpms_ble_unbind(0) == save_error);
    assert(!memcmp(&config, &saved, sizeof(config)));
    save_error = 0;
    assert(tpms_ble_bind(0, "123456") == ESP_OK);
    assert(!strcmp(config.tpms_sensor_id[0], "123456"));
    assert(tpms_ble_unbind(2) == ESP_OK);
    assert(!tpms_ble_is_bound(config.tpms_sensor_id[2]));
    assert(!strcmp(config.tpms_sensor_id[1], saved.tpms_sensor_id[1]));
    assert(!strcmp(config.tpms_sensor_id[3], saved.tpms_sensor_id[3]));
    assert(tpms_ble_unbind(4) == ESP_ERR_INVALID_ARG);
}
static void test_live_monitor(void)
{
    memset(&config, 0, sizeof(config));
    tpms_ble_background_pause(false);
    now_us += 1000000;
    tpms_ble_service_step(true); assert(!scanning); /* no saved ID */
    assert(tpms_ble_bind(0, "18010F") == ESP_OK);
    tpms_wheel_data_t wheels[4];
    tpms_ble_snapshot(wheels);
    assert(wheels[0].bound && !wheels[0].received && !wheels[0].fresh);
    tpms_ble_service_step(false); assert(!scanning);
    now_us += 600000; tpms_ble_service_step(true);
    assert(scanning && last_duration == 0);
    /* A full discovery list must not block reception of a bound wheel. */
    for (unsigned i = 0; i < 30; ++i) { esp_ble_gap_cb_param_t p = packet(i); emit(&p); }
    esp_ble_gap_cb_param_t p = packet(0x18010F); emit(&p);
    tpms_ble_snapshot(wheels);
    assert(wheels[0].fresh && wheels[0].received && wheels[0].age_ms == 0);
    assert(wheels[0].sample.data.pressure_pa == 251520);
    now_us += 1000000; p.scan_rst.ble_adv[12] ^= 1; emit(&p);
    tpms_ble_snapshot(wheels); assert(wheels[0].age_ms == 1000);
    now_us += (TPMS_FRESH_MS - 1000) * 1000LL;
    tpms_ble_snapshot(wheels); assert(wheels[0].received && !wheels[0].fresh);
    p = packet(0x18010F); emit(&p);
    tpms_ble_snapshot(wheels); assert(wheels[0].fresh);
    save_error = ESP_ERR_NO_MEM;
    assert(tpms_ble_bind(0, "ABCDEF") == save_error);
    tpms_ble_snapshot(wheels); assert(wheels[0].fresh);
    save_error = ESP_OK;
    assert(tpms_ble_bind(0, "ABCDEF") == ESP_OK);
    tpms_ble_snapshot(wheels); assert(!wheels[0].received);
    p = packet(0x18010F); emit(&p);
    tpms_ble_snapshot(wheels); assert(!wheels[0].received);
    p = packet(0xABCDEF); emit(&p);
    tpms_ble_snapshot(wheels); assert(wheels[0].fresh);
    tpms_ble_background_pause(true); assert(!scanning);
    now_us += 600000; tpms_ble_service_step(true); assert(!scanning);
    tpms_ble_background_pause(false); tpms_ble_service_step(true); assert(!scanning);
    now_us += 600000;
    scanning = true; owner = BLE_HUB_APP_ELM327;
    tpms_ble_service_step(true); assert(scanning && owner == BLE_HUB_APP_ELM327);
    scanning = false; tpms_ble_service_step(true); assert(!scanning);
    now_us += 600000; tpms_ble_service_step(true); assert(scanning && last_duration == 0);
    tpms_ble_service_step(false); assert(!scanning); /* reconnect priority */
    now_us += 600000; tpms_ble_service_step(true); assert(scanning);
    assert(tpms_ble_unbind(0) == ESP_OK);
    tpms_ble_snapshot(wheels); assert(!wheels[0].bound && !wheels[0].received);
    tpms_ble_service_step(true); assert(!scanning);
    assert(tpms_ble_bind(3, "000000") == ESP_OK);
    now_us += 600000; tpms_ble_service_step(true); assert(scanning);
    ready = false; client.shutdown_cb(); scanning = false;
    now_us += 600000; tpms_ble_service_step(true); assert(!scanning);
}
int main(void) { test_protocol(); test_scanning(); test_diagnostic_logs(); test_binding(); test_live_monitor(); return 0; }
