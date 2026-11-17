#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Compile the actual ELM worker, start and shutdown wake paths. The scheduler
 * is deterministic; it does not model BLE timing or FreeRTOS internals. */
typedef struct { bool token; } StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int esp_err_t;
typedef struct { void (*gap_cb)(void); void (*gattc_cb)(void); void (*gatts_cb)(void); void (*shutdown_cb)(void); } ble_hub_client_t;
#define ESP_OK 0
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define BLE_HUB_APP_ELM327 1
#define RX_BUF_BYTES 1024
#define TASK_STACK 5120
#define TASK_PRIO 4
#define IDLE_WAIT_MS 300
#define OTA_WAIT_MS 200
#define SHOWROOM_WAIT_MS 500
#define NOTIFY_SETTLE_US 300000
#define RECONNECT_GRACE_US 5000000
#define STATS_EVERY_US 60000000
#define PERF_BEGIN(name) ((void)0)
#define PERF_END(metric, start, count) ((void)0)
#define portENTER_CRITICAL(lock) do { assert(*(lock) == 0); (*(lock))++; lock_entries++; } while (0)
#define portEXIT_CRITICAL(lock) do { assert(*(lock) == 1); (*(lock))--; } while (0)
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))

static const char *TAG = "test";
static StaticSemaphore_t s_poll_wake_storage, s_tx_done_storage;
static SemaphoreHandle_t s_poll_wake, s_tx_done;
static char s_target_name[32] = "OBDII";
static int s_lock;
static unsigned lock_entries;
static bool s_registered, s_task_started, s_paused, s_connected, s_notify_ready, s_engine_up, s_quiet;
static int64_t s_ready_us, s_reconnect_us;
static void *s_rx, *s_engine;
static bool hub_ready = true, fail_create, fail_semaphore, shutdown_in_wait, deleted;
static unsigned creates, gives, takes, task_deletes, downs;
static TaskFunction_t worker;
static void (*shutdown_callback)(void);
static jmp_buf exit_worker;

static void gap_cb(void) {}
static void gattc_cb(void) {}
static esp_err_t ble_hub_register(int app, const ble_hub_client_t *client)
{ assert(app == BLE_HUB_APP_ELM327 && client->shutdown_cb); shutdown_callback = client->shutdown_cb; return ESP_OK; }
static bool ble_hub_is_ready(void) { return hub_ready; }
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{ if (fail_semaphore) return NULL; storage->token=false; return storage; }
static int xSemaphoreGive(SemaphoreHandle_t sem) { assert(sem); gives++; sem->token=true; return pdTRUE; }
static int xSemaphoreTake(SemaphoreHandle_t sem, unsigned wait)
{
    assert(sem && wait && ++takes < 10);
    if (shutdown_in_wait) { shutdown_in_wait=false; hub_ready=false; shutdown_callback(); }
    if (sem->token) { sem->token=false; return pdTRUE; }
    return 0;
}
static void *xStreamBufferCreateWithCaps(unsigned size, unsigned trigger, unsigned caps)
{ assert(size == RX_BUF_BYTES && trigger == 1 && caps); return (void *)1; }
static int xTaskCreate(TaskFunction_t fn, const char *name, unsigned stack, void *arg, unsigned prio, void *handle)
{ (void)name; assert(stack == TASK_STACK && !arg && prio == TASK_PRIO && !handle); creates++; if (fail_create) return 0; worker=fn; return pdPASS; }
#define APP_TASK_OBD_POLL_BLE 1
static bool app_task_start(int id, TaskFunction_t fn, void *arg)
{ assert(id==APP_TASK_OBD_POLL_BLE); return xTaskCreate(fn,"obd_poll",TASK_STACK,arg,TASK_PRIO,NULL)==pdPASS; }
static void vTaskDelete(void *task) { assert(!task); task_deletes++; deleted=true; longjmp(exit_worker, 1); }
static void esp_task_wdt_add(void *t) { (void)t; }
static void esp_task_wdt_reset(void) {}
static void esp_task_wdt_delete(void *t) { (void)t; }
static int64_t esp_timer_get_time(void) { return 0; }
static bool engine_create(void) { return true; }
static void engine_down(void) { downs++; s_engine_up=false; }
static void rx_discard(void) {}
static void reconnect_watchdog(int64_t now) { (void)now; }
static bool ui_showroom_is_active(void) { return false; }
static void vp_engine_link_up(void *e) { (void)e; }
static void vp_engine_link_down(void *e) { (void)e; }
static void rx_drain(void) {}
static unsigned signal_store_hub_obd_mask(void) { return 0; }
static unsigned obd_demand_mask_get(void) { return 0; }
static void vp_engine_set_obd_demand_mask(void *e, unsigned mask) { (void)e; (void)mask; }
static uint32_t vp_engine_run(void *e) { (void)e; return 1; }
static void log_stats(void) {}
#include "elm_worker.inc"

int main(void)
{
    link_time_set(&s_ready_us, INT64_C(0x123456789abcdef));
    assert(link_time_get(&s_ready_us) == INT64_C(0x123456789abcdef));
    assert(lock_entries == 2 && s_lock == 0);
    char name[40]; memset(name, 0x5A, sizeof(name));
    elm327_ble_copy_connected_name(NULL, 10);
    elm327_ble_copy_connected_name(name, 0); assert(name[0] == 0x5A);
    elm327_ble_copy_connected_name(name, 1); assert(name[0] == 0 && name[1] == 0x5A);
    elm327_ble_copy_connected_name(name, 4); assert(!strcmp(name, "OBD") && name[4] == 0x5A);
    elm327_ble_copy_connected_name(name, sizeof(name)); assert(!strcmp(name, "OBDII"));
    memset(s_target_name, 'X', sizeof(s_target_name)-1); s_target_name[31] = 0;
    elm327_ble_copy_connected_name(name, 32); assert(strlen(name) == 31 && name[32] == 0x5A);
    s_target_name[0] = 'Y'; assert(name[0] == 'X'); // caller owns the snapshot
    assert(s_lock == 0);
    fail_semaphore=true; assert(!ensure_registered() && !s_poll_wake && !shutdown_callback);
    fail_semaphore=false; assert(ensure_registered() && shutdown_callback && s_poll_wake);
    wake_task(); wake_task(); assert(s_poll_wake->token && gives==2); /* events coalesce */
    fail_create=true; start_task(); assert(!s_task_started && creates==1);
    fail_create=false; start_task(); assert(s_task_started && worker && creates==2);
    start_task(); assert(creates==2 && s_poll_wake->token); /* no duplicate worker */
    shutdown_in_wait=true;
    if (!setjmp(exit_worker)) worker(NULL);
    assert(deleted && task_deletes==1 && !s_task_started && !hub_ready && downs==1);
    wake_task(); shutdown_callback(); /* lifetime survives worker deletion */
    assert(s_poll_wake->token && takes==1);
    return 0;
}
