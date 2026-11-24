/* Compile the real RaceChrono worker, GATT callback and hub lifecycle.
 * Only SDK/RTOS primitives and the signal-store boundary are replaced.
 * Virtual time + deterministic event injection, NOT a radio/RTOS benchmark. */
#include "ble_test_sdk.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp_obd_dsp/racechrono_ble_diy.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "app_obd_dsp/signal_store.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define END_US INT64_C(1000001)
#define IFACE 3
#define CONN 7
#define VALUE_HANDLE 12
#define CCCD_HANDLE 13
#define FILTER_HANDLE 15

typedef enum {
    ALL, ONE, DENY, DISCONNECTED, NOTIFY_OFF, RETIRED, INVALID,
    EVENT_STORM, INTERVAL_101, PRE_TAKE_SHUTDOWN, RECONFIGURE, SNAPSHOT_NOTIFY_OFF,
    SCENARIO_COUNT
} scenario_t;
static const char *const names[] = {
    "all", "one", "deny", "disconnected", "notify_off", "retired", "invalid",
    "event_storm", "101ms", "shutdown_before_take", "reconfigure", "notify_off_during_snapshot"
};
static scenario_t scenario;
static esp_gatts_cb_t hub_gatts;
static esp_gap_ble_cb_t hub_gap;
static TaskFunction_t worker;
static jmp_buf worker_exit;
static int64_t now_us, last_snapshot_us = -1;
static unsigned snapshots, packets, slot_reads, timed_wakes, gives, takes, lock_depth;
static unsigned shutdowns, memory_releases, service_starts, pre_attr_gives;
static bool deleted, injected, reading, shutting_down;
static esp_bt_controller_status_t controller_status;
static esp_bluedroid_status_t bluedroid_status;
static int64_t storm_at = 1000;
static int64_t first_packet_us;
static SemaphoreHandle_t wake_sem;
static unsigned scan_params_calls, scan_start_calls, scan_stop_calls;
static unsigned owner_failures, owner_stops, observer_failures, other_controls;
static esp_err_t scan_start_error, scan_params_error, scan_stop_error;
static uint32_t last_scan_duration;

void ble_test_enter(portMUX_TYPE *mux) { CHECK(!mux->locked); mux->locked = true; lock_depth++; }
void ble_test_exit(portMUX_TYPE *mux) { CHECK(mux->locked && lock_depth); lock_depth--; mux->locked = false; }
void ble_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; CHECK(lock_depth == 0); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
int64_t esp_timer_get_time(void) { return now_us; }

static void write_attr(uint16_t handle, uint8_t *data, uint16_t len)
{
    esp_ble_gatts_cb_param_t p = {0};
    p.write.handle = handle; p.write.value = data; p.write.len = len;
    hub_gatts(ESP_GATTS_WRITE_EVT, IFACE, &p);
}
static void notify(bool enabled)
{
    uint8_t data[] = {(uint8_t)enabled, 0};
    write_attr(CCCD_HANDLE, data, sizeof(data));
}
static void filter_deny(void) { uint8_t data[] = {0}; write_attr(FILTER_HANDLE, data, sizeof(data)); }
static void filter_all(uint16_t interval)
{
    uint8_t data[] = {1, (uint8_t)(interval >> 8), (uint8_t)interval};
    write_attr(FILTER_HANDLE, data, sizeof(data));
}
static void filter_one(unsigned ch, uint16_t interval)
{
    uint32_t pid = 0xF101u + ch;
    uint8_t data[] = {2, (uint8_t)(interval >> 8), (uint8_t)interval,
        (uint8_t)pid, (uint8_t)(pid >> 8), 0, 0};
    write_attr(FILTER_HANDLE, data, sizeof(data));
}
static void connection(bool connect, uint16_t conn, int role)
{
    esp_ble_gatts_cb_param_t p = {0};
    if (connect) {
        p.connect.conn_id = conn; p.connect.link_role = role;
        memset(p.connect.remote_bda, 42, sizeof(p.connect.remote_bda));
    } else {
        p.disconnect.conn_id = conn;
        memset(p.disconnect.remote_bda, 42, sizeof(p.disconnect.remote_bda));
    }
    hub_gatts(connect ? ESP_GATTS_CONNECT_EVT : ESP_GATTS_DISCONNECT_EVT, IFACE, &p);
}
static void shutdown_observer(void)
{
    CHECK(lock_depth == 0 && !ble_hub_is_ready());
    shutdowns++;
}
static void shutdown_hub(void)
{
    CHECK(lock_depth == 0);
    shutting_down = true;
    CHECK(ble_hub_shutdown() == ESP_OK);
    CHECK(shutdowns == 1 && memory_releases == 1);
    CHECK(ble_hub_shutdown() == ESP_OK); /* one-way idempotence */
    CHECK(shutdowns == 1 && memory_releases == 1);
    shutting_down = false;
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    CHECK(lock_depth == 0 && !wake_sem);
    storage->token = false;
    wake_sem = storage;
    return storage;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    CHECK(lock_depth == 0 && sem == wake_sem);
    if (shutting_down) CHECK(!ble_hub_is_ready());
    gives++;
    if (sem->token) return pdFALSE; /* binary token coalescing */
    sem->token = true;
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    CHECK(lock_depth == 0 && sem == wake_sem && ticks != 0);
    CHECK(++takes < 5000); /* negative controls fail, never hang */
    if (scenario == PRE_TAKE_SHUTDOWN && !injected) {
        injected = true;
        shutdown_hub(); /* after the real worker planned its wait, before taking */
    }
    if (sem->token) { sem->token = false; return pdTRUE; }
    int64_t timeout = ticks == portMAX_DELAY ? INT64_MAX : now_us + (int64_t)ticks * 1000;
    while (true) {
        int64_t event_at = END_US;
        if (scenario == EVENT_STORM && storm_at < END_US) event_at = storm_at;
        if (scenario == RECONFIGURE && !injected) event_at = 500000;
        if (scenario == SNAPSHOT_NOTIFY_OFF && injected && !reading) event_at = 110000;
        if (timeout < event_at) { now_us = timeout; timed_wakes++; return pdFALSE; }
        now_us = event_at;
        if (event_at == END_US) shutdown_hub();
        else if (scenario == EVENT_STORM) { filter_all(100); storm_at += 1000; }
        else if (scenario == RECONFIGURE) {
            injected = true;
            /* Reconnect resets deny-all; ignored central links must not do so. */
            connection(false, CONN, 1); connection(true, CONN, 1); notify(true);
            filter_deny(); filter_one(0, 100);
        } else if (scenario == SNAPSHOT_NOTIFY_OFF) { reading = true; notify(true); }
        else CHECK(false);
        CHECK(sem->token); /* every accepted event, including shutdown, wakes */
        sem->token = false;
        return pdTRUE;
    }
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority;
    CHECK(lock_depth == 0 && !worker);
    worker = fn; *handle = (void *)1; return pdPASS;
}
void vTaskDelete(TaskHandle_t task)
{
    CHECK(task == NULL && lock_depth == 0 && !ble_hub_is_ready());
    deleted = true;
    longjmp(worker_exit, 1);
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(lock_depth == 0);
    /* The hub's original settle delay is retained. Worker fixed-delay polling
     * is forbidden in this harness; an old worker fails here. */
    CHECK(shutting_down && ticks == 50);
}

nvs_user_cfg_t nvs_cfg_read(void) { nvs_user_cfg_t cfg = {0}; return cfg; }
esp_err_t esp_read_mac(uint8_t *mac, int type) { (void)type; memset(mac, 0, 6); return ESP_OK; }
uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values)
{
    CHECK(lock_depth == 0 && ble_hub_is_ready() && count && count <= 8);
    CHECK(last_snapshot_us < 0 || now_us - last_snapshot_us >= 20000);
    last_snapshot_us = now_us;
    snapshots++; slot_reads += (unsigned)count;
    for (size_t i = 0; i < count; i++) {
        CHECK(slots[i] != SIG_SLOT_NONE);
        values[i] = slots[i] + 10;
    }
    if (scenario == SNAPSHOT_NOTIFY_OFF && !injected) {
        injected = true; notify(false); /* snapshot/claim race */
    }
    return scenario == INVALID || scenario == EVENT_STORM ? 0 : (UINT64_C(1) << count) - 1u;
}

/* All stack APIs must remain outside critical sections. */
#define SDK_OK(...) do { CHECK(lock_depth == 0); return ESP_OK; } while (0)
esp_err_t esp_ble_gap_register_callback(esp_gap_ble_cb_t cb) { hub_gap = cb; SDK_OK(); }
esp_err_t esp_ble_gattc_register_callback(esp_gattc_cb_t cb) { (void)cb; SDK_OK(); }
esp_err_t esp_ble_gatts_register_callback(esp_gatts_cb_t cb) { hub_gatts = cb; SDK_OK(); }
esp_err_t esp_ble_gatt_set_local_mtu(uint16_t mtu) { (void)mtu; SDK_OK(); }
esp_err_t esp_ble_gap_set_scan_params(esp_ble_scan_params_t *p) { (void)p; CHECK(!lock_depth); ++scan_params_calls; return scan_params_error; }
esp_err_t esp_ble_gap_start_scanning(uint32_t duration) { CHECK(!lock_depth); last_scan_duration = duration; ++scan_start_calls; return scan_start_error; }
esp_err_t esp_ble_gap_stop_scanning(void) { CHECK(!lock_depth); ++scan_stop_calls; return scan_stop_error; }
esp_err_t esp_ble_gap_stop_advertising(void) { SDK_OK(); }
esp_err_t esp_ble_gap_set_device_name(const char *name) { (void)name; SDK_OK(); }
esp_err_t esp_ble_gap_config_adv_data_raw(uint8_t *data, uint32_t len) { (void)data; (void)len; SDK_OK(); }
esp_err_t esp_ble_gap_config_scan_rsp_data_raw(uint8_t *data, uint32_t len) { (void)data; (void)len; SDK_OK(); }
esp_err_t esp_ble_gap_start_advertising(esp_ble_adv_params_t *p) { (void)p; SDK_OK(); }
esp_err_t esp_ble_gattc_app_register(uint16_t app) { (void)app; SDK_OK(); }
esp_err_t esp_ble_gatts_app_register(uint16_t app) { (void)app; SDK_OK(); }
esp_err_t esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *db, esp_gatt_if_t iface, uint8_t n, uint8_t instance)
{ (void)db; (void)iface; (void)n; (void)instance; SDK_OK(); }
esp_err_t esp_ble_gatts_start_service(uint16_t handle)
{
    CHECK(handle == 10 && gives == pre_attr_gives); /* readiness wake comes AFTER start_service */
    service_starts++; SDK_OK();
}
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
esp_err_t esp_ble_gatts_send_indicate(esp_gatt_if_t iface, uint16_t conn, uint16_t handle,
                                     uint16_t len, uint8_t *data, bool confirm)
{
    CHECK(lock_depth == 0 && ble_hub_is_ready() && iface == IFACE && conn == CONN);
    CHECK(handle == VALUE_HANDLE && len == 8 && !confirm && now_us <= END_US);
    uint32_t pid = le32(data);
    CHECK(pid >= 0xF101 && pid <= 0xF108); /* no F109 reuse */
    const int slots[] = {SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_COOLANT_TEMP, SIG_OBD_INTAKE_TEMP,
                         SIG_OBD_OIL_TEMP, SIG_OBD_LOAD, SIG_OBD_THROTTLE, SIG_OBD_BATTERY_VOLTAGE};
    uint32_t expected = (uint32_t)(slots[pid - 0xF101] + 10);
    if (pid == 0xF106 || pid == 0xF107) expected *= 10;
    CHECK(le32(data + 4) == expected);
    if (!packets) first_packet_us = now_us;
    packets++; return ESP_OK;
}
esp_bt_controller_status_t esp_bt_controller_get_status(void) { return controller_status; }
esp_bluedroid_status_t esp_bluedroid_get_status(void) { return bluedroid_status; }
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg) { (void)cfg; controller_status = ESP_BT_CONTROLLER_STATUS_INITED; SDK_OK(); }
esp_err_t esp_bt_controller_enable(int mode) { (void)mode; controller_status = ESP_BT_CONTROLLER_STATUS_ENABLED; SDK_OK(); }
esp_err_t esp_bt_controller_disable(void) { controller_status = ESP_BT_CONTROLLER_STATUS_INITED; SDK_OK(); }
esp_err_t esp_bt_controller_deinit(void) { controller_status = ESP_BT_CONTROLLER_STATUS_IDLE; SDK_OK(); }
esp_err_t esp_bt_controller_mem_release(int mode) { if (mode == ESP_BT_MODE_BLE) memory_releases++; SDK_OK(); }
esp_err_t esp_bluedroid_init(void) { bluedroid_status = ESP_BLUEDROID_STATUS_INITIALIZED; SDK_OK(); }
esp_err_t esp_bluedroid_enable(void) { bluedroid_status = ESP_BLUEDROID_STATUS_ENABLED; SDK_OK(); }
esp_err_t esp_bluedroid_disable(void) { bluedroid_status = ESP_BLUEDROID_STATUS_INITIALIZED; SDK_OK(); }
esp_err_t esp_bluedroid_deinit(void) { bluedroid_status = ESP_BLUEDROID_STATUS_UNINITIALIZED; SDK_OK(); }

static void run_scenario(scenario_t which)
{
    scenario = which;
    racechrono_ble_diy_start(true);
    CHECK(worker && hub_gatts && wake_sem && ble_hub_is_ready());
    ble_hub_client_t observer = {.shutdown_cb = shutdown_observer};
    CHECK(ble_hub_register(BLE_HUB_APP_TPMS, &observer) == ESP_OK);
    esp_ble_gatts_cb_param_t p = {0};
    p.reg.app_id = BLE_HUB_APP_RACECHRONO;
    hub_gatts(ESP_GATTS_REG_EVT, IFACE, &p);
    memset(&p, 0, sizeof(p));
    uint16_t handles[] = {10, 11, VALUE_HANDLE, CCCD_HANDLE, 14, FILTER_HANDLE};
    p.add_attr_tab.num_handle = 6; p.add_attr_tab.handles = handles;
    p.add_attr_tab.svc_uuid.uuid.uuid16 = 0x1FF8;
    pre_attr_gives = gives;
    hub_gatts(ESP_GATTS_CREAT_ATTR_TAB_EVT, IFACE, &p);
    CHECK(service_starts == 1);
    if (which != DISCONNECTED) connection(true, CONN, 1);
    if (which != NOTIFY_OFF) notify(true);
    if (which == ONE || which == DENY || which == RETIRED || which == RECONFIGURE) filter_deny();
    if (which == ONE) filter_one(0, 100);
    if (which == RETIRED) filter_one(8, 20);
    if (which == INTERVAL_101) filter_all(101);
    unsigned before_ignored = gives;
    connection(true, CONN + 1, 0); /* our central link is not a phone */
    connection(false, CONN + 1, 0);
    CHECK(gives == before_ignored);
    if (!setjmp(worker_exit)) worker(NULL);
    CHECK(deleted && shutdowns == 1 && memory_releases == 1);
    CHECK(!ble_hub_is_ready() && !lock_depth);
    unsigned expected = which == ALL ? 10 : which == ONE ? 10 : which == INTERVAL_101 ? 9 :
        which == INVALID || which == EVENT_STORM ? 46 : which == RECONFIGURE ? 6 :
        which == SNAPSHOT_NOTIFY_OFF ? 10 : 0;
    CHECK(snapshots == expected);
    CHECK(packets == (which == ALL ? 80 : which == ONE ? 10 : which == INTERVAL_101 ? 72 :
        which == RECONFIGURE ? 6 : which == SNAPSHOT_NOTIFY_OFF ? 72 : 0));
    if (which == SNAPSHOT_NOTIFY_OFF) CHECK(first_packet_us == 120000);
    if (which == DENY || which == DISCONNECTED || which == NOTIFY_OFF || which == RETIRED) CHECK(timed_wakes == 0);
    if (which == ALL || which == ONE) CHECK(timed_wakes == 10);
    if (which == INTERVAL_101) CHECK(timed_wakes == 9 && first_packet_us == 101000);
    /* Late callbacks after self-delete can still give boot-lifetime storage. */
    filter_all(100); notify(false);
    CHECK(wake_sem->token);
    CHECK(ble_hub_init() == ESP_ERR_INVALID_STATE);
    printf("%s: snapshots=%u slots=%u packets=%u timed_wakes=%u event_gives=%u\n",
           names[which], snapshots, slot_reads, packets, timed_wakes, gives);
    fflush(stdout);
}

static unsigned owner_ads, owner_done, observer_ads, observer_done, other_results;
static void owner_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    CHECK(!lock_depth);
    if (e != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) ++owner_ads;
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) ++owner_done;
}
static void observer_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    CHECK(!lock_depth);
    if (e != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) ++observer_ads;
    if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) ++observer_done;
}
static void other_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    (void)p; CHECK(!lock_depth);
    if (e == ESP_GAP_BLE_SCAN_RESULT_EVT) ++other_results;
}
static void control_owner_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    CHECK(!lock_depth);
    if (e == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT && p->scan_start_cmpl.status) ++owner_failures;
    if (e == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) ++owner_stops;
    owner_gap(e, p);
}
static void control_observer_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    CHECK(!lock_depth);
    if (e == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT && p->scan_start_cmpl.status) ++observer_failures;
    observer_gap(e, p);
}
static void control_other_gap(esp_gap_ble_cb_event_t e, esp_ble_gap_cb_param_t *p)
{
    (void)p; CHECK(!lock_depth);
    if (e != ESP_GAP_BLE_SCAN_RESULT_EVT) ++other_controls;
}
static void scan_control_lifecycle(void)
{
    CHECK(ble_hub_init() == ESP_OK && hub_gap);
    ble_hub_client_t elm = {.gap_cb=control_owner_gap};
    ble_hub_client_t tpms = {.gap_cb=control_observer_gap, .observe_scan_results=true};
    ble_hub_client_t other = {.gap_cb=control_other_gap};
    CHECK(ble_hub_register(BLE_HUB_APP_ELM327, &elm) == ESP_OK);
    CHECK(ble_hub_register(BLE_HUB_APP_TPMS, &tpms) == ESP_OK);
    CHECK(ble_hub_register(BLE_HUB_APP_GAUGE_PAIR, &other) == ESP_OK);
    const esp_ble_scan_params_t params = {0};
    esp_ble_gap_cb_param_t p = {0};
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 10) == ESP_OK);
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    hub_gap(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 20) == ESP_OK);
    CHECK(scan_params_calls == 1 && scan_stop_calls == 1); /* wait for stop before reconfiguring */
    CHECK(ble_hub_scan_start(BLE_HUB_APP_TPMS, &params, 10) == ESP_ERR_INVALID_STATE);
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 30) == ESP_OK);
    CHECK(scan_stop_calls == 1); /* coalesce same-owner restarts */
    p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
    hub_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p); /* late natural completion cannot release the lease */
    CHECK(ble_hub_scan_active(NULL) && !owner_done);
    p = (esp_ble_gap_cb_param_t){0};
    hub_gap(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
    CHECK(scan_params_calls == 2 && owner_stops == 1);
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    hub_gap(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    CHECK(last_scan_duration == 30);
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_OK);
    CHECK(ble_hub_scan_active(NULL));
    CHECK(ble_hub_scan_start(BLE_HUB_APP_TPMS, &params, 10) == ESP_ERR_INVALID_STATE);
    hub_gap(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
    CHECK(!ble_hub_scan_active(NULL));
    scan_start_error = ESP_ERR_INVALID_STATE;
    CHECK(ble_hub_scan_start(BLE_HUB_APP_TPMS, &params, 10) == ESP_OK);
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    CHECK(!ble_hub_scan_active(NULL) && observer_failures == 1 && !owner_failures && !other_controls);
    scan_start_error = ESP_OK;
    /* Cancellation while parameters or scan start are outstanding must drain that event first. */
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 10) == ESP_OK);
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_OK);
    CHECK(scan_stop_calls == 2 && ble_hub_scan_active(NULL));
    unsigned starts = scan_start_calls;
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    CHECK(!ble_hub_scan_active(NULL) && scan_start_calls == starts && owner_stops == 3);
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 10) == ESP_OK);
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_OK);
    CHECK(scan_stop_calls == 2);
    hub_gap(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    CHECK(scan_stop_calls == 3 && ble_hub_scan_active(NULL));
    hub_gap(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
    CHECK(!ble_hub_scan_active(NULL));
    /* Parameter failure must also release the reservation. */
    scan_params_error = ESP_ERR_INVALID_STATE;
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 10) == ESP_ERR_INVALID_STATE);
    CHECK(!ble_hub_scan_active(NULL));
    scan_params_error = ESP_OK;
    CHECK(ble_hub_scan_start(BLE_HUB_APP_ELM327, &params, 10) == ESP_OK);
    hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
    hub_gap(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
    scan_stop_error = ESP_ERR_INVALID_STATE;
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_ERR_INVALID_STATE);
    CHECK(ble_hub_scan_active(NULL)); /* controller may still be scanning */
    CHECK(ble_hub_scan_start(BLE_HUB_APP_TPMS, &params, 10) == ESP_ERR_INVALID_STATE);
    scan_stop_error = ESP_OK;
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_OK);
    p.scan_stop_cmpl.status = ESP_BT_STATUS_FAIL;
    hub_gap(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
    CHECK(ble_hub_scan_active(NULL));
    p = (esp_ble_gap_cb_param_t){0};
    CHECK(ble_hub_scan_stop(BLE_HUB_APP_ELM327) == ESP_OK);
    p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
    hub_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
    CHECK(ble_hub_scan_active(NULL)); /* drain the outstanding stop first */
    p.scan_stop_cmpl.status = ESP_BT_STATUS_FAIL;
    hub_gap(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, &p);
    CHECK(!ble_hub_scan_active(NULL)); /* natural completion already proved idle */
    CHECK(p.scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS);
    shutting_down = true;
    CHECK(ble_hub_shutdown() == ESP_OK);
    puts("scan control: async restart, cancellation, stop lease and failure isolation pass");
}
static void scan_observer_routing(void)
{
    CHECK(ble_hub_init() == ESP_OK && hub_gap);
    ble_hub_client_t elm = {.gap_cb=owner_gap};
    ble_hub_client_t tpms = {.gap_cb=observer_gap, .observe_scan_results=true};
    ble_hub_client_t other = {.gap_cb=other_gap};
    CHECK(ble_hub_register(BLE_HUB_APP_ELM327, &elm) == ESP_OK);
    CHECK(ble_hub_register(BLE_HUB_APP_TPMS, &tpms) == ESP_OK);
    CHECK(ble_hub_register(BLE_HUB_APP_GAUGE_PAIR, &other) == ESP_OK);
    const esp_ble_scan_params_t params = {0};
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        ble_hub_app_t requested = cycle ? BLE_HUB_APP_TPMS : BLE_HUB_APP_ELM327;
        CHECK(ble_hub_scan_start(requested, &params, 10) == ESP_OK);
        esp_ble_gap_cb_param_t p = {0};
        p.scan_param_cmpl.status = ESP_BT_STATUS_SUCCESS;
        hub_gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, &p);
        p = (esp_ble_gap_cb_param_t){0};
        p.scan_start_cmpl.status = ESP_BT_STATUS_SUCCESS;
        hub_gap(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, &p);
        ble_hub_app_t current;
        CHECK(ble_hub_scan_active(&current) && current == requested);
        CHECK(ble_hub_scan_start(BLE_HUB_APP_GAUGE_PAIR, &params, 20) == ESP_ERR_INVALID_STATE);
        p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
        hub_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
        CHECK(ble_hub_scan_active(&current) && current == requested);
        CHECK(owner_ads == 1 && observer_ads == cycle + 1 && !other_results);
        p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
        hub_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
        CHECK(!ble_hub_scan_active(NULL));
        CHECK(owner_done == 1 && observer_done == cycle && !other_results);
        /* No owner: neither consumers nor observers see orphan results. */
        p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
        hub_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
        CHECK(owner_ads == 1 && observer_ads == cycle + 1 && !other_results);
    }
    shutting_down = true;
    CHECK(ble_hub_shutdown() == ESP_OK);
    puts("scan observers: ads only, ownership and completion isolation pass");
}

int main(void)
{
    pid_t control_pid = fork(); CHECK(control_pid >= 0);
    if (!control_pid) { scan_control_lifecycle(); exit(0); }
    int control_status = 0;
    CHECK(waitpid(control_pid, &control_status, 0) == control_pid);
    CHECK(WIFEXITED(control_status) && WEXITSTATUS(control_status) == 0);
    pid_t observer_pid = fork(); CHECK(observer_pid >= 0);
    if (!observer_pid) { scan_observer_routing(); exit(0); }
    int observer_status = 0;
    CHECK(waitpid(observer_pid, &observer_status, 0) == observer_pid);
    CHECK(WIFEXITED(observer_status) && WEXITSTATUS(observer_status) == 0);
    /* Every scenario gets a fresh boot; one-way shutdown is never reset by
     * reaching into the firmware's static state. */
    for (unsigned i = 0; i < SCENARIO_COUNT; i++) {
        fflush(NULL);
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { run_scenario((scenario_t)i); exit(0); }
        int status = 0; CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("racechrono_stream: all deterministic firmware-wrapper scenarios pass");
    return 0;
}
