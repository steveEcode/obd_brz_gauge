// Three-gauge ESP-NOW link: the master reads OBD then broadcasts to slaves.
// One master, many slaves, broadcast (1-to-many), coexists with BLE (master).
// Wire formats (v5 legacy + v6 TLV) and the capability handshake: espnow_v6.h.
//
// Master: snapshots the valid obd.* / veh.* values from signal_store every
//   100 ms (20 ms during the linked test) and sends them as v6 fragments when every online slave announced
//   v6 support (HELLO), otherwise as one v5 packet (old slaves keep working).
// TPMS: page subscriptions use extended HELLO; type 3 snapshots every 500 ms
//   remain independent of legacy DATA fallback. System sources stay local.
// Slave: accepts v5 and v6 DATA, writes signal_store, drives sys.obd_link and
//   invalidates obd.* / veh.* when the master goes silent.

#include "espnow_link.h"
#include "espnow_v6.h"
#include "tpms_ble_client.h"
#include "app_obd_dsp/app_event.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/signal_store.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "vehicle/vp_manager.h"

// Sweep / boot-animation sync: read from the master (implemented in ui.c to avoid re-including lvgl here)
extern int  ui_sweep_get_step(void);
extern int  ui_intro_get_step(void);

#define TAG "espnow_link"

#define ESPNOW_CHANNEL          1       // master and slaves must share a channel (fixed here when STA is not connected to an AP)
#define MASTER_NAME_LEN         12
static const char MASTER_NAME[] = "SkyGauge";   // name the master broadcasts (shown on slaves); could become configurable later
#define BROADCAST_INTERVAL_MS   100     // master broadcast period (10Hz, plenty for gauges)
#define LINKTEST_INTERVAL_MS    20      // master broadcast period during the linked test (smooth ramp)
#define PRESENCE_INTERVAL_MS    500     // slave presence + HELLO report period
#define SLAVE_TICK_MS           250     // slave housekeeping period (presence, link timeout)
#define MG_MAX_SLAVES           4       // max slaves the master tracks
#define ESPNOW_TIMEOUT_US       2000000 // slave online window (master) / master silent -> link lost (slave)
_Static_assert(ESPNOW_TIMEOUT_US == ESPNOW_SOURCE_TIMEOUT_US, "shared source timeout");
_Static_assert(NVS_TPMS_WHEELS == TPMS_WHEEL_COUNT, "TPMS wheel order/count");
_Static_assert(ESPNOW_TPMS_FRAME_LEN <= ESPNOW_FRAME_MAX, "TPMS fits one frame");

_Static_assert(sizeof(MASTER_NAME) <= MASTER_NAME_LEN, "master name too long");
_Static_assert(MASTER_NAME_LEN == ESPNOW_NAME_LEN, "wire name length");
_Static_assert(ESPNOW_FRAME_MAX <= ESP_NOW_MAX_DATA_LEN, "v6 frames must fit one ESP-NOW payload");

static bool s_is_master = false;
static bool s_espnow_started = false;  // Track if ESP-NOW was actually started

// Slave -> master presence packet (v5; still sent by v6 slaves so old masters count them)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  position;   // slave position 1/2/3
} espnow_presence_t;
_Static_assert(sizeof(espnow_presence_t) == ESPNOW_V5_PRESENCE_LEN, "v5 presence layout");

// Multi-gauge linked control packet (slave <-> master, v5 so mixed firmware keeps syncing; dispatched by length)
#define ESPNOW_CTRL_TEST_START   1   // ask the master to start the linked test (arg unused)
#define ESPNOW_CTRL_THRESH_SET   2   // sync the RPM warning threshold (arg = threshold)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint16_t arg;
} espnow_ctrl_packet_t;
_Static_assert(sizeof(espnow_ctrl_packet_t) == ESPNOW_V5_CTRL_LEN, "v5 ctrl layout");

// Master side: linked-test RPM ramp state (the master is the single RPM injection source; slaves follow via broadcast)
// The WiFi callback/UI only restart the timeline. The sole master worker
// computes both the display override and the coherent broadcast RPM/flag.
static portMUX_TYPE s_linktest_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_master_running = false;
static bool s_linktest_accepting = false;  // closed before OTA asks the worker to exit
static bool s_linktest_active = false;
static int64_t s_linktest_start_us = 0;
// Boot-lifetime storage: a late callback cannot notify a deleted task handle.
static StaticSemaphore_t s_master_wake_storage;
static SemaphoreHandle_t s_master_wake = NULL;
typedef struct { bool active; uint16_t rpm; } linktest_sample_t;
static volatile bool s_rx_linktest = false;   // slave side: last received "linked test in progress" flag from the master
static volatile bool s_rx_linked_en = false;  // slave side: master's rpm_warn_linked_en (mirrored from broadcast flags bit2)
#define LINKTEST_RISE_MS 5000   // 0 -> peak, slow rise
#define LINKTEST_HOLD_MS 800    // hold at peak (flash)
#define LINKTEST_FALL_MS 2500   // peak -> 0, fall back

// Master side: online slave records (last_v6_us = last HELLO announcing v6 DATA support)
static struct { uint8_t mac[6]; uint8_t position; int64_t last_us; int64_t last_v6_us;
                int64_t last_demand_us; espnow_demand_t demand; } s_slaves[MG_MAX_SLAVES];
static portMUX_TYPE s_slaves_mux = portMUX_INITIALIZER_UNLOCKED;
static espnow_demand_t s_local_demand;
static espnow_tpms_cache_t s_tpms_cache;
/* Cache shares the binding lock: rebinding and clearing delivered TPMS are
 * atomic, and a previously queued source cannot revive the old binding. */
static espnow_snapshot_t *s_snap = NULL;   // master tx snapshot (PSRAM, allocated once)
static bool s_tx_v6 = false;               // master: currently sending v6

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static int64_t s_last_rx_us = 0;
static portMUX_TYPE s_last_rx_mux = portMUX_INITIALIZER_UNLOCKED;
/* Callback-owned payloads expire on return. Copy a bounded burst into a
 * boot-lifetime queue; only the slave worker applies signals or times out. */
#define SLAVE_RX_QUEUE_LEN 8
typedef struct {
    int64_t received_us;
    uint16_t len;
    uint8_t mac[6];
    uint8_t data[ESPNOW_FRAME_MAX];
} slave_packet_t;
static StaticQueue_t s_slave_queue_storage;
static uint8_t s_slave_queue_bytes[SLAVE_RX_QUEUE_LEN * sizeof(slave_packet_t)];
static QueueHandle_t s_slave_queue;
static StaticSemaphore_t s_slave_wake_storage;
static SemaphoreHandle_t s_slave_wake;
static bool s_slave_accepting;
static bool s_slave_running;
static bool s_slave_stop;
static uint32_t s_slave_queue_dropped;

static volatile bool s_rx_live = false;     // slave: sys.obd_link reflects a live master
static volatile uint8_t s_rx_link = SIG_LINK_NONE;   // slave: last sys.obd_link value written
static bool s_rx_veh_ok = false;            // slave: last frame carried veh.* for our profile
static bool s_rx_sweep_valid = false;
static bool s_rx_intro_valid = false;
static uint8_t s_rx_sweep_step = 0;
static uint8_t s_rx_intro_step = 0;
static uint32_t s_tx_seq = 0;
static uint32_t s_local_profile_hash = 0;

// Slave receive callback runs for every ESP-NOW frame. The binding changes only
// through the explicit bind/unbind APIs, so keep the six-byte filter in RAM
// instead of taking the NVS config mutex and copying the whole config per frame.
static uint8_t s_bound_master_mac[6] = {0};
static bool s_bound_master_mac_active = false;
static portMUX_TYPE s_bound_master_mac_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_master_name[MASTER_NAME_LEN] = {0};  // slave side: name of the last master heard

// ---- WiFi + ESP-NOW low-level init (shared by master and slave) ----
static void wifi_espnow_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    esp_wifi_set_ps(WIFI_PS_NONE);   // disable power save, otherwise ESP-NOW RX drops/lags
    ESP_ERROR_CHECK(esp_now_init());
    s_espnow_started = true;
}

static void add_broadcast_peer(void) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static void link_send(const void *data, size_t len) {
    if (len == 0) return;
    esp_err_t r = esp_now_send(s_broadcast_mac, (const uint8_t *)data, len);
    if (r != ESP_OK) ESP_LOGW(TAG, "esp_now_send err=%d", r);
}

// Profile hash of this gauge (FNV-1a of active id + rev). The active profile is
// stable for the whole boot; cache it when the link starts so every broadcast and
// received frame avoids repeating the profile lookup and hash walk.
static void cache_local_profile_hash(void)
{
    const vp_profile_t *p = vp_manager_active();
    s_local_profile_hash = espnow_profile_hash(vp_manager_active_id(), p ? p->rev : 0);
}

static inline uint32_t local_profile_hash(void)
{
    return s_local_profile_hash;
}

static void cache_bound_master_mac(void)
{
    nvs_user_cfg_t cfg;
    nvs_cfg_snapshot(&cfg);
    portENTER_CRITICAL(&s_bound_master_mac_mux);
    memset(&s_tpms_cache, 0, sizeof(s_tpms_cache));
    memcpy(s_bound_master_mac, cfg.espnow_master_mac, sizeof(s_bound_master_mac));
    s_bound_master_mac_active = s_bound_master_mac[0] != 0;
    portEXIT_CRITICAL(&s_bound_master_mac_mux);
}

// ========================= Master =========================
static void master_collect(espnow_snapshot_t *s, const nvs_user_cfg_t *cfg,
                           linktest_sample_t test) {

    espnow_snapshot_init(s);
    s->seq          = ++s_tx_seq;
    s->profile_hash = local_profile_hash();
    // bit0 = data flowing (sys.obd_link == 2); bit1 = linked test in progress; bit2 = rpm_warn_linked_en (slaves mirror it)
    // Read sys.obd_link together with the OBD range so this 10 Hz path enters
    // signal_store only once for both the link flag and the payload values.
    enum { SNAP_LINK_INDEX = SIG_OBD_COUNT };
    enum { SNAP_BATCH_CAP = (SIG_OBD_COUNT + 1) > SIG_VEH_MAX ? (SIG_OBD_COUNT + 1) : SIG_VEH_MAX };
    int slots[SNAP_BATCH_CAP];
    int32_t values[SNAP_BATCH_CAP];
    for (int i = 0; i < SIG_OBD_COUNT; i++) slots[i] = SIG_OBD_FIRST + i;
    slots[SNAP_LINK_INDEX] = SIG_SYS_OBD_LINK;
    uint64_t valid = signal_store_get_many(slots, SIG_OBD_COUNT + 1, values);
    const bool link = (valid & (UINT64_C(1) << SNAP_LINK_INDEX)) && values[SNAP_LINK_INDEX] == SIG_LINK_DATA;
    s->flags = (uint8_t)((link ? ESPNOW_FLAG_ELM : 0)
                       | (test.active ? ESPNOW_FLAG_LINKTEST : 0)
                       | (cfg->rpm_warn_linked_en ? ESPNOW_FLAG_LINKED_EN : 0));
    s->sweep_step = (uint8_t)ui_sweep_get_step();   // broadcast sweep progress for slave sync
    s->intro_step = (uint8_t)ui_intro_get_step();   // broadcast boot-animation progress for slave sync
    memcpy(s->name, MASTER_NAME, sizeof(MASTER_NAME));

    // The first batch also includes sys.obd_link. A profile change between the
    // two batches is harmless: removed slots are reported invalid by the store.
    // A fresh snapshot visits every OBD slot once; registry IDs are unique.
    // Dynamic IDs below are disjoint from the static OBD range.
    for (int i = 0; i < SIG_OBD_COUNT; i++) {
        if (valid & (UINT64_C(1) << i)) {
            espnow_snapshot_append_unique(s, sig_registry[slots[i]].num_id, values[i]);
        }
    }

    int n_veh = signal_store_veh_count();
    if (n_veh > SIG_VEH_MAX) n_veh = SIG_VEH_MAX;
    for (int i = 0; i < n_veh; i++) slots[i] = SIG_VEH_BASE + i;
    valid = signal_store_get_many(slots, (size_t)n_veh, values);
    for (int i = 0; i < n_veh; i++) {
        if (valid & (UINT64_C(1) << i)) {
            const uint16_t num_id = (uint16_t)(SIG_VEH_ID_BASE + i);
            espnow_snapshot_append_unique(s, num_id, values[i]);
        }
    }
    // Linked test: slaves follow the master's simulated RPM ramp (the master shows it via the override layer).
    if (test.active) espnow_snapshot_put(s, ESPNOW_ID_RPM, test.rpm);
}

// v6 only while at least one slave is online and every online slave announced v6 DATA support.
static bool master_slaves_all_v6(void) {
    portENTER_CRITICAL(&s_slaves_mux);
    int64_t now = esp_timer_get_time();
    int online = 0;
    bool all_v6 = true;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us == 0 || (now - s_slaves[i].last_us) >= ESPNOW_TIMEOUT_US) continue;
        online++;
        if (s_slaves[i].last_v6_us == 0 || (now - s_slaves[i].last_v6_us) >= ESPNOW_TIMEOUT_US) {
            all_v6 = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_slaves_mux);
    return online > 0 && all_v6;
}

// Restart requests coalesce into the latest timeline; no signal-store or
// radio calls under this lock. Reject requests once OTA begins quiescing.
static bool master_start_linktest(void)
{
    bool accepted;
    portENTER_CRITICAL(&s_linktest_mux);
    accepted = s_linktest_accepting;
    if (accepted) {
        s_linktest_start_us = esp_timer_get_time();
        s_linktest_active = true;
    }
    portEXIT_CRITICAL(&s_linktest_mux);
    if (accepted) xSemaphoreGive(s_master_wake);
    return accepted;
}

static bool master_linktest_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_linktest_mux);
    active = s_linktest_active;
    portEXIT_CRITICAL(&s_linktest_mux);
    return active;
}

// Called once per broadcast, using the same NVS snapshot as the packet flags.
// A restart cannot tear the 64-bit timestamp or be overwritten by completion.
static linktest_sample_t master_sample_linktest(uint16_t threshold)
{
    linktest_sample_t sample = {0};
    portENTER_CRITICAL(&s_linktest_mux);
    if (s_linktest_active) {
        const uint32_t peak = (uint32_t)threshold + 200;
        const int64_t el_ms = (esp_timer_get_time() - s_linktest_start_us) / 1000;
        if (el_ms < 0 || el_ms >= LINKTEST_RISE_MS + LINKTEST_HOLD_MS + LINKTEST_FALL_MS) {
            s_linktest_active = false;
        } else {
            uint32_t rpm;
            if (el_ms < LINKTEST_RISE_MS) {
                /* peak <= 65735, elapsed < 5000: the product fits uint32_t. */
                rpm = peak * (uint32_t)el_ms / LINKTEST_RISE_MS;
            } else if (el_ms < LINKTEST_RISE_MS + LINKTEST_HOLD_MS) {
                rpm = peak;
            } else {
                const int64_t f = el_ms - LINKTEST_RISE_MS - LINKTEST_HOLD_MS;
                rpm = peak - peak * (uint32_t)f / LINKTEST_FALL_MS;
            }
            sample.active = true;
            sample.rpm = rpm > UINT16_MAX ? UINT16_MAX : (uint16_t)rpm;
        }
    }
    portEXIT_CRITICAL(&s_linktest_mux);
    return sample;
}

static bool master_stopping(void)
{
    portENTER_CRITICAL(&s_linktest_mux);
    bool stop = !s_linktest_accepting;
    portEXIT_CRITICAL(&s_linktest_mux);
    return stop;
}

// A TEST event may shorten an idle wait to the 20 ms test cadence, but never
// publish early: even repeated requests cannot turn this into a radio flood.
static void master_wait(int64_t sent_us)
{
    while (!master_stopping()) {
        const int interval_ms = master_linktest_active() ? LINKTEST_INTERVAL_MS : BROADCAST_INTERVAL_MS;
        const int64_t remaining = sent_us + (int64_t)interval_ms * 1000 - esp_timer_get_time();
        if (remaining <= 0) return;
        const TickType_t ticks = (TickType_t)(((uint64_t)remaining * configTICK_RATE_HZ + 999999) / 1000000);
        xSemaphoreTake(s_master_wake, ticks);  // ceil to ticks, then recheck latest state/deadline
    }
}

static void master_task(void *arg) {
    (void)arg;
    uint8_t frame[ESPNOW_FRAME_MAX];
    bool override_active = false;
    int64_t next_sources_us = 0;
    while (!master_stopping()) {
        const nvs_user_cfg_t cfg = nvs_cfg_read();
        const linktest_sample_t test = master_sample_linktest(cfg.rpm_warn_threshold);
        if (test.active || override_active) obd_data_rpm_override_set(test.active, test.rpm);
        override_active = test.active;
        master_collect(s_snap, &cfg, test);
        if (master_stopping()) break;
        bool v6 = master_slaves_all_v6();
        if (v6 != s_tx_v6) {
            s_tx_v6 = v6;
            ESP_LOGI(TAG, "Broadcast format -> v%d", v6 ? ESPNOW_V6_VERSION : ESPNOW_V5_VERSION);
        }
        if (v6) {
            int n = espnow_v6_frame_count(s_snap);
            for (int i = 0; i < n; i++) link_send(frame, espnow_v6_encode(s_snap, i, frame, sizeof(frame)));
        } else {
            link_send(frame, espnow_v5_encode(s_snap, frame, sizeof(frame)));
        }
        // Extensions do not participate in the v5/v6 DATA fallback: old peers
        // ignore type 3, new peers still receive TPMS in a mixed-version group.
        int64_t now = esp_timer_get_time();
        if (now >= next_sources_us && !master_stopping()) {
            espnow_demand_t demand = espnow_link_remote_data_demand();
            if (demand.tpms) {
                tpms_wheel_data_t wheels[TPMS_WHEEL_COUNT];
                tpms_ble_snapshot(wheels); // always local; never relay a slave cache
                link_send(frame, espnow_tpms_encode(s_snap->seq, demand.tpms, wheels, frame, sizeof(frame)));
            }
            next_sources_us = now + (int64_t)PRESENCE_INTERVAL_MS * 1000;
        }
        master_wait(esp_timer_get_time());
    }
    obd_data_rpm_override_set(false, 0);
    portENTER_CRITICAL(&s_linktest_mux);
    s_master_running = false;
    portEXIT_CRITICAL(&s_linktest_mux);
    vTaskDelete(NULL);
}

void espnow_link_start_master(void) {
    if (!s_master_wake) s_master_wake = xSemaphoreCreateBinaryStatic(&s_master_wake_storage);
    if (!s_master_wake) {
        ESP_LOGE(TAG, "master wake allocation failed");
        return;
    }
    cache_local_profile_hash();
    if (!s_snap) {
        s_snap = heap_caps_malloc(sizeof(*s_snap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_snap) s_snap = malloc(sizeof(*s_snap));
        if (!s_snap) {
            ESP_LOGE(TAG, "snapshot alloc failed, ESP-NOW master not started");
            return;
        }
    }
    wifi_espnow_init();
    s_is_master = true;
    s_tx_v6 = false;
    portENTER_CRITICAL(&s_linktest_mux);
    s_linktest_active = false;
    s_linktest_accepting = true;
    s_master_running = true;
    portEXIT_CRITICAL(&s_linktest_mux);
    add_broadcast_peer();

    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));   // receive slave presence/HELLO/control packets
    if (xTaskCreate(master_task, "espnow_tx", 3584, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_linktest_mux);
        s_linktest_accepting = false;
        s_linktest_active = false;
        s_master_running = false;
        portEXIT_CRITICAL(&s_linktest_mux);
        ESP_LOGE(TAG, "master task alloc failed");
        return;
    }
    ESP_LOGI(TAG, "ESP-NOW MASTER up (broadcast %dms, ch%d)", BROADCAST_INTERVAL_MS, ESPNOW_CHANNEL);
}

void espnow_link_set_data_demand(const espnow_demand_t *demand)
{
    if (!demand) return;
    portENTER_CRITICAL(&s_slaves_mux);
    s_local_demand = *demand;
    s_local_demand.tpms &= 0x0fu;
    portEXIT_CRITICAL(&s_slaves_mux);
}

void espnow_link_set_demand_mask(uint16_t mask)
{
    portENTER_CRITICAL(&s_slaves_mux);
    s_local_demand.obd = mask;
    portEXIT_CRITICAL(&s_slaves_mux);
}

espnow_demand_t espnow_link_remote_data_demand(void)
{
    espnow_demand_t demand = {0};
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_slaves_mux);
    if (s_is_master) {
        for (int i = 0; i < MG_MAX_SLAVES; i++) {
            if (s_slaves[i].last_demand_us != 0 &&
                now - s_slaves[i].last_demand_us < ESPNOW_TIMEOUT_US) {
                demand.obd |= s_slaves[i].demand.obd;
                demand.veh |= s_slaves[i].demand.veh;
                demand.tpms |= s_slaves[i].demand.tpms;
            }
        }
    }
    portEXIT_CRITICAL(&s_slaves_mux);
    return demand;
}

uint16_t espnow_link_remote_demand_mask(void)
{
    return espnow_link_remote_data_demand().obd;
}

void espnow_link_tpms_snapshot(tpms_wheel_data_t out[TPMS_WHEEL_COUNT])
{
    if (nvs_cfg_read().device_role != ESPNOW_ROLE_SLAVE) {
        tpms_ble_snapshot(out);
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_bound_master_mac_mux);
    espnow_tpms_cache_snapshot(&s_tpms_cache, now, out);
    portEXIT_CRITICAL(&s_bound_master_mac_mux);
}

// ========================= Slave =========================
static int ops_slot_by_id(void *ctx, uint16_t num_id) {
    (void)ctx;
    return signal_store_slot_by_id(num_id);
}

// Slots fed by this unit's own sensor (signal_store_set_local) are never
// overwritten or invalidated by the master's frames.
static void ops_set(void *ctx, int slot, int32_t value) {
    (void)ctx;
    if (signal_store_is_local(slot)) return;
    signal_store_set(slot, value);
}

static void ops_set_many(void *ctx, const int *slots, const int32_t *values, size_t count) {
    (void)ctx;
    signal_store_set_many(slots, values, count);
}

static void ops_invalidate(void *ctx, int slot) {
    (void)ctx;
    if (signal_store_is_local(slot)) return;
    signal_store_invalidate(slot);
}

static void ops_invalidate_many(void *ctx, const int *slots, size_t count) {
    (void)ctx;
    signal_store_invalidate_many(slots, count);
}

static const espnow_apply_ops_t s_apply_ops = {
    .ctx = NULL, .slot_by_id = ops_slot_by_id, .set = ops_set, .invalidate = ops_invalidate,
    .set_many = ops_set_many, .invalidate_many = ops_invalidate_many,
};

static void slave_set_link(uint8_t link) {
    if (s_rx_link == link) return;
    s_rx_link = link;
    signal_store_set(SIG_SYS_OBD_LINK, link);
}

static void slave_invalidate_veh(void) {
    int n = signal_store_veh_count();
    if (n <= 0) return;
    int slots[SIG_VEH_MAX];
    for (int i = 0; i < n; i++) slots[i] = SIG_VEH_BASE + i;
    signal_store_invalidate_many(slots, (size_t)n);
}

/* The WiFi callback, slave worker and UI run on different tasks. */
static void slave_last_rx_set(int64_t timestamp) {
    portENTER_CRITICAL(&s_last_rx_mux);
    s_last_rx_us = timestamp;
    portEXIT_CRITICAL(&s_last_rx_mux);
}

static int64_t slave_last_rx_get(void) {
    portENTER_CRITICAL(&s_last_rx_mux);
    int64_t timestamp = s_last_rx_us;
    portEXIT_CRITICAL(&s_last_rx_mux);
    return timestamp;
}

// Slave side: a validated DATA frame (v5 or one v6 fragment) from the master -> signal_store
static void slave_on_data(const espnow_rx_frame_t *f, int64_t received_us) {
    slave_last_rx_set(received_us);
    s_rx_live = true;
    // sys.obd_link first: obd.* / veh.* read as valid only while it is 2
    slave_set_link((f->flags & ESPNOW_FLAG_ELM) ? SIG_LINK_DATA : SIG_LINK_NO_DATA);

    // veh.* ids are assigned in profile order: only accept them from a master running our profile
    bool veh_ok = f->version == ESPNOW_V6_VERSION && f->profile_hash == local_profile_hash();
    if (s_rx_veh_ok && !veh_ok) slave_invalidate_veh();
    s_rx_veh_ok = veh_ok;
    espnow_rx_apply(f, veh_ok, &s_apply_ops);

    // master is running a linked test -> slaves take part in rendering during TEST, following its RPM ramp
    bool lt = (f->flags & ESPNOW_FLAG_LINKTEST) != 0;
    if (lt) {
        for (int i = 0; i < f->n_rec; i++) {
            uint16_t id;
            int32_t v;
            if (espnow_rx_record(f, i, &id, &v) && id == ESPNOW_ID_RPM) {
                obd_data_rpm_override_set(true, (uint16_t)(v < 0 ? 0 : (v > UINT16_MAX ? UINT16_MAX : v)));
                break;
            }
        }
    } else if (s_rx_linktest) {
        obd_data_rpm_override_set(false, 0);
    }
    s_rx_linktest  = lt;
    s_rx_linked_en = (f->flags & ESPNOW_FLAG_LINKED_EN) != 0;   // mirror so slaves render the gradient without manual NVS config

    if (f->frag_index == 0) {   // once per snapshot
        // These fields are snapshots, not commands. Avoid queue traffic when
        // they have not changed; retain the old value if the queue is full so
        // the next broadcast retries it.
        if (!s_rx_sweep_valid || s_rx_sweep_step != f->sweep_step) {
            if (app_event_send(APP_EVT_ESPNOW_SYNC_SLOT, f->sweep_step)) {
                s_rx_sweep_step = f->sweep_step;
                s_rx_sweep_valid = true;
            }
        }
        if (!s_rx_intro_valid || s_rx_intro_step != f->intro_step) {
            if (app_event_send(APP_EVT_ESPNOW_INTRO_STEP, f->intro_step)) {
                s_rx_intro_step = f->intro_step;
                s_rx_intro_valid = true;
            }
        }
        portENTER_CRITICAL(&s_last_rx_mux);
        memcpy(s_master_name, f->name, MASTER_NAME_LEN - 1);
        s_master_name[MASTER_NAME_LEN - 1] = '\0';
        portEXIT_CRITICAL(&s_last_rx_mux);   // remember the master name (shown on the info page)
    }
}

// Slave side: link lost -> sys.obd_link = 0 and drop every vehicle value
static void slave_drop_link(void) {
    s_rx_live = false;
    slave_set_link(SIG_LINK_NONE);
    signal_store_invalidate_vehicle();
    s_rx_veh_ok = false;
    if (s_rx_linktest) {
        s_rx_linktest = false;
        obd_data_rpm_override_set(false, 0);
    }
}

// Slave side: master silent for ESPNOW_TIMEOUT_US -> link lost
static void slave_check_timeout(int64_t now) {
    if (!s_rx_live || (now - slave_last_rx_get()) < ESPNOW_TIMEOUT_US) return;
    slave_drop_link();
    ESP_LOGI(TAG, "Master silent for %d ms: link lost", ESPNOW_TIMEOUT_US / 1000);
}

// Master side: received a slave presence (v5) or HELLO (v6) -> record/refresh the online slave
static void handle_presence(const uint8_t *mac, uint8_t position, bool hello_v6,
                            bool has_demand, const espnow_demand_t *demand) {
    if (!mac) return;
    portENTER_CRITICAL(&s_slaves_mux);
    int free_idx = -1, match = -1, oldest = 0;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us != 0 && memcmp(s_slaves[i].mac, mac, 6) == 0) { match = i; break; }
        if (s_slaves[i].last_us == 0 && free_idx < 0) free_idx = i;
        if (s_slaves[i].last_us < s_slaves[oldest].last_us) oldest = i;
    }
    int idx = (match >= 0) ? match : (free_idx >= 0 ? free_idx : oldest);
    int64_t now = esp_timer_get_time();
    if (match < 0) {
        s_slaves[idx].last_v6_us = 0;   // new / recycled entry
        s_slaves[idx].last_demand_us = 0;
        s_slaves[idx].demand = (espnow_demand_t){0};
    }
    memcpy(s_slaves[idx].mac, mac, 6);
    s_slaves[idx].position = position;
    s_slaves[idx].last_us = now;
    if (hello_v6) s_slaves[idx].last_v6_us = now;
    if (has_demand) {
        s_slaves[idx].demand = *demand;
        s_slaves[idx].last_demand_us = now;
    }
    portEXIT_CRITICAL(&s_slaves_mux);
}

// Linked control packet:
//   master receives TEST_START -> start the RPM ramp; receives THRESH_SET -> send event (UI task writes NVS + relays)
//   slave receives THRESH_SET  -> send event (UI task writes NVS, no relay); TEST is master-driven, slave ignores it
static void handle_ctrl(const espnow_ctrl_packet_t *c) {
    if (c->cmd == ESPNOW_CTRL_TEST_START) {
        if (s_is_master) {
            if (master_start_linktest()) ESP_LOGI(TAG, "Linked test started (ctrl from slave)");
        }
    } else if (c->cmd == ESPNOW_CTRL_THRESH_SET) {
        app_event_send(APP_EVT_ESPNOW_THRESH_SYNC, (uint32_t)c->arg);
    }
}

static void handle_rx(const uint8_t *mac, const uint8_t *data, int len, int64_t received_us) {
    if (!data || len <= 0) return;
    espnow_rx_frame_t f;
    uint8_t pos, caps;

    switch (espnow_classify(data, (size_t)len)) {
    case ESPNOW_PKT_V5_DATA:   // the master does not process DATA packets
        if (!s_is_master && espnow_v5_decode(data, (size_t)len, &f)) slave_on_data(&f, received_us);
        break;
    case ESPNOW_PKT_V6_DATA:
        if (!s_is_master && espnow_v6_decode(data, (size_t)len, &f)) slave_on_data(&f, received_us);
        break;
    case ESPNOW_PKT_V6_TPMS:
        if (!s_is_master && mac) {
            portENTER_CRITICAL(&s_bound_master_mac_mux);
            if (!s_bound_master_mac_active || !memcmp(mac, s_bound_master_mac, 6))
                espnow_tpms_cache_apply(&s_tpms_cache, mac, data, (size_t)len, received_us);
            portEXIT_CRITICAL(&s_bound_master_mac_mux);
        }
        break;
    case ESPNOW_PKT_V5_CTRL: {
        espnow_ctrl_packet_t c;
        memcpy(&c, data, sizeof(c));
        handle_ctrl(&c);
        break;
    }
    case ESPNOW_PKT_V5_PRESENCE:   // only the master tallies slaves
        if (s_is_master) handle_presence(mac, data[3], false, false, NULL);
        break;
    case ESPNOW_PKT_V6_HELLO: {
        espnow_demand_t demand = {0};
        if (s_is_master && espnow_sources_hello_decode(data, (size_t)len, &pos, &caps, &demand)) {
            handle_presence(mac, pos, (caps & ESPNOW_CAP_V6_DATA) != 0,
                            (caps & ESPNOW_CAP_DEMAND_MASK) != 0, &demand);
        }
        break;
    }
    default:
        break;
    }
}

static bool slave_stopping(void) {
    portENTER_CRITICAL(&s_last_rx_mux);
    bool stop = s_slave_stop;
    portEXIT_CRITICAL(&s_last_rx_mux);
    return stop;
}

static bool slave_source_allowed(const uint8_t *mac) {
    if (!mac) return false;
    portENTER_CRITICAL(&s_bound_master_mac_mux);
    bool allowed = !s_bound_master_mac_active ||
                   memcmp(mac, s_bound_master_mac, sizeof(s_bound_master_mac)) == 0;
    portEXIT_CRITICAL(&s_bound_master_mac_mux);
    return allowed;
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_is_master) {
        portENTER_CRITICAL(&s_last_rx_mux);
        bool accepting = s_slave_accepting;
        portEXIT_CRITICAL(&s_last_rx_mux);
        if (!accepting || !info || !slave_source_allowed(info->src_addr)) return;
        if (!data || len <= 0 || len > ESPNOW_FRAME_MAX) return;
        espnow_pkt_kind_t kind = espnow_classify(data, (size_t)len);
        if (kind == ESPNOW_PKT_V5_DATA || kind == ESPNOW_PKT_V6_DATA || kind == ESPNOW_PKT_V6_TPMS) {
            slave_packet_t packet = {.received_us = esp_timer_get_time(), .len = (uint16_t)len};
            memcpy(packet.mac, info->src_addr, sizeof(packet.mac));
            memcpy(packet.data, data, (size_t)len);
            if (xQueueSend(s_slave_queue, &packet, 0) != pdTRUE) {
                portENTER_CRITICAL(&s_last_rx_mux);
                s_slave_queue_dropped++;
                portEXIT_CRITICAL(&s_last_rx_mux);
            }
            xSemaphoreGive(s_slave_wake);
            return;
        }
    }
    handle_rx(info ? info->src_addr : NULL, data, len, esp_timer_get_time());
}

// Slave: periodically broadcast presence (v5, for old masters) + HELLO (v6 capability), and watch the link timeout
static void slave_task(void *arg) {
    (void)arg;
    espnow_presence_t pr = { .magic = ESPNOW_MAGIC, .version = ESPNOW_V5_VERSION };
    uint8_t hello[ESPNOW_SOURCES_HELLO_LEN];
    int64_t next_presence_us = 0;
    slave_packet_t packet;
    while (!slave_stopping()) {
        for (int i = 0; i < SLAVE_RX_QUEUE_LEN && !slave_stopping(); i++) {
            if (xQueueReceive(s_slave_queue, &packet, 0) != pdTRUE) break;
            /* A binding may change while a packet is queued. Do not revive
             * the previous master or apply a packet that already expired. */
            if (slave_source_allowed(packet.mac) &&
                esp_timer_get_time() - packet.received_us < ESPNOW_TIMEOUT_US) {
                handle_rx(packet.mac, packet.data, packet.len, packet.received_us);
            }
        }
        if (slave_stopping()) break;
        portENTER_CRITICAL(&s_last_rx_mux);
        uint32_t dropped = s_slave_queue_dropped;
        s_slave_queue_dropped = 0;
        portEXIT_CRITICAL(&s_last_rx_mux);
        if (dropped) ESP_LOGW(TAG, "slave receive queue full: dropped %lu frames", (unsigned long)dropped);
        int64_t now = esp_timer_get_time();
        if (now >= next_presence_us) {
            pr.position = nvs_device_position_get();
            link_send(&pr, sizeof(pr));
            espnow_demand_t demand;
            portENTER_CRITICAL(&s_slaves_mux);
            demand = s_local_demand;
            portEXIT_CRITICAL(&s_slaves_mux);
            link_send(hello, espnow_sources_hello_encode(pr.position, &demand, hello, sizeof(hello)));
            next_presence_us = now + (int64_t)PRESENCE_INTERVAL_MS * 1000;
        }
        slave_check_timeout(esp_timer_get_time());
        xSemaphoreTake(s_slave_wake, pdMS_TO_TICKS(SLAVE_TICK_MS));
    }
    slave_drop_link();
    portENTER_CRITICAL(&s_bound_master_mac_mux);
    memset(&s_tpms_cache, 0, sizeof(s_tpms_cache));
    portEXIT_CRITICAL(&s_bound_master_mac_mux);
    slave_last_rx_set(0);
    portENTER_CRITICAL(&s_last_rx_mux);
    s_slave_running = false;
    portEXIT_CRITICAL(&s_last_rx_mux);
    vTaskDelete(NULL);
}

void espnow_link_start_slave(void) {
    if (!s_slave_queue) s_slave_queue = xQueueCreateStatic(SLAVE_RX_QUEUE_LEN, sizeof(slave_packet_t),
                                                          s_slave_queue_bytes, &s_slave_queue_storage);
    if (!s_slave_wake) s_slave_wake = xSemaphoreCreateBinaryStatic(&s_slave_wake_storage);
    if (!s_slave_queue || !s_slave_wake) {
        ESP_LOGE(TAG, "slave receive queue allocation failed");
        return;
    }
    cache_local_profile_hash();
    cache_bound_master_mac();
    wifi_espnow_init();
    s_is_master = false;
    s_rx_live = false;
    slave_last_rx_set(0);
    s_rx_veh_ok = false;
    s_rx_sweep_valid = false;
    s_rx_intro_valid = false;
    s_rx_link = SIG_LINK_NONE;
    signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_NONE);   // no master heard yet
    // A slave also needs the broadcast peer to be able to send presence
    add_broadcast_peer();
    portENTER_CRITICAL(&s_last_rx_mux);
    s_slave_stop = false;
    s_slave_running = true;
    s_slave_accepting = true;
    portEXIT_CRITICAL(&s_last_rx_mux);
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    if (xTaskCreate(slave_task, "espnow_pres", 3584, NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_last_rx_mux);
        s_slave_running = false;
        s_slave_accepting = false;
        portEXIT_CRITICAL(&s_last_rx_mux);
        ESP_LOGE(TAG, "slave task allocation failed");
        return;
    }
    ESP_LOGI(TAG, "ESP-NOW SLAVE up (listening ch%d, presence %dms)", ESPNOW_CHANNEL, PRESENCE_INTERVAL_MS);
}

bool espnow_link_slave_has_data(void) {
    int64_t last = slave_last_rx_get();
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < ESPNOW_TIMEOUT_US; // data within 2s counts as online
}

void espnow_link_copy_master_name(char *out, size_t capacity) {
    if (!out || capacity == 0) return;
    portENTER_CRITICAL(&s_last_rx_mux);
    size_t n = capacity - 1 < sizeof(s_master_name) - 1 ? capacity - 1 : sizeof(s_master_name) - 1;
    memcpy(out, s_master_name, n);
    out[n] = '\0';
    portEXIT_CRITICAL(&s_last_rx_mux);
}

// Slave: MAC of the currently bound master (all-zero = unbound)
void espnow_link_get_bound_master_mac(uint8_t out[6]) {
    if (!out) return;
    nvs_user_cfg_t cfg = nvs_cfg_read();
    memcpy(out, cfg.espnow_master_mac, 6);
}

// Slave: bind to a specific master MAC (the ESP-NOW MAC read during BLE pairing)
void espnow_link_bind_master(const uint8_t mac[6]) {
    if (!mac) return;
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    memcpy(cfg.espnow_master_mac, mac, 6);
    const esp_err_t err = nvs_cfg_update(&base, &cfg);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_bound_master_mac_mux);
        memset(&s_tpms_cache, 0, sizeof(s_tpms_cache));
        memcpy(s_bound_master_mac, mac, sizeof(s_bound_master_mac));
        s_bound_master_mac_active = true;
        portEXIT_CRITICAL(&s_bound_master_mac_mux);
    }
    ESP_LOGI(TAG, "Bound to master MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             cfg.espnow_master_mac[0], cfg.espnow_master_mac[1], cfg.espnow_master_mac[2],
             cfg.espnow_master_mac[3], cfg.espnow_master_mac[4], cfg.espnow_master_mac[5]);
}

// Slave: unbind the master (return to accept-any mode)
void espnow_link_unbind_master(void) {
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    memset(cfg.espnow_master_mac, 0, 6);
    const esp_err_t err = nvs_cfg_update(&base, &cfg);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_bound_master_mac_mux);
        memset(&s_tpms_cache, 0, sizeof(s_tpms_cache));
        memset(s_bound_master_mac, 0, sizeof(s_bound_master_mac));
        s_bound_master_mac_active = false;
        portEXIT_CRITICAL(&s_bound_master_mac_mux);
    }
    ESP_LOGI(TAG, "Unbound master MAC (receive from any master)");
}

// Master: number of slaves currently online (reported within the last ESPNOW_TIMEOUT_US)
uint8_t espnow_master_online_slaves(void) {
    portENTER_CRITICAL(&s_slaves_mux);
    int64_t now = esp_timer_get_time();
    uint8_t n = 0;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us != 0 && (now - s_slaves[i].last_us) < ESPNOW_TIMEOUT_US) n++;
    }
    portEXIT_CRITICAL(&s_slaves_mux);
    return n;
}

// ========================= Multi-gauge linked sync =========================
// Trigger the linked test: the master starts the RPM ramp directly; a slave sends a request to the
// master, which drives it centrally (all gauges stay in sync).
void espnow_link_trigger_linked_test(void) {
    if (s_is_master) {
        if (master_start_linktest()) ESP_LOGI(TAG, "Linked test started (local)");
    } else {
        espnow_ctrl_packet_t c = { .magic = ESPNOW_MAGIC, .version = ESPNOW_V5_VERSION,
                                   .cmd = ESPNOW_CTRL_TEST_START, .arg = 0 };
        link_send(&c, sizeof(c));
    }
}

// The local user changed the threshold -> broadcast it to the other gauges. When the master
// receives such a packet from a slave it relays it, covering all slaves.
void espnow_link_broadcast_threshold(uint16_t thresh) {
    espnow_ctrl_packet_t c = { .magic = ESPNOW_MAGIC, .version = ESPNOW_V5_VERSION,
                               .cmd = ESPNOW_CTRL_THRESH_SET, .arg = thresh };
    link_send(&c, sizeof(c));
}

// Whether a linked test is in progress: master = local ramp state, slave = the last flag received
// from the master's broadcast. During TEST a gauge takes part in the gradient rendering even if it
// has LINKED FLASH turned off, guaranteeing "TEST on any gauge syncs all gauges".
bool espnow_link_linktest_active(void) {
    return s_is_master ? master_linktest_active() : s_rx_linktest;
}

// Whether rpm_warn_linked_en is active on the master: master = local NVS, slave = mirrored from broadcast flags bit2.
// Slaves use this so the gradient fires without needing manual NVS config on each slave unit.
bool espnow_link_linked_en_with_local(bool local_enabled) {
    return s_is_master ? local_enabled : s_rx_linked_en;
}

bool espnow_link_linked_en(void) {
    return espnow_link_linked_en_with_local(nvs_cfg_read().rpm_warn_linked_en != 0);
}

// Apply a threshold synced from another gauge (called by the UI task): write local NVS; the master
// additionally relays it to the other slaves. ESP-NOW does not loop back one's own sends, so the
// relay cannot form a loop.
void espnow_link_apply_synced_threshold(uint16_t thresh) {
    nvs_user_cfg_t base;
    nvs_cfg_snapshot(&base);
    nvs_user_cfg_t cfg = base;
    if (cfg.rpm_warn_threshold != thresh) {
        cfg.rpm_warn_threshold = thresh;
        nvs_cfg_update(&base, &cfg);
    }
    if (s_is_master) {
        espnow_link_broadcast_threshold(thresh);
    }
}

static bool stop_link_tasks(void)
{
    // Stop admission, wake both workers, and wait for their exit acknowledgments.
    portENTER_CRITICAL(&s_linktest_mux);
    s_linktest_accepting = false;
    s_linktest_active = false;
    bool master_running = s_master_running;
    portEXIT_CRITICAL(&s_linktest_mux);
    if (s_master_wake) xSemaphoreGive(s_master_wake);
    portENTER_CRITICAL(&s_last_rx_mux);
    s_slave_accepting = false;
    s_slave_stop = true;
    bool running = s_slave_running;
    portEXIT_CRITICAL(&s_last_rx_mux);
    if (s_slave_wake) xSemaphoreGive(s_slave_wake);
    if (running || master_running) return false;   // workers finish their current operations
    s_rx_linktest = false;
    obd_data_rpm_override_set(false, 0);
    return true;
}

// Quiesce ESP-NOW for OTA without stopping/deinitializing the WiFi driver.
// OTA can then reuse the already-running WiFi stack for its SoftAP, while the
// 10 Hz master broadcast (or slave presence traffic) no longer competes with
// the TCP upload on the ESP32-S3's single 2.4 GHz radio.
bool espnow_link_pause_for_ota(void)
{
    if (!stop_link_tasks()) return false;

    if (!s_espnow_started) {
        ESP_LOGI(TAG, "ESP-NOW already inactive for OTA");
        return true;
    }

    ESP_LOGI(TAG, "Pausing ESP-NOW for OTA (keeping WiFi initialized)");
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();

    esp_err_t err = esp_now_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_deinit during OTA pause failed: %s", esp_err_to_name(err));
        return false;
    }
    s_espnow_started = false;
    ESP_LOGI(TAG, "ESP-NOW paused; WiFi remains available for OTA SoftAP");
    return true;
}
