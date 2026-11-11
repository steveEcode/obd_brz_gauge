/* Real link worker/callback/OTA path; deterministic event/tick injection.
 * Independent ramp oracle checks both override and decoded v5/v6 wire values.
 * POSIX child per scenario gives each run a fresh firmware boot. No RF/RTOS
 * simulation: work counts do not prove ESP32 time, heap or power savings. */
#include "espnow_test_sdk.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/espnow_v6.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "vehicle/vp_manager.h"
#include "app_obd_dsp/app_event.h"
#include "app_obd_dsp/obd_data_cache.h"
#include <setjmp.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define ORIGIN INT64_C(1000000)
typedef enum {
    IDLE, LOCAL, REMOTE, V6, RESTART, THRESHOLD, SATURATE, STORM,
    PRE_TAKE, OTA_ACTIVE, OTA_IDLE, CREATE_FAIL, SLAVE, HIGH_CLOCK,
    ZERO_THRESHOLD, REMOTE_IDLE, SNAPSHOT_RESTART, STOP_COLLECT, SLAVE_FAIL, MASTER_SOURCES, SLAVE_SOURCES, SCENARIO_COUNT
} scenario_t;
static const char *const names[] = {
    "idle", "local", "remote", "v6", "restart", "threshold", "saturate", "storm",
    "before_take", "ota_active", "ota_idle", "create_fail", "slave", "64bit_clock",
    "zero_threshold", "remote_idle", "snapshot_restart", "stop_during_collect", "slave_create_fail", "master_sources", "slave_sources"
};
static scenario_t scenario;
static TaskFunction_t worker;
static esp_now_recv_cb_t recv_fn, late_recv_fn;
static SemaphoreHandle_t wake_sem;
static jmp_buf task_exit;
static int64_t now_us, origin, end_us, last_frame_us = -1, test_start_us;
static unsigned creates, deletes, packets, configs, overrides, waits, gives, lock_depth, deinit_count;
static unsigned active_packets, idle_packets, slave_requests;
static bool reference_active, override_active, deleted, pre_injected, deinitialized;
static uint16_t override_rpm, expected_rpm;
static bool expected_active, snapshot_injected;
static bool oracle(uint16_t *rpm);
static nvs_user_cfg_t cfg;
typedef enum { START_LOCAL, START_REMOTE, SET_THRESHOLD, STOP_OTA } action_t;
static struct { int64_t at; action_t action; } events[1100];
static unsigned event_count, event_index;
static unsigned slave_wait_phase, slave_invalidations, slave_data_writes;
static int32_t slave_rpm, slave_link;
static bool slave_stop_requested;
static void slave_receive(int32_t rpm);
static unsigned sources_packets;
static void slave_sources_receive(uint8_t source, uint32_t seq)
{
    uint8_t frame[ESPNOW_FRAME_MAX], mac[6] = {source};
    esp_now_recv_info_t info = {.src_addr=mac};
    tpms_wheel_data_t wheels[4] = {0};
    memcpy(wheels[0].sensor_id, "18010F", 7);
    wheels[0].bound = wheels[0].received = true;
    wheels[0].sample.data.pressure_pa = 210000;
    wheels[0].age_ms = 1000;
    size_t len = espnow_tpms_encode(seq, 15, wheels, frame, sizeof(frame));
    CHECK(len && late_recv_fn); late_recv_fn(&info, frame, (int)len);
    memset(frame, 0xa5, sizeof(frame));
}

QueueHandle_t xQueueCreateStatic(UBaseType_t capacity, UBaseType_t item_size,
                                uint8_t *bytes, StaticQueue_t *storage)
{
    CHECK(!lock_depth && capacity == 8 && item_size && bytes && storage);
    *storage = (StaticQueue_t){.bytes=bytes, .size=item_size, .capacity=capacity};
    return storage;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    CHECK(!lock_depth && q && item && !wait);
    if (q->count == q->capacity) return pdFALSE;
    memcpy(q->bytes + ((q->head + q->count) % q->capacity) * q->size, item, q->size);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    CHECK(!lock_depth && q && item && !wait);
    if (!q->count) return pdFALSE;
    memcpy(item, q->bytes + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->capacity; q->count--;
    return pdTRUE;
}

void ble_test_enter(portMUX_TYPE *mux) { CHECK(!mux->locked); mux->locked = true; lock_depth++; }
void ble_test_exit(portMUX_TYPE *mux) { CHECK(mux->locked && lock_depth); mux->locked = false; lock_depth--; }
void link_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; CHECK(!lock_depth); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
int64_t esp_timer_get_time(void) { return now_us; }
static void remote_start(esp_now_recv_cb_t cb)
{
    uint8_t mac[6] = {1};
    esp_now_recv_info_t info = {.src_addr = mac};
    uint8_t ctrl[] = {(uint8_t)ESPNOW_MAGIC, (uint8_t)(ESPNOW_MAGIC >> 8), ESPNOW_V5_VERSION, 1, 0, 0};
    CHECK(cb); cb(&info, ctrl, sizeof(ctrl));
}
static void start(bool remote)
{
    test_start_us = now_us; reference_active = true;
    if (remote) remote_start(recv_fn); else espnow_link_trigger_linked_test();
    CHECK(espnow_link_linktest_active());
}
static void inject(action_t action)
{
    switch (action) {
    case START_LOCAL: start(false); break;
    case START_REMOTE: start(true); break;
    case SET_THRESHOLD: cfg.rpm_warn_threshold = 9000; break;
    case STOP_OTA: {
        CHECK(!espnow_link_pause_for_ota());
        CHECK(!deleted && !deinit_count && !espnow_link_linktest_active());
        unsigned old_gives = gives;
        espnow_link_trigger_linked_test(); remote_start(late_recv_fn);
        CHECK(!espnow_link_linktest_active() && gives == old_gives);
        CHECK(!espnow_link_pause_for_ota());
        CHECK(!deletes && !deinit_count);
        break;
    }
    }
}
static void event(int64_t relative, action_t action)
{
    CHECK(event_count < sizeof(events) / sizeof(events[0]));
    events[event_count].at = origin + relative;
    events[event_count++].action = action;
}
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{ CHECK(!wake_sem); storage->token = false; wake_sem = storage; return storage; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{ CHECK(sem == wake_sem && !lock_depth); gives++; sem->token = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    CHECK(sem == wake_sem && !lock_depth && ticks && ticks <= ((scenario == SLAVE || scenario == SLAVE_SOURCES) ? 250u : 100u));
    if (scenario == SLAVE_SOURCES) {
        tpms_wheel_data_t wheels[4];
        CHECK(++waits < 16);
        if (sem->token) { sem->token = false; return pdTRUE; }
        espnow_link_tpms_snapshot(wheels);
        switch (slave_wait_phase++) {
        case 0: // Queued old master was rejected after the binding change.
            CHECK(!wheels[0].bound);
            slave_sources_receive(2, 2);
            espnow_link_tpms_snapshot(wheels); CHECK(!wheels[0].bound); // callback only queued
            break;
        case 1:
            CHECK(wheels[0].fresh && wheels[0].age_ms == 1000 && wheels[0].sample.data.pressure_pa == 210000);
            now_us += 500000;
            espnow_link_tpms_snapshot(wheels); CHECK(wheels[0].age_ms == 1500);
            slave_sources_receive(2, 1); // out-of-order must not reset the age
            break;
        case 2:
            CHECK(wheels[0].age_ms == 1500);
            now_us += 1500000;
            espnow_link_tpms_snapshot(wheels); CHECK(!wheels[0].received); // link expiry, not 120s
            slave_sources_receive(2, 1); // reboot after silence
            break;
        case 3: {
            CHECK(wheels[0].fresh);
            uint8_t mac[6] = {3}; espnow_link_bind_master(mac);
            espnow_link_tpms_snapshot(wheels); CHECK(!wheels[0].received);
            slave_sources_receive(2, 3); // bound filter, cannot revive former master
            break;
        }
        case 4:
            CHECK(!wheels[0].bound);
            slave_sources_receive(3, 1);
            break;
        case 5:
            CHECK(wheels[0].fresh);
            CHECK(!espnow_link_pause_for_ota()); // worker owns its exit and cache clear
            break;
        default: CHECK(false);
        }
        sem->token = false;
        return pdTRUE;
    }
    if (scenario == SLAVE) {
        CHECK(++waits < 10);
        if (sem->token) { sem->token = false; return pdTRUE; }
        unsigned phase = slave_wait_phase++;
        if (phase == 0) {
            CHECK(!slave_data_writes && !espnow_link_slave_has_data()); // binding changed after enqueue
            uint8_t mac[6] = {1}; espnow_link_bind_master(mac);
            slave_receive(7777);
            now_us += 2000000; // expire the queued packet before the worker gets to it
            sem->token = false;
            return pdTRUE;
        }
        if (phase == 1) {
            CHECK(!slave_data_writes && !espnow_link_slave_has_data());
            for (int i=0; i<10; i++) slave_receive(1000 + i); // bounded FIFO
            sem->token = false;
            return pdTRUE;
        }
        if (phase == 2) {
            CHECK(slave_rpm == 1007 && slave_link == SIG_LINK_DATA && slave_data_writes == 8);
            CHECK(espnow_link_slave_has_data());
            char name[16]; memset(name, 0x5A, sizeof(name));
            espnow_link_copy_master_name(NULL, 10);
            espnow_link_copy_master_name(name, 0); CHECK(name[0] == 0x5A);
            espnow_link_copy_master_name(name, 1); CHECK(name[0] == 0 && name[1] == 0x5A);
            espnow_link_copy_master_name(name, 4); CHECK(!strcmp(name, "Rad") && name[4] == 0x5A);
            espnow_link_copy_master_name(name, sizeof(name)); CHECK(!strcmp(name, "RadioMaster"));
            CHECK(name[12] == 0x5A);
            slave_receive(2222);
            CHECK(slave_rpm == 1007); // callback only queues, never reenters the store
            sem->token = false;
            return pdTRUE;
        }
        CHECK(phase == 3 && slave_rpm == 2222);
        now_us += 2000000; // timeout clears the old values on this same worker
        return pdFALSE;
    }
    CHECK(++waits < 100000);  // a stale token/event must not cause a busy loop
    if (scenario == PRE_TAKE && !pre_injected) { pre_injected = true; start(false); }
    if (sem->token) { sem->token = false; return pdTRUE; }
    int64_t deadline = now_us + (int64_t)ticks * 1000;
    while (event_index < event_count && events[event_index].at <= deadline) {
        now_us = events[event_index].at;
        inject(events[event_index++].action);
        if (deleted) longjmp(task_exit, 1);
        if (sem->token) { sem->token = false; return pdTRUE; }
    }
    if (deadline >= end_us) { now_us = end_us; longjmp(task_exit, 1); }
    now_us = deadline;
    return pdFALSE;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)arg; CHECK(!lock_depth && priority == 4 && !handle); creates++;
    CHECK(creates == 1);  // no independent 3KB link-test task
    if (scenario == SLAVE || scenario == SLAVE_FAIL || scenario == SLAVE_SOURCES) { CHECK(!strcmp(name, "espnow_pres") && stack == 3584 && !handle); }
    else CHECK(!strcmp(name, "espnow_tx") && stack == 3584);
    if (scenario == CREATE_FAIL || scenario == SLAVE_FAIL) return pdFALSE;
    worker = fn; if (handle) *handle = &worker; return pdPASS;
}
void vTaskDelete(TaskHandle_t task) {
    CHECK(task == NULL && !lock_depth);
    deletes++; deleted = true;
    longjmp(task_exit, 1);
}
void vTaskDelay(TickType_t ticks) { (void)ticks; CHECK(false); }
nvs_user_cfg_t nvs_cfg_read(void)
{ CHECK(!lock_depth); configs++; expected_active = oracle(&expected_rpm); return cfg; }
esp_err_t nvs_cfg_snapshot(nvs_user_cfg_t *out) { CHECK(!lock_depth); *out = cfg; return ESP_OK; }
esp_err_t nvs_cfg_update(const nvs_user_cfg_t *base, const nvs_user_cfg_t *updated)
{ (void)base; cfg = *updated; return ESP_OK; }
uint8_t nvs_device_position_get(void) { return 1; }
static bool oracle(uint16_t *rpm)
{
    int64_t ms = (now_us - test_start_us) / 1000;
    if (!reference_active || ms < 0 || ms >= 8300) { reference_active = false; *rpm = 0; return false; }
    uint64_t peak = (uint32_t)cfg.rpm_warn_threshold + 200;
    uint64_t result = ms < 5000 ? peak * (uint64_t)ms / 5000 :
        ms < 5800 ? peak : peak - peak * (uint64_t)(ms - 5800) / 2500;
    *rpm = result > UINT16_MAX ? UINT16_MAX : (uint16_t)result;
    return true;
}
void obd_data_rpm_override_set(bool enabled, uint16_t rpm)
{ CHECK(!lock_depth); override_active = enabled; override_rpm = rpm; overrides++; }
int ui_sweep_get_step(void) { return 77; }
int ui_intro_get_step(void) { return 9; }
const vp_profile_t *vp_manager_active(void) { return NULL; }
const char *vp_manager_active_id(void) { return "test.profile"; }
uint64_t signal_store_get_many(const int *slots, size_t count, int32_t *values)
{
    CHECK(!lock_depth);
    if (scenario == STOP_COLLECT && !snapshot_injected) {
        snapshot_injected = true;
        CHECK(!espnow_link_pause_for_ota() && !deleted && !deinit_count);
    }
    if (scenario == SNAPSHOT_RESTART && now_us == origin + 8300000 && !snapshot_injected) {
        snapshot_injected = true; start(true); // after completion sample, before packet assembly
    }
    uint64_t valid = 0;
    for (size_t i = 0; i < count; i++) {
        values[i] = 0;
        if (slots[i] == SIG_OBD_RPM) { values[i] = 1234; valid |= UINT64_C(1) << i; }
        if (slots[i] == SIG_SYS_OBD_LINK) { values[i] = SIG_LINK_DATA; valid |= UINT64_C(1) << i; }
    }
    return valid;
}
int signal_store_veh_count(void) { return 0; }
int signal_store_slot_by_id(uint16_t id) { for (int i = 0; i < SIG_STD_COUNT; i++) if (sig_registry[i].num_id == id) return i; return SIG_SLOT_NONE; }
bool signal_store_is_local(int slot) { (void)slot; return false; }
bool signal_store_set(int slot, int32_t value) {
    CHECK(!lock_depth);
    if (scenario == SLAVE && slot == SIG_SYS_OBD_LINK) slave_link = value;
    return true;
}
void signal_store_set_many(const int *slots, const int32_t *values, size_t count)
{
    CHECK(!lock_depth);
    if (scenario != SLAVE) return;
    for (size_t i=0; i<count; i++) if (slots[i] == SIG_OBD_RPM) {
        slave_rpm = values[i]; slave_data_writes++;
        if (slave_rpm == 3333) {
            CHECK(!espnow_link_pause_for_ota());
            CHECK(!deleted && !deinit_count); // never kill a worker inside signal_store
            slave_stop_requested = true;
        }
    }
}
void signal_store_invalidate(int slot) { (void)slot; }
void signal_store_invalidate_many(const int *slots, size_t count) { (void)slots; (void)count; }
void signal_store_invalidate_vehicle(void) {
    CHECK(!lock_depth);
    if (scenario != SLAVE) return;
    slave_invalidations++;
    if (slave_invalidations == 1) {
        unsigned before = slave_data_writes;
        slave_receive(3333); // receive during timeout invalidation
        CHECK(slave_data_writes == before); // callback cannot interleave writes
    }
    slave_rpm = 0;
}
bool app_event_send(app_event_type_t type, uint32_t data) { (void)type; (void)data; return true; }

void *heap_caps_malloc(size_t bytes, unsigned caps) { (void)caps; return malloc(bytes); }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { (void)storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { (void)mode; return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t esp_wifi_set_channel(uint8_t channel, int secondary) { (void)channel; (void)secondary; return ESP_OK; }
esp_err_t esp_wifi_set_ps(int ps) { (void)ps; return ESP_OK; }
esp_err_t esp_now_init(void) { return ESP_OK; }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *peer) { (void)peer; return ESP_OK; }
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { recv_fn = late_recv_fn = cb; return ESP_OK; }
esp_err_t esp_now_unregister_recv_cb(void) { recv_fn = NULL; return ESP_OK; }
esp_err_t esp_now_unregister_send_cb(void) { return ESP_OK; }
esp_err_t esp_now_deinit(void) { deinit_count++; deinitialized = true; return ESP_OK; }
esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    (void)mac; CHECK(!lock_depth && !deinitialized);
    if (scenario == SLAVE || scenario == SLAVE_SOURCES) {
        espnow_pkt_kind_t kind = espnow_classify(data, len);
        CHECK(kind == ESPNOW_PKT_V5_CTRL || kind == ESPNOW_PKT_V5_PRESENCE || kind == ESPNOW_PKT_V6_HELLO);
        if (scenario == SLAVE_SOURCES && kind == ESPNOW_PKT_V6_HELLO) {
            espnow_demand_t d;
            CHECK(espnow_sources_hello_decode(data, len, NULL, NULL, &d));
            CHECK(d.tpms == 15 && d.obd == 1 && d.veh == UINT32_C(0x80000000));
        }
        if (kind == ESPNOW_PKT_V5_CTRL) { CHECK(len == 6 && data[3] == 1); slave_requests++; }
        return ESP_OK;
    }
    if (scenario == MASTER_SOURCES && espnow_classify(data, len) == ESPNOW_PKT_V6_TPMS) {
        uint8_t mask; tpms_wheel_data_t wheels[4];
        CHECK(espnow_tpms_decode(data, len, NULL, &mask, wheels));
        CHECK(mask == 5 && now_us == origin + (int64_t)sources_packets * 500000);
        CHECK(wheels[0].fresh && wheels[0].sample.data.pressure_pa == 210000);
        CHECK(wheels[2].bound && !wheels[1].bound);
        sources_packets++;
        return ESP_OK;
    }
    const uint16_t rpm = expected_rpm;
    const bool active = expected_active;
    espnow_rx_frame_t frame;
    CHECK((scenario == V6 ? espnow_v6_decode(data, len, &frame) : espnow_v5_decode(data, len, &frame)));
    CHECK(frame.flags == (ESPNOW_FLAG_ELM | ESPNOW_FLAG_LINKED_EN | (active ? ESPNOW_FLAG_LINKTEST : 0)));
    CHECK(frame.sweep_step == 77 && frame.intro_step == 9);
    bool found = false;
    for (int i = 0; i < frame.n_rec; i++) {
        uint16_t id; int32_t value;
        CHECK(espnow_rx_record(&frame, i, &id, &value));
        if (id == ESPNOW_ID_RPM) { CHECK(value == (active ? rpm : 1234)); found = true; }
    }
    CHECK(found && override_active == active && override_rpm == (active ? rpm : 0));
    if (last_frame_us >= 0) CHECK(now_us - last_frame_us >= 20000);
    if (scenario == IDLE) CHECK(now_us == origin + (int64_t)packets * 100000);
    if (scenario == LOCAL || scenario == REMOTE || scenario == V6 || scenario == SATURATE || scenario == HIGH_CLOCK || scenario == ZERO_THRESHOLD) {
        int64_t offset = packets <= 415 ? (int64_t)packets * 20000 : 8300000 + (int64_t)(packets - 415) * 100000;
        CHECK(now_us == origin + offset);
    }
    if (scenario == PRE_TAKE) CHECK(now_us == origin + (int64_t)packets * 20000);
    last_frame_us = now_us;
    packets++; if (active) active_packets++; else idle_packets++;
    return ESP_OK;
}
static void slave_receive(int32_t rpm)
{
    espnow_snapshot_t snapshot;
    uint8_t frame[ESPNOW_FRAME_MAX], mac[6] = {1};
    esp_now_recv_info_t info = {.src_addr = mac};
    espnow_snapshot_init(&snapshot);
    snapshot.flags = ESPNOW_FLAG_ELM;
    memcpy(snapshot.name, "RadioMaster", 12);
    espnow_snapshot_put(&snapshot, ESPNOW_ID_RPM, rpm);
    size_t len = rpm == 3333 ? espnow_v6_encode(&snapshot, 0, frame, sizeof(frame))
                             : espnow_v5_encode(&snapshot, frame, sizeof(frame));
    CHECK(len > 0 && late_recv_fn); late_recv_fn(&info, frame, (int)len);
    memset(frame, 0xA5, sizeof(frame)); // caller's buffer is no longer valid after the callback
}
static void setup(void)
{
    origin = scenario == HIGH_CLOCK ? INT64_C(5000000000000) : ORIGIN;
    now_us = origin; end_us = origin + 9000000;
    cfg.rpm_warn_threshold = scenario == SATURATE ? UINT16_MAX : scenario == ZERO_THRESHOLD ? 0 : 7000;
    cfg.rpm_warn_linked_en = 1;
    if (scenario == SLAVE_SOURCES) {
        cfg.device_role = ESPNOW_ROLE_SLAVE;
        espnow_demand_t d = {.obd=1, .veh=UINT32_C(0x80000000), .tpms=15};
        espnow_link_set_data_demand(&d);
        espnow_link_start_slave();
        slave_sources_receive(1, 1);
        uint8_t mac[6] = {2}; espnow_link_bind_master(mac);
        return;
    }
    if (scenario == SLAVE_FAIL) {
        espnow_link_start_slave(); CHECK(!worker && !espnow_link_slave_has_data());
        slave_receive(1234); CHECK(!slave_data_writes && !espnow_link_slave_has_data());
        CHECK(espnow_link_pause_for_ota() && deinit_count == 1); return;
    }
    if (scenario == SLAVE) {
        espnow_link_start_slave();
        CHECK(!espnow_link_slave_has_data());
        now_us = INT64_C(5000000000000);
        slave_receive(6666);
        uint8_t new_master[6] = {2}; espnow_link_bind_master(new_master);
        CHECK(!slave_data_writes && !espnow_link_slave_has_data());
        espnow_link_trigger_linked_test();
        CHECK(slave_requests == 1 && !espnow_link_linktest_active()); return;
    }
    espnow_link_start_master(); CHECK(creates == 1 && wake_sem);
    if (scenario == CREATE_FAIL) {
        espnow_link_trigger_linked_test(); remote_start(recv_fn);
        CHECK(!espnow_link_linktest_active() && !gives && !worker);
        espnow_link_pause_for_ota(); CHECK(!deletes && deinit_count == 1); return;
    }
    switch (scenario) {
    case MASTER_SOURCES: {
        uint8_t mac[6] = {1}, hello[15]; esp_now_recv_info_t info = {.src_addr=mac};
        espnow_demand_t d = {.obd=2, .veh=1, .tpms=1};
        size_t len = espnow_sources_hello_encode(1, &d, hello, sizeof(hello)); recv_fn(&info, hello, (int)len);
        mac[0] = 2; d = (espnow_demand_t){.obd=4, .veh=UINT32_C(0x80000000), .tpms=4};
        len = espnow_sources_hello_encode(2, &d, hello, sizeof(hello)); recv_fn(&info, hello, (int)len);
        espnow_demand_t merged = espnow_link_remote_data_demand();
        CHECK(merged.obd == 6 && merged.veh == UINT32_C(0x80000001) && merged.tpms == 5);
        d.tpms = 8; len = espnow_sources_hello_encode(2, &d, hello, sizeof(hello)); recv_fn(&info, hello, (int)len);
        CHECK(espnow_link_remote_data_demand().tpms == 9); // replace, not OR forever
        d.tpms = 4; len = espnow_sources_hello_encode(2, &d, hello, sizeof(hello)); recv_fn(&info, hello, (int)len);
        mac[0] = 3; uint8_t presence[] = {(uint8_t)ESPNOW_MAGIC, (uint8_t)(ESPNOW_MAGIC >> 8), 5, 3};
        recv_fn(&info, presence, sizeof(presence)); // mixed legacy peer must not suppress extensions
        end_us = origin + 3000000;
        break;
    }
    case IDLE: end_us = origin + 1000000; break;
    case REMOTE: start(true); break;
    case V6: {
        uint8_t mac[6] = {1}, hello[ESPNOW_V6_HELLO_LEN + 2];
        esp_now_recv_info_t info = {.src_addr = mac};
        recv_fn(&info, hello, (int)espnow_hello_encode_demand(
            1, ESPNOW_CAP_V6_DATA | ESPNOW_CAP_DEMAND_MASK,
            (uint16_t)((1u << SIG_OBD_SPEED) | (1u << SIG_OBD_BOOST)),
            hello, sizeof(hello)));
        CHECK(espnow_master_online_slaves() == 1);
        CHECK(espnow_link_remote_demand_mask() ==
              (uint16_t)((1u << SIG_OBD_SPEED) | (1u << SIG_OBD_BOOST)));
        now_us += 2000000;
        CHECK(espnow_master_online_slaves() == 0);
        now_us = origin; /* restore virtual clock before starting the worker */
        end_us = origin + 1000000; start(false); break;
    }
    case RESTART: start(false); event(8299000, START_REMOTE); end_us = origin + 17000000; break;
    case THRESHOLD: start(false); event(4999000, SET_THRESHOLD); break;
    case STORM:
        start(false); end_us = origin + 1000000;
        for (int64_t t = 1000; t < 1000000; t += 1000) event(t, START_REMOTE);
        break;
    case PRE_TAKE: end_us = origin + 1000000; break;
    case REMOTE_IDLE: event(23456, START_REMOTE); end_us = origin + 1000000; break;
    case SNAPSHOT_RESTART: start(false); end_us = origin + 17000000; break;
    case OTA_ACTIVE: start(false); event(123456, STOP_OTA); break;
    case OTA_IDLE: event(123456, STOP_OTA); break;
    default: start(false); break;
    }
}
static void run(scenario_t value)
{
    scenario = value; setup();
    if (worker) { if (!setjmp(task_exit)) worker(NULL); }
    if (scenario == SLAVE) {
        CHECK(slave_stop_requested && deleted && deletes == 1 && slave_invalidations == 2);
        CHECK(slave_data_writes == 10 && !slave_rpm && slave_link == SIG_LINK_NONE);
        CHECK(!espnow_link_slave_has_data() && espnow_link_pause_for_ota() && deinit_count == 1);
        unsigned before = slave_data_writes;
        slave_receive(9999); // retained late callback must not revive a stopped worker
        CHECK(slave_data_writes == before && !espnow_link_slave_has_data());
    }
    if (scenario == MASTER_SOURCES) {
        CHECK(sources_packets == 4 && packets == 30);
        espnow_demand_t d = espnow_link_remote_data_demand();
        CHECK(!d.obd && !d.veh && !d.tpms);
    }
    if (scenario == SLAVE_SOURCES) {
        tpms_wheel_data_t wheels[4]; espnow_link_tpms_snapshot(wheels);
        CHECK(!wheels[0].received && deleted && deletes == 1);
        CHECK(espnow_link_pause_for_ota() && deinit_count == 1);
    }
    CHECK(!lock_depth);
    if (scenario != SLAVE && scenario != SLAVE_SOURCES && scenario != CREATE_FAIL && scenario != STOP_COLLECT) CHECK(configs == packets);
    if (scenario == IDLE) CHECK(packets == 10 && waits == 10 && !overrides);
    if (scenario == LOCAL || scenario == REMOTE || scenario == SATURATE || scenario == HIGH_CLOCK || scenario == ZERO_THRESHOLD)
        CHECK(active_packets == 415 && idle_packets == 7 && overrides == 416);
    if (scenario == V6) CHECK(packets == 50 && active_packets == 50);
    if (scenario == STORM || scenario == PRE_TAKE) CHECK(packets == 50);
    if (scenario == REMOTE_IDLE) CHECK(packets == 50 && active_packets == 49 && last_frame_us == origin + 983456);
    if (scenario == SNAPSHOT_RESTART) CHECK(snapshot_injected && active_packets == 829 && idle_packets == 5);
    if (scenario == STOP_COLLECT) CHECK(snapshot_injected && configs == 1 && !packets);
    if (scenario == OTA_ACTIVE || scenario == OTA_IDLE || scenario == STOP_COLLECT) {
        CHECK(deleted && !override_active && !override_rpm);
        CHECK(espnow_link_pause_for_ota() && deinit_count == 1);
        CHECK(espnow_link_pause_for_ota() && deinit_count == 1);
    }
    printf("%-14s packets=%u cfg=%u override=%u waits=%u gives=%u tasks=%u\n",
           names[value], packets, configs, overrides, waits, gives, creates);
    fflush(stdout);
}
int main(void)
{
    for (unsigned i = 0; i < SCENARIO_COUNT; i++) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { run((scenario_t)i); _exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("21 real ESP-NOW worker/callback/lifecycle scenarios passed");
    return 0;
}

void tpms_ble_snapshot(tpms_wheel_data_t out[TPMS_WHEEL_COUNT]) {
    CHECK(!lock_depth);
    memset(out, 0, TPMS_WHEEL_COUNT * sizeof(*out));
    // A slave deliberately has different local bindings: they must never leak
    // into delivered data when its master is absent or its cache is cleared.
    unsigned wheel = scenario == SLAVE_SOURCES ? 3 : 0;
    memcpy(out[wheel].sensor_id, "18010F", 7);
    out[wheel].bound = out[wheel].received = out[wheel].fresh = true;
    out[wheel].sample.data.pressure_pa = 210000;
    out[2].bound = true; memcpy(out[2].sensor_id, "ABCDEF", 7);
}
