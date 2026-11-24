/*
 * ble_hub.c - the only owner of the Bluedroid GAP / GATTC / GATTS callbacks
 * and of the controller / Bluedroid lifecycle. Contract: ble_hub.h.
 *
 * Routing (callbacks run on the Bluedroid BTC task):
 *   - GAP: scan control completions go only to the owner; other events are
 *     fanned out to every registered module, except SCAN_RESULT_EVT,
 *     which the owner and opt-in advertisement observers receive (ownership is released
 *     before the owner sees INQ_CMPL, so it may restart scanning from there);
 *   - GATTC / GATTS: REG_EVT by app_id (the hub learns gattc_if / gatts_if),
 *     every other event by interface (ESP_GATT_IF_NONE = every module).
 * The module table is copied under a spinlock and module callbacks run with
 * no hub lock held, so a module may call back into the hub from a callback.
 *
 * Scanning: ble_hub_scan_start() sets the parameters, SCAN_PARAM_SET_COMPLETE
 * starts the scan for the owner. Stop/restart drains the outstanding parameters,
 * start and stop callbacks before releasing or reconfiguring the scanner.
 *
 * Shutdown (WiFi OTA): ble_hub_is_ready() turns false first, so the module
 * tasks stop touching BT, then advertising / scanning stop, Bluedroid and the
 * controller go down and the controller memory is handed to the heap. It is
 * one-way (reboot required) and safe to call more than once.
 */
#include "bsp_obd_dsp/ble_hub.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_hub";

#define HUB_MAX_APPS        4
#define HUB_LOCAL_MTU       517     /* phone OTA app asks for 517; negotiated = min of both */
#define HUB_NO_OWNER        (-1)
#define HUB_STOP_SETTLE_MS  50

typedef struct {
    bool             used;
    ble_hub_app_t    app;
    ble_hub_client_t cb;
    esp_gatt_if_t    gattc_if;
    esp_gatt_if_t    gatts_if;
} hub_slot_t;

static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static hub_slot_t    s_slots[HUB_MAX_APPS];
static volatile bool s_ready;
static volatile bool s_shutdown;
static bool          s_init_busy;       /* bring-up / shutdown in progress (under s_lock) */
static bool          s_cbs_registered;

/* Scanner arbitration (under s_lock). */
static int      s_scan_owner = HUB_NO_OWNER;
typedef enum { SCAN_IDLE, SCAN_PARAMS, SCAN_STARTING, SCAN_ACTIVE, SCAN_STOPPING } scan_phase_t;
static scan_phase_t s_scan_phase;
static uint32_t s_scan_duration;
static bool s_scan_cancel, s_scan_restart, s_scan_ended;
static esp_ble_scan_params_t s_scan_next_params;
static uint32_t s_scan_next_duration;

/* ------------------------------------------------------------------ */

static bool hub_busy_take(uint32_t max_ms)
{
    uint32_t waited = 0;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        if (!s_init_busy) {
            s_init_busy = true;
            portEXIT_CRITICAL(&s_lock);
            return true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (waited >= max_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

static void hub_busy_give(void)
{
    portENTER_CRITICAL(&s_lock);
    s_init_busy = false;
    portEXIT_CRITICAL(&s_lock);
}

static hub_slot_t *slot_find(ble_hub_app_t app)
{
    for (int i = 0; i < HUB_MAX_APPS; i++) {
        if (s_slots[i].used && s_slots[i].app == app) return &s_slots[i];
    }
    return NULL;
}

static void scan_release_locked(void)
{
    s_scan_owner = HUB_NO_OWNER;
    s_scan_phase = SCAN_IDLE;
    s_scan_cancel = false;
    s_scan_restart = false;
    s_scan_ended = false;
}

/* Keep a queued restart reserved across the old owner's completion callback. */
static bool scan_finish_locked(esp_ble_scan_params_t *next)
{
    if (s_scan_restart && !s_shutdown) {
        *next = s_scan_next_params;
        s_scan_duration = s_scan_next_duration;
        s_scan_phase = SCAN_PARAMS;
        s_scan_cancel = s_scan_restart = s_scan_ended = false;
        return true;
    }
    scan_release_locked();
    return false;
}

static void hub_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

static esp_err_t scan_set_params(const esp_ble_scan_params_t *params)
{
    esp_err_t err = esp_ble_gap_set_scan_params((esp_ble_scan_params_t *)params);
    if (err != ESP_OK) {
        esp_ble_gap_cb_param_t failed = {0};
        failed.scan_param_cmpl.status = ESP_BT_STATUS_FAIL;
        hub_gap_cb(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &failed);
    }
    return err;
}

static esp_err_t scan_stop_controller(void)
{
    esp_err_t err = esp_ble_gap_stop_scanning();
    if (err != ESP_OK) {
        esp_ble_gap_cb_param_t failed = {0};
        failed.scan_stop_cmpl.status = ESP_BT_STATUS_FAIL;
        hub_gap_cb(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &failed);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Bluedroid callbacks                                                  */
/* ------------------------------------------------------------------ */

static void hub_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_gap_ble_cb_t cbs[HUB_MAX_APPS];
    int n = 0, only = HUB_NO_OWNER;
    bool control = false, start = false, stop = false, restart = false, cancelled = false;
    uint32_t duration = 0;
    esp_ble_scan_params_t next = {0};

    portENTER_CRITICAL(&s_lock);
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        control = true;
        if (s_scan_phase != SCAN_PARAMS) break;
        only = s_scan_owner;
        if (s_scan_cancel) {
            cancelled = true;
            restart = scan_finish_locked(&next);
        } else if (param->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            restart = scan_finish_locked(&next);
        } else {
            s_scan_phase = SCAN_STARTING;
            start = true;
            duration = s_scan_duration;
        }
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        control = true;
        if (s_scan_phase != SCAN_STARTING) break;
        only = s_scan_owner;
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            cancelled = s_scan_cancel;
            restart = scan_finish_locked(&next);
        } else if (s_scan_cancel) {
            s_scan_phase = SCAN_STOPPING;
            stop = true;
        } else {
            s_scan_phase = SCAN_ACTIVE;
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        control = true;
        if (s_scan_phase != SCAN_STOPPING) break;
        only = s_scan_owner;
        if (param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS || s_scan_ended) {
            /* A scan can expire between issuing stop and its completion. Its
             * INQ_CMPL proves idle even if the redundant stop reports failure. */
            param->scan_stop_cmpl.status = ESP_BT_STATUS_SUCCESS;
            restart = scan_finish_locked(&next);
        } else {
            /* A failed stop does not prove that the controller is idle. */
            s_scan_phase = SCAN_ACTIVE;
            s_scan_cancel = s_scan_restart = s_scan_ended = false;
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (s_scan_phase == SCAN_STOPPING && param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)
            s_scan_ended = true;
        if (s_scan_phase != SCAN_ACTIVE || s_scan_cancel) break;
        only = s_scan_owner;
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            restart = scan_finish_locked(&next);
        }
        break;
    default:
        break;
    }
    for (int i = 0; i < HUB_MAX_APPS; i++) {
        const hub_slot_t *slot = &s_slots[i];
        if (!slot->used || !slot->cb.gap_cb) continue;
        if (control && (int)slot->app != only) continue;
        if (event == ESP_GAP_BLE_SCAN_RESULT_EVT &&
            (only == HUB_NO_OWNER || ((int)slot->app != only &&
             !(slot->cb.observe_scan_results && param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT))))
            continue;
        cbs[n++] = slot->cb.gap_cb;
    }
    portEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < n; i++) cbs[i](event, param);
    if (cancelled) {
        /* No scan was started, so there will be no controller stop callback. */
        esp_ble_gap_cb_param_t done = {0};
        done.scan_stop_cmpl.status = ESP_BT_STATUS_SUCCESS;
        for (int i = 0; i < n; i++) cbs[i](ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &done);
    }
    if (s_shutdown) return;
    if (restart) scan_set_params(&next);
    if (stop) scan_stop_controller();
    if (start) {
        esp_err_t err = esp_ble_gap_start_scanning(duration);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "start scanning failed: %s", esp_err_to_name(err));
            esp_ble_gap_cb_param_t failed = {0};
            failed.scan_start_cmpl.status = ESP_BT_STATUS_FAIL;
            hub_gap_cb(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &failed);
        }
    }
}

static void hub_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_gattc_cb_t cbs[HUB_MAX_APPS];
    int n = 0;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < HUB_MAX_APPS; i++) {
        hub_slot_t *s = &s_slots[i];
        if (!s->used || !s->cb.gattc_cb) continue;
        if (event == ESP_GATTC_REG_EVT) {
            if (param->reg.app_id != (uint16_t)s->app) continue;
            s->gattc_if = (param->reg.status == ESP_GATT_OK) ? gattc_if : ESP_GATT_IF_NONE;
        } else if (gattc_if != ESP_GATT_IF_NONE && gattc_if != s->gattc_if) {
            continue;
        }
        cbs[n++] = s->cb.gattc_cb;
    }
    portEXIT_CRITICAL(&s_lock);

    if (event == ESP_GATTC_REG_EVT && param->reg.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "GATTC app 0x%02X register failed, status %d", param->reg.app_id, param->reg.status);
    }
    for (int i = 0; i < n; i++) cbs[i](event, gattc_if, param);
}

static void hub_gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    esp_gatts_cb_t cbs[HUB_MAX_APPS];
    int n = 0;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < HUB_MAX_APPS; i++) {
        hub_slot_t *s = &s_slots[i];
        if (!s->used || !s->cb.gatts_cb) continue;
        if (event == ESP_GATTS_REG_EVT) {
            if (param->reg.app_id != (uint16_t)s->app) continue;
            s->gatts_if = (param->reg.status == ESP_GATT_OK) ? gatts_if : ESP_GATT_IF_NONE;
        } else if (gatts_if != ESP_GATT_IF_NONE && gatts_if != s->gatts_if) {
            continue;
        }
        cbs[n++] = s->cb.gatts_cb;
    }
    portEXIT_CRITICAL(&s_lock);

    if (event == ESP_GATTS_REG_EVT && param->reg.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "GATTS app 0x%02X register failed, status %d", param->reg.app_id, param->reg.status);
    }
    for (int i = 0; i < n; i++) cbs[i](event, gatts_if, param);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t bring_up(void)
{
    esp_err_t err;

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);   /* BLE only */
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller init failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "controller enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        err = esp_bluedroid_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        err = esp_bluedroid_enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (!s_cbs_registered) {
        err = esp_ble_gap_register_callback(hub_gap_cb);
        if (err == ESP_OK) err = esp_ble_gattc_register_callback(hub_gattc_cb);
        if (err == ESP_OK) err = esp_ble_gatts_register_callback(hub_gatts_cb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "callback register failed: %s", esp_err_to_name(err));
            return err;
        }
        s_cbs_registered = true;
    }
    /* needs Bluedroid enabled */
    err = esp_ble_gatt_set_local_mtu(HUB_LOCAL_MTU);
    if (err != ESP_OK) ESP_LOGW(TAG, "set local MTU failed: %s", esp_err_to_name(err));

    s_ready = true;
    ESP_LOGI(TAG, "BLE stack up");
    return ESP_OK;
}

esp_err_t ble_hub_init(void)
{
    if (s_shutdown) return ESP_ERR_INVALID_STATE;
    if (s_ready) return ESP_OK;
    hub_busy_take(UINT32_MAX);
    esp_err_t err = ESP_OK;
    if (s_shutdown) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!s_ready) {
        err = bring_up();
    }
    hub_busy_give();
    return err;
}

bool ble_hub_is_ready(void)
{
    return s_ready && !s_shutdown;
}

esp_err_t ble_hub_register(ble_hub_app_t app, const ble_hub_client_t *client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ble_hub_init();
    if (err != ESP_OK) return err;

    bool reg_c = false;
    bool reg_s = false;
    portENTER_CRITICAL(&s_lock);
    hub_slot_t *s = slot_find(app);
    if (!s) {
        for (int i = 0; i < HUB_MAX_APPS && !s; i++) {
            if (!s_slots[i].used) s = &s_slots[i];
        }
        if (s) {
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->app = app;
            s->gattc_if = ESP_GATT_IF_NONE;
            s->gatts_if = ESP_GATT_IF_NONE;
        }
    }
    if (s) {
        reg_c = client->gattc_cb && !s->cb.gattc_cb;
        reg_s = client->gatts_cb && !s->cb.gatts_cb;
        s->cb = *client;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!s) return ESP_ERR_NO_MEM;

    if (reg_c) {
        err = esp_ble_gattc_app_register((uint16_t)app);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gattc app 0x%02X register: %s", (unsigned)app, esp_err_to_name(err));
            portENTER_CRITICAL(&s_lock);
            s->cb.gattc_cb = NULL;          /* a later register retries */
            portEXIT_CRITICAL(&s_lock);
        }
    }
    if (reg_s) {
        esp_err_t e2 = esp_ble_gatts_app_register((uint16_t)app);
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "gatts app 0x%02X register: %s", (unsigned)app, esp_err_to_name(e2));
            portENTER_CRITICAL(&s_lock);
            s->cb.gatts_cb = NULL;
            portEXIT_CRITICAL(&s_lock);
            if (err == ESP_OK) err = e2;
        }
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Scanner                                                              */
/* ------------------------------------------------------------------ */

esp_err_t ble_hub_scan_start(ble_hub_app_t app, const esp_ble_scan_params_t *params,
                             uint32_t duration_s)
{
    if (!params) return ESP_ERR_INVALID_ARG;
    bool stop = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_ready || s_shutdown || !slot_find(app) ||
        (s_scan_owner != HUB_NO_OWNER && s_scan_owner != (int)app)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scan_owner == (int)app) {
        /* Copy caller-owned parameters; the old scan must drain first. */
        s_scan_next_params = *params;
        s_scan_next_duration = duration_s;
        s_scan_restart = s_scan_cancel = true;
        if (s_scan_phase == SCAN_ACTIVE) {
            s_scan_phase = SCAN_STOPPING;
            stop = true;
        }
        portEXIT_CRITICAL(&s_lock);
        return stop ? scan_stop_controller() : ESP_OK;
    }
    s_scan_owner = (int)app;
    s_scan_phase = SCAN_PARAMS;
    s_scan_cancel = s_scan_restart = s_scan_ended = false;
    s_scan_duration = duration_s;
    portEXIT_CRITICAL(&s_lock);
    return scan_set_params(params);
}

esp_err_t ble_hub_scan_stop(ble_hub_app_t app)
{
    bool stop = false;
    portENTER_CRITICAL(&s_lock);
    if (s_scan_owner != (int)app) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_cancel = true;
    s_scan_restart = false;
    if (s_scan_phase == SCAN_ACTIVE) {
        s_scan_phase = SCAN_STOPPING;
        stop = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return stop ? scan_stop_controller() : ESP_OK;
}

bool ble_hub_scan_active(ble_hub_app_t *owner)
{
    portENTER_CRITICAL(&s_lock);
    int o = s_scan_owner;
    portEXIT_CRITICAL(&s_lock);
    if (o == HUB_NO_OWNER) return false;
    if (owner) *owner = (ble_hub_app_t)o;
    return true;
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                             */
/* ------------------------------------------------------------------ */

static void keep_first_err(esp_err_t *acc, esp_err_t err, const char *what)
{
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return;
    ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    if (*acc == ESP_OK) *acc = err;
}

esp_err_t ble_hub_shutdown(void)
{
    portENTER_CRITICAL(&s_lock);
    void (*wake[HUB_MAX_APPS])(void);
    unsigned wake_count = 0;
    bool first = !s_shutdown;
    if (first) {
        for (unsigned i = 0; i < HUB_MAX_APPS; i++) {
            if (s_slots[i].used && s_slots[i].cb.shutdown_cb) wake[wake_count++] = s_slots[i].cb.shutdown_cb;
        }
    }
    s_shutdown = true;                 /* ble_hub_is_ready() is false from here on */
    scan_release_locked();
    portEXIT_CRITICAL(&s_lock);
    if (!first) return ESP_OK;
    /* Sleeping clients cannot discover !ready by polling. Their wake tokens
     * survive the race where shutdown occurs just before a client blocks. */
    for (unsigned i = 0; i < wake_count; i++) wake[i]();

    /* Never tear down the controller concurrently with an in-flight bring_up(). */
    hub_busy_take(UINT32_MAX);
    s_ready = false;

    esp_err_t err = ESP_OK;
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_ble_gap_stop_advertising();
        esp_ble_gap_stop_scanning();
        vTaskDelay(pdMS_TO_TICKS(HUB_STOP_SETTLE_MS));   /* module tasks see !ready, BTC drains */
        keep_first_err(&err, esp_bluedroid_disable(), "bluedroid disable");
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        keep_first_err(&err, esp_bluedroid_deinit(), "bluedroid deinit");
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        keep_first_err(&err, esp_bt_controller_disable(), "controller disable");
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        keep_first_err(&err, esp_bt_controller_deinit(), "controller deinit");
    }
    /* Once only: releasing the same regions twice would corrupt the heap. */
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        keep_first_err(&err, esp_bt_controller_mem_release(ESP_BT_MODE_BLE), "controller mem release");
    }

    hub_busy_give();
    ESP_LOGI(TAG, "BLE released (reboot required to use BLE again)");
    return err;
}
