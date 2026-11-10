#include "bsp_obd_dsp/tpms_ble_client.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tpms_scan_result_t s_results[TPMS_SCAN_MAX];
static size_t s_count;
static tpms_scan_state_t s_state;
static bool s_registered;
static bool s_background, s_paused;
static int64_t s_retry_us;
static struct {
    char id[7];
    bool received;
    int64_t received_us;
    tpms_scan_result_t sample;
} s_wheels[NVS_TPMS_WHEELS];

static void sync_bindings(void)
{
    nvs_user_cfg_t cfg = nvs_cfg_read();
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i) {
        if (memcmp(s_wheels[i].id, cfg.tpms_sensor_id[i], 7)) {
            memset(&s_wheels[i], 0, sizeof(s_wheels[i]));
            memcpy(s_wheels[i].id, cfg.tpms_sensor_id[i], 7);
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

void tpms_ble_snapshot(tpms_wheel_data_t out[NVS_TPMS_WHEELS])
{
    nvs_user_cfg_t cfg = nvs_cfg_read();
    int64_t now = esp_timer_get_time();
    memset(out, 0, sizeof(*out) * NVS_TPMS_WHEELS);
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i) {
        memcpy(out[i].sensor_id, cfg.tpms_sensor_id[i], 7);
        out[i].bound = tpms_fbb0_id_valid(out[i].sensor_id);
        if (!out[i].bound || memcmp(out[i].sensor_id, s_wheels[i].id, 7) ||
            !s_wheels[i].received) continue;
        out[i].received = true;
        int64_t age = (now - s_wheels[i].received_us) / 1000;
        out[i].age_ms = age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
        out[i].fresh = age < TPMS_FRESH_MS;
        out[i].sample = s_wheels[i].sample;
    }
    portEXIT_CRITICAL(&s_lock);
}
static const char *TAG = "tpms_ble";
#define TPMS_LOG_INTERVAL_US (5LL * 1000 * 1000)
static struct {
    int64_t last_us;
    bool logged;
    bool response_logged;
} s_log[TPMS_SCAN_MAX];
static bool s_full_logged;
static const esp_ble_scan_params_t k_scan = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50, .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

bool tpms_ble_is_bound(const char *id)
{
    return tpms_fbb0_id_valid(id);
}

/* Legacy ADV and scan response are separate, at most 31-byte streams. */
static void hex_payload(const uint8_t *data, size_t len, char out[63])
{
    static const char hex[] = "0123456789ABCDEF";
    if (!len) { strcpy(out, "-"); return; }
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = hex[data[i] >> 4];
        out[2 * i + 1] = hex[data[i] & 15];
    }
    out[2 * len] = '\0';
}

static void log_advertisement(const tpms_scan_result_t *result,
                              const esp_ble_gap_cb_param_t *p)
{
    char adv[63], rsp[63];
    const unsigned adv_len = p->scan_rst.adv_data_len;
    const unsigned rsp_len = p->scan_rst.scan_rsp_len;
    if (adv_len <= 31 && rsp_len <= 31) {
        hex_payload(p->scan_rst.ble_adv, adv_len, adv);
        hex_payload(p->scan_rst.ble_adv + adv_len, rsp_len, rsp);
    } else {
        /* Never use an invalid length as the scan-response offset. */
        strcpy(adv, "INVALID");
        strcpy(rsp, "INVALID");
    }
    const uint8_t *mac = result->mac;
    ESP_LOGI(TAG, "sensor=%s device=%02X:%02X:%02X:%02X:%02X:%02X type=%u rssi=%d "
             "pressure_pa=%lu temp_c=%d voltage_mv=%u state=%u version=%u ble_ver=%u "
             "ADV[%u]=%s RSP[%u]=%s",
             result->data.sensor_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned)result->addr_type, result->rssi,
             (unsigned long)result->data.pressure_pa, (int)result->data.temperature_c,
             (unsigned)result->data.voltage_mv, (unsigned)result->data.state,
             (unsigned)result->data.version, (unsigned)result->data.ble_version_code,
             adv_len, adv, rsp_len, rsp);
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    const int64_t now = esp_timer_get_time();
    bool log_device = false, log_full = false;
    const char *scan_event = NULL;
    unsigned status = 0;
    size_t count;
    tpms_scan_result_t result = {0};
    tpms_fbb0_data_t decoded;
    bool valid = false;
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT &&
        p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT &&
        p->scan_rst.adv_data_len <= 31 && p->scan_rst.scan_rsp_len <= 31) {
        valid = tpms_fbb0_decode(p->scan_rst.ble_adv, p->scan_rst.adv_data_len, &decoded);
        /* Never concatenate ADV and RSP to satisfy raw offsets. */
        if (!valid)
            valid = tpms_fbb0_decode(p->scan_rst.ble_adv + p->scan_rst.adv_data_len,
                                     p->scan_rst.scan_rsp_len, &decoded);
    }
    portENTER_CRITICAL(&s_lock);
    if (s_state != TPMS_SCAN_RUNNING) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if ((event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT &&
         p->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) ||
        (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT &&
         p->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS)) {
        s_state = TPMS_SCAN_FAILED;
        scan_event = "failed";
        status = event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT ?
                 p->scan_start_cmpl.status : p->scan_param_cmpl.status;
    } else if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s_state = TPMS_SCAN_DONE;
            scan_event = "done";
        } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT && valid) {
            /* Bound-wheel reception is independent of the discovery list cap. */
            for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w) {
                if (memcmp(s_wheels[w].id, decoded.sensor_id, 7)) continue;
                s_wheels[w].received = true;
                s_wheels[w].received_us = now;
                s_wheels[w].sample.data = decoded;
                memcpy(s_wheels[w].sample.mac, p->scan_rst.bda, 6);
                s_wheels[w].sample.addr_type = p->scan_rst.ble_addr_type;
                s_wheels[w].sample.rssi = p->scan_rst.rssi;
            }
            size_t i;
            for (i = 0; i < s_count; ++i) {
                if (!strcmp(s_results[i].data.sensor_id, decoded.sensor_id)) break;
            }
            if (i < TPMS_SCAN_MAX) {
                if (i == s_count) {
                    memset(&s_results[i], 0, sizeof(s_results[i]));
                    ++s_count;
                }
                s_results[i].data = decoded;
                memcpy(s_results[i].mac, p->scan_rst.bda, 6);
                s_results[i].addr_type = p->scan_rst.ble_addr_type;
                s_results[i].rssi = p->scan_rst.rssi;
                const bool valid_response = p->scan_rst.adv_data_len <= 31 &&
                    p->scan_rst.scan_rsp_len > 0 && p->scan_rst.scan_rsp_len <= 31;
                if (!s_log[i].logged || (valid_response && !s_log[i].response_logged) ||
                    now - s_log[i].last_us >= TPMS_LOG_INTERVAL_US) {
                    s_log[i].logged = true;
                    s_log[i].last_us = now;
                    s_log[i].response_logged |= valid_response;
                    result = s_results[i];
                    log_device = true;
                }
            } else if (!s_full_logged) {
                s_full_logged = true;
                log_full = true;
            }
        }
    }
    count = s_count;
    portEXIT_CRITICAL(&s_lock);
    /* No formatting or log I/O while holding the shared spinlock. */
    if (log_device) log_advertisement(&result, p);
    if (log_full) ESP_LOGW(TAG, "scan list full (%u); additional devices omitted; move closer and rescan",
                           (unsigned)TPMS_SCAN_MAX);
    if (scan_event) ESP_LOGI(TAG, "scan %s status=%u devices=%u",
                            scan_event, status, (unsigned)count);
}

static void shutdown_cb(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_state == TPMS_SCAN_RUNNING;
    s_state = TPMS_SCAN_FAILED;
    portEXIT_CRITICAL(&s_lock);
    if (running) ESP_LOGW(TAG, "scan interrupted: BLE shutdown");
}

static esp_err_t scan_start(bool background)
{
    sync_bindings();
    if (!s_registered) {
        const ble_hub_client_t client = {.gap_cb = gap_cb, .shutdown_cb = shutdown_cb};
        esp_err_t err = ble_hub_register(BLE_HUB_APP_TPMS, &client);
        if (err != ESP_OK) return err;
        s_registered = true;
    }
    ble_hub_app_t owner;
    if (ble_hub_scan_active(&owner)) {
        if (owner != BLE_HUB_APP_TPMS) return ESP_ERR_INVALID_STATE;
        if (s_background == background) return ESP_OK;
        tpms_ble_scan_stop();
        return ESP_ERR_INVALID_STATE; /* caller retries after controller stop */
    }
    if (esp_timer_get_time() < s_retry_us) return ESP_ERR_INVALID_STATE;
    s_background = background;
    portENTER_CRITICAL(&s_lock);
    s_count = 0;
    memset(s_log, 0, sizeof(s_log));
    s_full_logged = false;
    s_state = TPMS_SCAN_RUNNING;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "scan requested duration=%us limit=%u; FBB0 + CRC only; identity=sensor ID",
             background ? 0u : 20u, (unsigned)TPMS_SCAN_MAX);
    esp_err_t err = ble_hub_scan_start(BLE_HUB_APP_TPMS, &k_scan, background ? 0 : 20);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_state = TPMS_SCAN_FAILED;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "scan request failed err=%d", (int)err);
    }
    return err;
}

void tpms_ble_scan_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    bool running = s_state == TPMS_SCAN_RUNNING;
    s_state = TPMS_SCAN_IDLE;
    portEXIT_CRITICAL(&s_lock);
    if (running) ESP_LOGI(TAG, "scan stopped");
    ble_hub_scan_stop(BLE_HUB_APP_TPMS);
    s_background = false;
    s_retry_us = esp_timer_get_time() + 500000;
}

esp_err_t tpms_ble_scan_start(void) { return scan_start(false); }

void tpms_ble_background_pause(bool pause)
{
    s_paused = pause;
    if (pause && s_background) tpms_ble_scan_stop();
    /* Also wait for an external owner's asynchronous stop when returning. */
    s_retry_us = esp_timer_get_time() + 500000;
}

void tpms_ble_service_step(bool allow_background)
{
    sync_bindings();
    if (!allow_background || s_paused || !ble_hub_is_ready()) {
        if (s_background) tpms_ble_scan_stop();
        s_retry_us = esp_timer_get_time() + 500000;
        return;
    }
    nvs_user_cfg_t cfg = nvs_cfg_read();
    bool any = false;
    for (unsigned w = 0; w < NVS_TPMS_WHEELS; ++w)
        any |= tpms_fbb0_id_valid(cfg.tpms_sensor_id[w]);
    if (!any) {
        if (s_background) tpms_ble_scan_stop();
        return;
    }
    ble_hub_app_t owner;
    if (ble_hub_scan_active(&owner)) {
        if (owner != BLE_HUB_APP_TPMS) s_retry_us = esp_timer_get_time() + 500000;
        return;
    }
    if (esp_timer_get_time() < s_retry_us) return;
    esp_err_t err = scan_start(true);
    s_retry_us = esp_timer_get_time() + (err == ESP_OK ? 500000 : 2000000);
}

tpms_scan_state_t tpms_ble_scan_snapshot(tpms_scan_result_t *out, size_t cap, size_t *count)
{
    portENTER_CRITICAL(&s_lock);
    size_t n = s_count < cap ? s_count : cap;
    if (out) memcpy(out, s_results, n * sizeof(*out));
    if (count) *count = n;
    tpms_scan_state_t state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

esp_err_t tpms_ble_bind(unsigned wheel, const char *sensor_id)
{
    if (wheel >= NVS_TPMS_WHEELS || !tpms_ble_is_bound(sensor_id))
        return ESP_ERR_INVALID_ARG;
    nvs_user_cfg_t base = nvs_cfg_read(), next = base;
    for (unsigned i = 0; i < NVS_TPMS_WHEELS; ++i) {
        if (i != wheel && !memcmp(base.tpms_sensor_id[i], sensor_id, 7))
            return ESP_ERR_INVALID_STATE;
    }
    memcpy(next.tpms_sensor_id[wheel], sensor_id, 7);
    memset(&next.tpms[wheel], 0, sizeof(next.tpms[wheel]));
    esp_err_t err = nvs_cfg_update(&base, &next);
    if (err == ESP_OK) sync_bindings();
    return err;
}

esp_err_t tpms_ble_unbind(unsigned wheel)
{
    if (wheel >= NVS_TPMS_WHEELS) return ESP_ERR_INVALID_ARG;
    nvs_user_cfg_t base = nvs_cfg_read(), next = base;
    memset(next.tpms_sensor_id[wheel], 0, sizeof(next.tpms_sensor_id[wheel]));
    memset(&next.tpms[wheel], 0, sizeof(next.tpms[wheel]));
    esp_err_t err = nvs_cfg_update(&base, &next);
    if (err == ESP_OK) sync_bindings();
    return err;
}
