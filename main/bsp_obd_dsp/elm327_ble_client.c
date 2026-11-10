/*
 * elm327_ble_client.c - ELM327 BLE transport + the "obd_poll" task that drives
 * vp_engine. Contract: elm327_ble_client.h.
 *
 * Threads:
 *   - Bluedroid task (gap_cb / gattc_cb via ble_hub): scan, connect,
 *     discovery, notify subscription; notifications go into s_rx;
 *   - obd_poll: the only caller of vp_engine (single-threaded state machine:
 *     one command in flight, never interleaved). It also restarts the scan
 *     for a bound adapter after a drop.
 * Shutdown (WiFi OTA): ble_hub_is_ready() turns false; obd_poll stops
 * writing and deletes itself.
 */
#include "app_obd_dsp/perf_stats.h"
#include "app_tasks.h"
#include "bsp_obd_dsp/elm327_ble_client.h"

#include <stdlib.h>
#include <string.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/signal_store.h"
#include "bsp_obd_dsp/ble_adv_util.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "vehicle/vp_engine.h"
#include "vehicle/vp_manager.h"

extern bool ui_showroom_is_active(void);   /* export_path/ui_ext.c */

static const char *TAG = "elm327_ble";

#define UUID16_CCCD          0x2902
#define SVC_COUNT            3

#define RX_BUF_BYTES         1024
#define RX_CHUNK             128
#define RX_DRAIN_MAX         16          /* chunks per loop */
#define TX_LINE_MAX          64
#define TX_CHUNK             20          /* fallback ATT payload when MTU stays at 23 */
#define SCAN_RECONNECT_S     10
#define SCAN_CONNECT_S       15
#define RESCAN_GAP_US        (2LL * 1000 * 1000)
#define OPEN_GUARD_US        (35LL * 1000 * 1000)   /* direct connect gives up after ~30 s */
#define NOTIFY_SETTLE_US     (300LL * 1000)         /* subscription settle before ATZ */
#define RECONNECT_GRACE_US   (5LL * 1000 * 1000)
#define STATS_EVERY_US       (60LL * 1000 * 1000)
#define TX_WRITE_TIMEOUT_MS  1000
#define IDLE_WAIT_MS         300
#define OTA_WAIT_MS          200
#define SHOWROOM_WAIT_MS     500

/* Adapter services in preference order: generic ELM327 (FFF1/FFF2),
 * IOS-Vlink (2AF1/2AF0), Viecar-style (FF15/FF14). */
static const uint16_t k_svc_uuid[SVC_COUNT] = { 0xFFF0, 0x18F0, 0xFF12 };

typedef struct {
    uint16_t start;
    uint16_t end;
    bool     found;
} svc_range_t;

static const esp_ble_scan_params_t k_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x60,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- link (written on the Bluedroid task) ---- */
static volatile esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static volatile uint16_t s_conn_id = 0xFFFF;
static esp_bd_addr_t     s_peer_bda;
static volatile bool     s_connected;
static volatile bool     s_notify_ready;
static volatile bool     s_mtu_ready;
static volatile bool     s_notify_configured;
static volatile bool     s_opening;
static int64_t           s_open_us;
static int64_t           s_ready_us;
static volatile uint16_t s_att_mtu = 23;
static svc_range_t       s_svc[SVC_COUNT];
static uint16_t          s_all_attr_end;
static volatile uint16_t s_write_handle;
static uint16_t          s_notify_handle;
static uint16_t          s_cccd_handle;
static bool               s_notify_indicate;
static volatile esp_gatt_write_type_t s_write_type = ESP_GATT_WRITE_TYPE_RSP;
static StaticSemaphore_t  s_tx_done_storage;
static SemaphoreHandle_t  s_tx_done;
static volatile bool      s_tx_waiting;
static volatile esp_gatt_status_t s_tx_status = ESP_GATT_ERROR;

/* ---- target / modes ---- */
static char              s_target_name[32] = "OBDII";
static esp_bd_addr_t     s_target_bda;
static volatile bool     s_target_valid;
static volatile bool     s_scan_only;
static volatile bool     s_scan_yielded;
static volatile bool     s_paused;
static volatile bool     s_registered;
static int64_t           s_last_scan_us;
static ble_scan_found_cb_t s_scan_cb;
static ble_scan_result_t s_scan_list[BLE_SCAN_MAX_DEVICES];
static int               s_scan_count;

/* ---- poll task ---- */
/* Static wake object outlives the worker and BLE callbacks during shutdown. */
static StaticSemaphore_t     s_poll_wake_storage;
static SemaphoreHandle_t     s_poll_wake;
static bool                  s_task_started;
static StreamBufferHandle_t  s_rx;
static vp_engine_t          *s_engine;
static bool                  s_engine_up;
static uint16_t              s_obd_demand_mask;
static volatile bool         s_quiet;          /* engine must not touch signal_store */
static volatile int64_t      s_reconnect_us;   /* engine asked for a reconnect */
static volatile uint32_t     s_rx_dropped;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static void wake_task(void)
{
    if (s_poll_wake) xSemaphoreGive(s_poll_wake);
}

/* These timestamps are shared with the Bluedroid/UI tasks. ESP32-S3 is a
 * 32-bit target, so volatile alone cannot make an int64_t access atomic. */
static void link_time_set(int64_t *timestamp, int64_t value)
{
    portENTER_CRITICAL(&s_lock);
    *timestamp = value;
    portEXIT_CRITICAL(&s_lock);
}

static int64_t link_time_get(const int64_t *timestamp)
{
    portENTER_CRITICAL(&s_lock);
    int64_t value = *timestamp;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

static bool mac_nonzero(const uint8_t *m)
{
    return m && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

static uint16_t obd_demand_mask_get(void)
{
    uint16_t mask;
    portENTER_CRITICAL(&s_lock);
    mask = s_obd_demand_mask;
    portEXIT_CRITICAL(&s_lock);
    return mask;
}

static bool is_target(const uint8_t *bda)
{
    portENTER_CRITICAL(&s_lock);
    bool eq = s_target_valid && memcmp(bda, s_target_bda, sizeof(esp_bd_addr_t)) == 0;
    portEXIT_CRITICAL(&s_lock);
    return eq;
}

static void set_name(const char *name)
{
    if (!name || !name[0]) return;
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_target_name, name, sizeof(s_target_name));
    portEXIT_CRITICAL(&s_lock);
}

static void bind_target(const uint8_t *mac)
{
    portENTER_CRITICAL(&s_lock);
    if (mac_nonzero(mac)) {
        memcpy(s_target_bda, mac, sizeof(esp_bd_addr_t));
        s_target_valid = true;
    } else {
        s_target_valid = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t scan_start(uint32_t duration_s)
{
    if (!ble_hub_is_ready() || s_scan_yielded) return ESP_ERR_INVALID_STATE;
    link_time_set(&s_last_scan_us, esp_timer_get_time());
    esp_err_t err = ble_hub_scan_start(BLE_HUB_APP_ELM327, &k_scan_params, duration_s);
    if (err != ESP_OK) ESP_LOGD(TAG, "scan start: %s", esp_err_to_name(err));
    return err;
}

static void close_link(void)
{
    esp_gatt_if_t gif = s_gattc_if;
    if (s_connected && gif != ESP_GATT_IF_NONE && ble_hub_is_ready()) {
        esp_ble_gattc_close(gif, s_conn_id);
    }
}

static void reset_link_state(void)
{
    /* Wake a sender blocked on a Write With Response before invalidating the
     * connection state. The sender will observe the failed status below. */
    s_tx_waiting = false;
    s_tx_status = ESP_GATT_ERROR;
    if (s_tx_done) xSemaphoreGive(s_tx_done);
    s_connected = false;
    s_notify_ready = false;
    s_mtu_ready = false;
    s_notify_configured = false;
    s_opening = false;
    s_conn_id = 0xFFFF;
    s_att_mtu = 23;
    memset(s_svc, 0, sizeof(s_svc));
    s_all_attr_end = 0;
    s_write_handle = 0;
    s_notify_handle = 0;
    s_cccd_handle = 0;
    s_notify_indicate = false;
    s_write_type = ESP_GATT_WRITE_TYPE_RSP;
}

/* ------------------------------------------------------------------ */
/* GAP / GATTC (Bluedroid task)                                         */
/* ------------------------------------------------------------------ */

static void scan_collect(const esp_ble_gap_cb_param_t *p, const char *name)
{
    if (!name[0] || s_scan_count >= BLE_SCAN_MAX_DEVICES) return;
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_list[i].name, name) == 0) return;
    }
    ble_scan_result_t *r = &s_scan_list[s_scan_count];
    strlcpy(r->name, name, sizeof(r->name));
    memcpy(r->addr, p->scan_rst.bda, sizeof(r->addr));
    r->rssi = p->scan_rst.rssi;
    s_scan_count++;
    ESP_LOGD(TAG, "scan found [%d]: %s (RSSI %d)", s_scan_count, name, r->rssi);
    ble_scan_found_cb_t cb = s_scan_cb;
    if (cb) cb(r, s_scan_count);
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* ble_hub delivers scan results only while this module owns the scanner */
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) return;

    if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        wake_task();   /* obd_poll rescans for a bound adapter */
        return;
    }
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;

    if (s_scan_only) {
        char name[32] = {0};
        ble_adv_extract_name(param->scan_rst.ble_adv, param->scan_rst.adv_data_len,
                             param->scan_rst.scan_rsp_len, name, sizeof(name));
        scan_collect(param, name);
        return;
    }
    /* exact MAC only: a same-name adapter nearby must never be picked */
    esp_gatt_if_t gif = s_gattc_if;
    if (s_connected || s_opening || s_paused || s_scan_yielded || gif == ESP_GATT_IF_NONE) return;
    if (!is_target(param->scan_rst.bda)) return;

    ble_hub_scan_stop(BLE_HUB_APP_ELM327);
    memcpy(s_peer_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
    link_time_set(&s_open_us, esp_timer_get_time());
    s_opening = true;
    char name[sizeof(s_target_name)];
    elm327_ble_copy_connected_name(name, sizeof(name));
    ESP_LOGI(TAG, "found %s (RSSI %d), connecting", name, param->scan_rst.rssi);
    esp_err_t err = esp_ble_gattc_open(gif, s_peer_bda, param->scan_rst.ble_addr_type, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open: %s", esp_err_to_name(err));
        s_opening = false;
    }
    wake_task();
}

static void link_open(esp_gatt_if_t gattc_if, uint16_t conn_id)
{
    s_conn_id = conn_id;
    s_connected = true;
    s_opening = false;
    s_notify_ready = false;
    s_mtu_ready = false;
    s_notify_configured = false;
    if (s_paused || !is_target(s_peer_bda)) {
        /* unbound / OTA while the connect was pending */
        ESP_LOGI(TAG, "dropping a link that is no longer wanted");
        esp_ble_gattc_close(gattc_if, conn_id);
        return;
    }
    char name[sizeof(s_target_name)];
    elm327_ble_copy_connected_name(name, sizeof(name));
    ESP_LOGI(TAG, "connected to %s", name);
    /* ELM327 BLE clones commonly accept the phone-sized MTU used by Car
     * Scanner. Discovery can run in parallel; writes use the negotiated value
     * once ESP_GATTC_CFG_MTU_EVT arrives and keep the 20-byte fallback before
     * then. */
    esp_err_t mtu_err = esp_ble_gattc_send_mtu_req(gattc_if, conn_id);
    if (mtu_err != ESP_OK) {
        s_mtu_ready = true; /* 23-byte ATT fallback is safe to use now. */
        ESP_LOGW(TAG, "MTU request: %s; using 20-byte payload", esp_err_to_name(mtu_err));
    }
    /* NULL: discover every service (adapters use different UUIDs) */
    esp_ble_gattc_search_service(gattc_if, conn_id, NULL);
    wake_task();
}

static void notify_ready(void)
{
    if (!s_mtu_ready || !s_notify_configured) return;
    link_time_set(&s_ready_us, esp_timer_get_time());
    s_notify_ready = true;
    wake_task();
}

/* After discovery: pick the write / notify characteristics and subscribe. */
static void setup_channel(esp_gatt_if_t gattc_if)
{
    const uint16_t all_start = 0x0001;
    const uint16_t all_end = s_all_attr_end > 0x0001 ? s_all_attr_end : 0xFFFF;
    uint16_t start = all_start;
    uint16_t end = all_end;
    int svc = -1;
    for (int i = 0; i < SVC_COUNT && svc < 0; i++) {
        if (s_svc[i].found) svc = i;
    }
    if (svc >= 0) {
        start = s_svc[svc].start;
        end = s_svc[svc].end;
        ESP_LOGI(TAG, "using service 0x%04X (0x%04X..0x%04X)", k_svc_uuid[svc], start, end);
    } else {
        ESP_LOGW(TAG, "no known adapter service, searching 0x0001..0x%04X", end);
    }

    esp_gatt_status_t st;
    for (int pass = 0; pass < 2; pass++) {
        uint16_t count = 0;
        st = esp_ble_gattc_get_attr_count(gattc_if, s_conn_id,
                                          ESP_GATT_DB_CHARACTERISTIC,
                                          start, end, 0, &count);
        esp_gattc_char_elem_t *chars = NULL;

        s_write_handle = 0;
        s_notify_handle = 0;
        if (st == ESP_GATT_OK && count > 0) chars = malloc(count * sizeof(*chars));
        if (chars) {
            uint16_t combined_handle = 0;
            uint16_t write_handle = 0;
            uint16_t notify_handle = 0;
            uint8_t combined_props = 0;
            uint8_t write_props = 0;
            uint8_t notify_props = 0;
            st = esp_ble_gattc_get_all_char(gattc_if, s_conn_id, start, end, chars, &count, 0);
            if (st != ESP_GATT_OK) count = 0;
            for (uint16_t i = 0; i < count; i++) {
                const esp_gattc_char_elem_t *c = &chars[i];
                ESP_LOGD(TAG, "char handle 0x%04X prop 0x%02X", c->char_handle, c->properties);
                bool writable = (c->properties & (ESP_GATT_CHAR_PROP_BIT_WRITE |
                                                  ESP_GATT_CHAR_PROP_BIT_WRITE_NR)) != 0;
                bool notify = (c->properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) != 0;
                bool indicate = (c->properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0;
                if (writable && (notify || indicate) &&
                    (!combined_handle || (!(combined_props & ESP_GATT_CHAR_PROP_BIT_WRITE) &&
                                           (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE)))) {
                    combined_handle = c->char_handle;
                    combined_props = c->properties;
                }
                if (writable && (!write_handle || (!(write_props & ESP_GATT_CHAR_PROP_BIT_WRITE) &&
                                                   (c->properties & ESP_GATT_CHAR_PROP_BIT_WRITE)))) {
                    write_handle = c->char_handle;
                    write_props = c->properties;
                }
                if (!notify_handle && (notify || indicate)) {
                    notify_handle = c->char_handle;
                    notify_props = c->properties;
                }
            }
            if (combined_handle) {
                s_write_handle = combined_handle;
                s_notify_handle = combined_handle;
                s_notify_indicate = (combined_props & ESP_GATT_CHAR_PROP_BIT_NOTIFY) == 0;
                s_write_type = (combined_props & ESP_GATT_CHAR_PROP_BIT_WRITE) ?
                               ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
            } else {
                s_write_handle = write_handle;
                s_notify_handle = notify_handle;
                s_notify_indicate = (notify_props & ESP_GATT_CHAR_PROP_BIT_NOTIFY) == 0 &&
                                    (notify_props & ESP_GATT_CHAR_PROP_BIT_INDICATE) != 0;
                s_write_type = (write_props & ESP_GATT_CHAR_PROP_BIT_WRITE) ?
                               ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
            }
            free(chars);
        }
        if (s_write_handle && s_notify_handle) break;
        if (start == all_start && end == all_end) break;
        start = all_start;
        end = all_end;
        ESP_LOGW(TAG, "known service has no complete UART channel, searching all attributes");
    }
    if (!s_write_handle) {
        ESP_LOGE(TAG, "adapter has no writable characteristic, dropping the link");
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    /* A writable characteristic is not necessarily the RX channel. Do not
     * start vp_engine until a real notify/indicate characteristic has been
     * discovered and subscribed; otherwise the first AT command is sent into
     * a link that can never deliver its prompt. */
    if (!s_notify_handle) {
        ESP_LOGE(TAG, "adapter has no notify/indicate characteristic, dropping the link");
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    ESP_LOGI(TAG, "write 0x%04X (%s), notify 0x%04X", s_write_handle,
             s_write_type == ESP_GATT_WRITE_TYPE_NO_RSP ? "no-rsp" : "rsp", s_notify_handle);

    esp_err_t notify_err = esp_ble_gattc_register_for_notify(gattc_if, s_peer_bda, s_notify_handle);
    if (notify_err != ESP_OK) {
        ESP_LOGW(TAG, "register notify: %s", esp_err_to_name(notify_err));
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
}

/* Registration is asynchronous; subscribe only after Bluedroid accepts it. */
static void notify_registered(esp_gatt_if_t gattc_if, esp_gatt_status_t status, uint16_t handle)
{
    if (!s_connected || handle != s_notify_handle) return;
    if (status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "notify registration failed, status %d", status);
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    esp_gattc_descr_elem_t descr[2];
    uint16_t n = 2;
    esp_bt_uuid_t cccd = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = UUID16_CCCD } };
    esp_gatt_status_t st = esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id, s_notify_handle, cccd, descr, &n);
    if (st != ESP_GATT_OK && st != ESP_GATT_NOT_FOUND) {
        ESP_LOGW(TAG, "CCCD lookup failed, status %d", st);
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    if (st == ESP_GATT_OK && n > 0) {
        s_cccd_handle = descr[0].handle;
        uint8_t en[2] = { s_notify_indicate ? 0x02 : 0x01, 0x00 };
        esp_err_t err = esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, s_cccd_handle, sizeof(en), en,
                                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (err == ESP_OK) return;   /* ready on WRITE_DESCR_EVT + MTU */
        ESP_LOGW(TAG, "CCCD write: %s", esp_err_to_name(err));
        /* The CCCD write did not even get queued.  Do not treat this as an
         * enabled subscription: without it the ELM prompt can never reach
         * the engine.  The disconnect path will schedule a clean retry. */
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    } else {
        ESP_LOGW(TAG, "no CCCD on 0x%04X, assuming notifications are on", s_notify_handle);
    }
    s_notify_configured = true;
    notify_ready();
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) {
            s_gattc_if = gattc_if;
            wake_task();
        }
        break;

    case ESP_GATTC_CONNECT_EVT:
        /* fires for every LE link (phones, gauge pairing): only ours counts */
        if (s_opening && !s_connected &&
            memcmp(param->connect.remote_bda, s_peer_bda, sizeof(esp_bd_addr_t)) == 0) {
            link_open(gattc_if, param->connect.conn_id);
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        if (memcmp(param->open.remote_bda, s_peer_bda, sizeof(esp_bd_addr_t)) != 0) break;
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "connect failed, status %d", param->open.status);
            if (!s_connected) s_opening = false;   /* obd_poll rescans */
            wake_task();
        } else if (!s_connected) {
            link_open(gattc_if, param->open.conn_id);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        if (!s_connected || param->search_res.conn_id != s_conn_id) break;
        uint16_t sh = param->search_res.start_handle;
        uint16_t eh = param->search_res.end_handle;
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16) {
            uint16_t u = param->search_res.srvc_id.uuid.uuid.uuid16;
            ESP_LOGD(TAG, "service 0x%04X 0x%04X..0x%04X", u, sh, eh);
            for (int i = 0; i < SVC_COUNT; i++) {
                if (u == k_svc_uuid[i]) {
                    s_svc[i].start = sh;
                    s_svc[i].end = eh;
                    s_svc[i].found = true;
                }
            }
        }
        if (eh > s_all_attr_end) s_all_attr_end = eh;
        break;
    }

    case ESP_GATTC_CFG_MTU_EVT:
        if (!s_connected || param->cfg_mtu.conn_id != s_conn_id) break;
        if (param->cfg_mtu.status == ESP_GATT_OK && param->cfg_mtu.mtu > 3) {
            s_att_mtu = param->cfg_mtu.mtu;
            ESP_LOGI(TAG, "ATT MTU negotiated: %u (payload %u)", (unsigned)s_att_mtu,
                     (unsigned)(s_att_mtu - 3));
        } else {
            s_att_mtu = 23;
            ESP_LOGW(TAG, "ATT MTU negotiation failed, using 20-byte payload (status %d)",
                     param->cfg_mtu.status);
        }
        s_mtu_ready = true;
        notify_ready();
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (!s_connected || param->search_cmpl.conn_id != s_conn_id) break;
        if (param->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service discovery failed, status %d", param->search_cmpl.status);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        setup_channel(gattc_if);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        notify_registered(gattc_if, param->reg_for_notify.status, param->reg_for_notify.handle);
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (!s_connected || param->write.conn_id != s_conn_id || param->write.handle != s_cccd_handle) break;
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "enable notify failed, status %d", param->write.status);
            /* Do not start the ELM engine without a usable RX path. Keep the
             * target bound and let the disconnect path schedule a clean
             * reconnect instead of sending ATZ into a dead subscription. */
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        s_notify_configured = true;
        notify_ready();
        break;

    case ESP_GATTC_NOTIFY_EVT:
        if (!s_connected || param->notify.conn_id != s_conn_id ||
            param->notify.handle != s_notify_handle || !s_rx) break;
        if (param->notify.value_len) {
            size_t sent = xStreamBufferSend(s_rx, param->notify.value, param->notify.value_len, 0);
            if (sent < param->notify.value_len) s_rx_dropped += param->notify.value_len - sent;
        }
        wake_task();
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.conn_id == s_conn_id && param->write.handle == s_write_handle) {
            s_tx_status = param->write.status;
            if (param->write.status != ESP_GATT_OK) {
                ESP_LOGW(TAG, "write failed, handle 0x%04X status %d",
                         param->write.handle, param->write.status);
            }
            if (s_tx_waiting) {
                s_tx_waiting = false;
                if (s_tx_done) xSemaphoreGive(s_tx_done);
            }
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        if (!(s_connected || s_opening) ||
            memcmp(param->disconnect.remote_bda, s_peer_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        if (s_connected && param->disconnect.conn_id != s_conn_id) break;
        ESP_LOGI(TAG, "disconnected (reason 0x%02X)", param->disconnect.reason);
        reset_link_state();
        wake_task();   /* engine down; obd_poll rescans for a bound adapter */
        break;

    default:
        break;
    }
}

static bool ensure_registered(void)
{
    if (s_registered) return true;
    if (!s_poll_wake) s_poll_wake = xSemaphoreCreateBinaryStatic(&s_poll_wake_storage);
    if (!s_poll_wake) return false;
    const ble_hub_client_t client = {
        .gap_cb = gap_cb, .gattc_cb = gattc_cb, .gatts_cb = NULL,
        .shutdown_cb = wake_task,
    };
    esp_err_t err = ble_hub_register(BLE_HUB_APP_ELM327, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE register failed: %s", esp_err_to_name(err));
        return false;
    }
    s_registered = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* vp_engine sink (obd_poll task)                                       */
/* ------------------------------------------------------------------ */

static bool sink_send(void *ctx, const char *cmd)
{
    (void)ctx;
    esp_gatt_if_t gif = s_gattc_if;
    uint16_t handle = s_write_handle;
    uint16_t conn_id = s_conn_id;
    esp_gatt_write_type_t write_type = s_write_type;
    if (!ble_hub_is_ready() || s_paused || !s_connected || !s_notify_ready ||
        !handle || gif == ESP_GATT_IF_NONE) {
        return false;
    }
    uint8_t buf[TX_LINE_MAX];
    /* vp_engine supplies normalized profile commands and generates all other
     * commands without whitespace. Keep this transport hot path as one bounded
     * copy; the old per-character whitespace filter duplicated the work for
     * every request and could never change a valid engine command. */
    size_t n = strnlen(cmd, sizeof(buf) - 1);
    memcpy(buf, cmd, n);
    buf[n++] = '\r';
    uint16_t att_payload = s_att_mtu > 3 ? (uint16_t)(s_att_mtu - 3) : TX_CHUNK;
    if (att_payload == 0) att_payload = TX_CHUNK;
    for (size_t off = 0; off < n; off += att_payload) {
        size_t len = (n - off < att_payload) ? n - off : att_payload;
        if (!s_connected || s_conn_id != conn_id || !s_notify_ready) return false;
        if (write_type == ESP_GATT_WRITE_TYPE_RSP) {
            if (!s_tx_done) return false;
            while (xSemaphoreTake(s_tx_done, 0) == pdTRUE) {}
            s_tx_status = ESP_GATT_ERROR;
            s_tx_waiting = true;
        }
        esp_err_t err = esp_ble_gattc_write_char(gif, conn_id, handle, (uint16_t)len, buf + off,
                                                 write_type, ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
            s_tx_waiting = false;
            ESP_LOGW(TAG, "write: %s", esp_err_to_name(err));
            return false;
        }
        if (write_type == ESP_GATT_WRITE_TYPE_RSP &&
            xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(TX_WRITE_TIMEOUT_MS)) != pdTRUE) {
            s_tx_waiting = false;
            ESP_LOGW(TAG, "write response timeout (offset %u length %u mtu %u)",
                     (unsigned)off, (unsigned)len, (unsigned)s_att_mtu);
            return false;
        }
        if (write_type == ESP_GATT_WRITE_TYPE_RSP && s_tx_status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "write response failed (offset %u length %u status %d)",
                     (unsigned)off, (unsigned)len, s_tx_status);
            return false;
        }
    }
    return true;
}

static uint32_t sink_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sink_set_signal(void *ctx, int slot, int32_t internal)
{
    (void)ctx;
    if (!s_quiet) signal_store_set(slot, internal);
}

static void sink_invalidate(void *ctx, int slot)
{
    (void)ctx;
    if (!s_quiet) signal_store_invalidate(slot);
}

static void sink_set_speed(void *ctx, uint8_t kmh)
{
    (void)ctx;
    if (!s_quiet) obd_data_set_speed(kmh);
}

static void sink_on_link(void *ctx, uint8_t state)
{
    (void)ctx;
    if (!s_quiet) signal_store_set(SIG_SYS_OBD_LINK, state);
}

static int sink_veh_register(void *ctx, const char *key, int32_t min, int32_t max,
                             uint32_t stale_ms)
{
    (void)ctx;
    return signal_store_veh_register(key, min, max, stale_ms);
}

static void sink_veh_clear(void *ctx)
{
    (void)ctx;
    signal_store_veh_clear();
}

static void sink_on_response(void *ctx, int slot)
{
    (void)ctx;
    vp_manager_on_response(slot);
}

static void sink_on_voltage(void *ctx, float volts)
{
    (void)ctx;
    vp_manager_on_voltage(volts);
}

static void sink_on_elm_link(void *ctx, bool up)
{
    (void)ctx;
    vp_manager_on_elm_link(up);
}

/* The engine retains a detected protocol for this session.  Keep the user's
 * selection at 0 (Automatic): saving a detected value here turns it into a
 * forced protocol on the next boot and prevents recovery after changing cars. */
static void sink_on_proto(void *ctx, uint8_t proto)
{
    (void)ctx;
    ESP_LOGI(TAG, "protocol %u detected for this session (Automatic selection retained)", proto);
}

/* Engine recovery (failed synchronization, write failure or repeated heals). */
static void sink_reconnect(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "engine requested adapter reconnect");
    s_reconnect_us = esp_timer_get_time();
    close_link();
}

static void sink_log(void *ctx, const char *msg)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", msg);
}

static const vp_engine_sink_t k_sink = {
    .ctx               = NULL,
    .send              = sink_send,
    .now_ms            = sink_now_ms,
    .set_signal        = sink_set_signal,
    .invalidate        = sink_invalidate,
    .set_speed_kmh     = sink_set_speed,
    .on_link           = sink_on_link,
    .veh_register      = sink_veh_register,
    .veh_clear         = sink_veh_clear,
    .on_response       = sink_on_response,
    .on_voltage        = sink_on_voltage,
    .on_elm_link       = sink_on_elm_link,
    .on_proto_detected = sink_on_proto,
    .reconnect         = sink_reconnect,
    .log               = sink_log,
};

static void *psram_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void *ctx, void *ptr)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static const vp_allocator_t k_alloc = { psram_alloc, psram_free, NULL };

/* ------------------------------------------------------------------ */
/* obd_poll task                                                        */
/* ------------------------------------------------------------------ */

static bool engine_create(void)
{
    if (s_engine) return true;
    nvs_user_cfg_t nv = nvs_cfg_read();
    vp_engine_cfg_t cfg = { 0 };
    cfg.proto = (nv.protocol <= 12) ? nv.protocol : 0;
    cfg.responsive_polling = true;
    s_engine = vp_engine_create(vp_manager_active(), &k_sink, &cfg, &k_alloc);
    if (!s_engine) {
        ESP_LOGE(TAG, "vp_engine_create failed (profile %s)", vp_manager_active_id());
        return false;
    }
    vp_engine_set_obd_demand_mask(s_engine, obd_demand_mask_get());
    ESP_LOGI(TAG, "engine ready: profile %s, protocol %u%s", vp_manager_active_id(), cfg.proto,
             cfg.proto ? "" : " (auto)");
    return true;
}

static void engine_down(void)
{
    if (s_engine && s_engine_up) vp_engine_link_down(s_engine);
    s_engine_up = false;
    s_reconnect_us = 0;
}

static void rx_discard(void)
{
    if (s_rx) xStreamBufferReset(s_rx);
}

static void rx_drain(void)
{
    uint8_t buf[RX_CHUNK];
    for (int i = 0; i < RX_DRAIN_MAX; i++) {
        size_t n = xStreamBufferReceive(s_rx, buf, sizeof(buf), 0);
        if (n == 0) break;
        vp_engine_on_rx(s_engine, buf, n);
    }
}

/* Keep looking for a bound adapter while it is not connected. */
static void reconnect_watchdog(int64_t now)
{
    if (s_opening && now - link_time_get(&s_open_us) > OPEN_GUARD_US) {
        ESP_LOGW(TAG, "connect attempt timed out");
        s_opening = false;
    }
    if (!s_target_valid || s_scan_only || s_scan_yielded || s_connected || s_opening) return;
    if (s_gattc_if == ESP_GATT_IF_NONE || ble_hub_scan_active(NULL)) return;
    if (now - link_time_get(&s_last_scan_us) < RESCAN_GAP_US) return;
    scan_start(SCAN_RECONNECT_S);
}

static void log_stats(void)
{
    vp_engine_stats_t st = { 0 };
    if (s_engine) vp_engine_get_stats(s_engine, &st);
    ESP_LOGD(TAG, "stack free %u B, phase %u link %u proto %u, tx %lu req %lu ok %lu fail %lu "
                  "timeout %lu heal %lu, rx dropped %lu",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), st.phase, st.link, st.proto,
             (unsigned long)st.tx_cmds, (unsigned long)st.tx_req, (unsigned long)st.rx_ok,
             (unsigned long)st.rx_fail, (unsigned long)st.timeouts, (unsigned long)st.heals,
             (unsigned long)s_rx_dropped);
}

static void obd_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    engine_create();
    int64_t last_stats = esp_timer_get_time();
    uint32_t wait_ms = IDLE_WAIT_MS;

    for (;;) {
        esp_task_wdt_reset();
        xSemaphoreTake(s_poll_wake, pdMS_TO_TICKS(wait_ms));
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();

        if (!ble_hub_is_ready()) {
            break;   /* BLE released for WiFi OTA */
        }
        if (s_paused) {
            engine_down();
            rx_discard();
            wait_ms = OTA_WAIT_MS;
            continue;
        }
        reconnect_watchdog(now);

        if (ui_showroom_is_active()) {
            /* showroom feeds fake values: keep the engine off the store */
            s_quiet = true;
            engine_down();
            s_quiet = false;
            rx_discard();
            wait_ms = SHOWROOM_WAIT_MS;
            continue;
        }
        if (!s_connected || !s_notify_ready) {
            engine_down();
            rx_discard();
            wait_ms = IDLE_WAIT_MS;
            continue;
        }
        if (!s_engine_up) {
            int64_t settle = link_time_get(&s_ready_us) + NOTIFY_SETTLE_US - now;
            if (settle > 0) {
                wait_ms = (uint32_t)(settle / 1000) + 1;
                continue;
            }
            if (!engine_create()) {
                wait_ms = 1000;
                continue;
            }
            rx_discard();
            vp_engine_link_up(s_engine);
            s_engine_up = true;
        }
        PERF_BEGIN(perf_elm_start);
        rx_drain();
        vp_engine_set_obd_demand_mask(s_engine, obd_demand_mask_get());
        wait_ms = vp_engine_run(s_engine);
        PERF_END(PERF_ELM_RUN, perf_elm_start, 1);

        if (s_reconnect_us && now - s_reconnect_us > RECONNECT_GRACE_US) {
            /* the close did not drop the link: restart the bring-up instead */
            ESP_LOGW(TAG, "adapter link still up after reconnect request, re-running bring-up");
            vp_engine_link_down(s_engine);
            vp_engine_link_up(s_engine);
            s_reconnect_us = 0;
        }
        if (now - last_stats > STATS_EVERY_US) {
            last_stats = now;
            log_stats();
        }
    }

    engine_down();
    ESP_LOGI(TAG, "BLE released, obd_poll exits");
    portENTER_CRITICAL(&s_lock);
    s_task_started = false;
    portEXIT_CRITICAL(&s_lock);
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
}

static void start_task(void)
{
    if (!ble_hub_is_ready()) return;
    portENTER_CRITICAL(&s_lock);
    bool start = !s_task_started;
    s_task_started = true;
    portEXIT_CRITICAL(&s_lock);
    if (!start) {
        wake_task();
        return;
    }
    if (!s_tx_done) s_tx_done = xSemaphoreCreateBinaryStatic(&s_tx_done_storage);
    if (!s_rx) s_rx = xStreamBufferCreateWithCaps(RX_BUF_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx || !app_task_start(APP_TASK_OBD_POLL_BLE, obd_task, NULL)) {
        ESP_LOGE(TAG, "obd_poll start failed (rx %p)", (void *)s_rx);
        portENTER_CRITICAL(&s_lock);
        s_task_started = false;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */

void elm327_ble_ensure_stack_init(void)
{
    ensure_registered();
}

void elm327_ble_start_default(const char *target_name, const uint8_t mac[6])
{
    if (!ensure_registered()) return;
    s_paused = false;
    set_name(target_name);
    bind_target(mac);
    s_scan_only = false;
    start_task();
    wake_task();
}

void elm327_ble_scan_only_start(int duration_s, ble_scan_found_cb_t cb)
{
    if (!ensure_registered()) return;
    s_scan_only = false;
    s_scan_cb = cb;
    s_scan_count = 0;
    memset(s_scan_list, 0, sizeof(s_scan_list));
    s_scan_only = true;
    ESP_LOGD(TAG, "device scan (%d s)", duration_s);
    scan_start(duration_s > 0 ? (uint32_t)duration_s : 0);
}

void elm327_ble_yield_scanner(bool yield)
{
    s_scan_yielded = yield;
    if (yield) ble_hub_scan_stop(BLE_HUB_APP_ELM327);
    else wake_task();
}

void elm327_ble_scan_only_stop(void)
{
    s_scan_only = false;
    ble_hub_scan_stop(BLE_HUB_APP_ELM327);
    ESP_LOGD(TAG, "device scan stopped, %d found", s_scan_count);
    wake_task();   /* resume looking for a bound adapter */
}

void elm327_ble_connect_by_addr(const uint8_t mac[6], const char *name)
{
    if (!mac_nonzero(mac) || !ensure_registered()) return;
    ESP_LOGI(TAG, "bind %02X:%02X:%02X:%02X:%02X:%02X (%s)", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5], name ? name : "");
    s_paused = false;
    s_scan_only = false;
    set_name(name);
    bind_target(mac);
    if (s_connected && !is_target(s_peer_bda)) close_link();   /* switch adapters */
    start_task();
    if (!s_connected && !s_opening) scan_start(SCAN_CONNECT_S);
    wake_task();
}

bool elm327_ble_is_connected(void)
{
    return s_connected;
}

void elm327_ble_set_obd_demand_mask(uint16_t mask)
{
    bool changed;
    portENTER_CRITICAL(&s_lock);
    changed = s_obd_demand_mask != mask;
    s_obd_demand_mask = mask;
    portEXIT_CRITICAL(&s_lock);
    if (changed) wake_task();
}

void elm327_ble_disconnect(void)
{
    bind_target(NULL);   /* no auto-reconnect */
    if (!s_scan_only) ble_hub_scan_stop(BLE_HUB_APP_ELM327);
    close_link();
    wake_task();
}

void elm327_ble_pause_for_ota(void)
{
    s_paused = true;
    s_scan_only = false;
    ble_hub_scan_stop(BLE_HUB_APP_ELM327);
    close_link();
    wake_task();
    ESP_LOGI(TAG, "paused for WiFi OTA");
}

void elm327_ble_copy_connected_name(char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    portENTER_CRITICAL(&s_lock);
    size_t n = capacity - 1 < sizeof(s_target_name) - 1 ? capacity - 1 : sizeof(s_target_name) - 1;
    memcpy(out, s_target_name, n);
    out[n] = '\0';
    portEXIT_CRITICAL(&s_lock);
}
