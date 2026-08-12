// Three-gauge ESP-NOW link: the master reads OBD then broadcasts to slaves.
// One master, many slaves, broadcast (1-to-many), coexists with BLE (master).
// Step 1: broadcast + no MAC filtering (enough for the single-master-per-car case).

#include "espnow_link.h"
#include "app_obd_dsp/app_event.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/nvs_storage.h"

// Sweep / boot-animation sync: read from the master (implemented in ui.c to avoid re-including lvgl here)
extern int  ui_sweep_get_step(void);
extern int  ui_intro_get_step(void);

#define TAG "espnow_link"

#define ESPNOW_CHANNEL          1       // master and slaves must share a channel (fixed here when STA is not connected to an AP)
#define ESPNOW_MAGIC            0x4F42  // 'OB' packet-header magic
#define ESPNOW_VER              5       // v5: added afr_x100 (air-fuel ratio)
#define MASTER_NAME_LEN         12
static const char MASTER_NAME[] = "SkyGauge";   // name the master broadcasts (shown on slaves); could become configurable later
#define BROADCAST_INTERVAL_MS   100     // master broadcast period (10Hz, plenty for gauges)
#define PRESENCE_INTERVAL_MS    500     // slave "presence" report period
#define MG_MAX_SLAVES           4       // max slaves the master tracks
#define MG_SLAVE_TIMEOUT_US     2000000 // a slave is considered online if it reported within 2s

static bool s_is_master = false;

// Slave -> master "presence" packet (small; distinguished from the OBD packet by length)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  position;   // slave position 1/2/3
} espnow_presence_t;

// Multi-gauge linked control packet (slave <-> master; length differs from both OBD and presence packets, dispatched by length)
#define ESPNOW_CTRL_TEST_START   1   // ask the master to start the linked test (arg unused)
#define ESPNOW_CTRL_THRESH_SET   2   // sync the RPM warning threshold (arg = threshold)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint16_t arg;
} espnow_ctrl_packet_t;

// Master side: linked-test RPM ramp state (the master is the single RPM injection source; slaves follow via broadcast)
static volatile bool s_linktest_active = false;
static int64_t s_linktest_start_us = 0;
static volatile bool s_rx_linktest = false;   // slave side: last received "linked test in progress" flag from the master
#define LINKTEST_RISE_MS 5000   // 0 -> peak, slow rise
#define LINKTEST_HOLD_MS 800    // hold at peak (flash)
#define LINKTEST_FALL_MS 2500   // peak -> 0, fall back

// Master side: online slave records
static struct { uint8_t mac[6]; uint8_t position; int64_t last_us; } s_slaves[MG_MAX_SLAVES];

// recv callback forward declaration (defined later in the file, needed by master init; signature depends on IDF version)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len);
#endif

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile int64_t s_last_rx_us = 0;
static uint32_t s_tx_seq = 0;

// Broadcast packet: mirrors the available fields of obd_data_cache (packed, identical on master and slaves).
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;             // bit0: master connected to ELM327; bit1: linked test in progress
    uint32_t seq;               // incrementing sequence (for packet-loss diagnosis)
    uint16_t rpm;
    uint8_t  speed;
    uint8_t  sweep_step;        // master's current sweep progress (0=none); slaves follow for a synced sweep
    uint8_t  intro_step;        // boot-animation progress (0=none, 1..4, 255=done); slaves follow
    int16_t  coolant_temp;
    int16_t  intake_temp;
    int16_t  oil_temp;
    int16_t  oil_pressure_x10;
    int16_t  boost_x10;
    int16_t  brake_temp_x10;
    int16_t  load_pct;
    int16_t  tps;
    int32_t  bat_mv;
    int16_t  afr_x100;          // air-fuel ratio AFR, x100 (1470=14.7:1), -1=invalid
    char     name[MASTER_NAME_LEN];  // master name (shown on the slave info page as "SLAVE: <name>")
} espnow_obd_packet_t;

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
}

// ========================= Master =========================
static void master_pack(espnow_obd_packet_t *p) {
    p->magic   = ESPNOW_MAGIC;
    p->version = ESPNOW_VER;
    // bit0 = ELM connected; bit1 = linked test in progress (slaves use it to force gradient rendering during TEST)
    p->flags   = (elm327_ble_is_connected() ? 0x01 : 0x00) | (s_linktest_active ? 0x02 : 0x00);
    p->seq     = ++s_tx_seq;
    p->rpm              = obd_data_get_rpm();
    p->speed            = obd_data_get_speed();
    p->sweep_step       = (uint8_t)ui_sweep_get_step();   // broadcast sweep progress for slave sync
    p->intro_step       = (uint8_t)ui_intro_get_step();   // broadcast boot-animation progress for slave sync
    p->coolant_temp     = obd_data_get_coolant_temp();
    p->intake_temp      = obd_data_get_intake_temp();
    p->oil_temp         = obd_data_get_oil_temp();
    p->oil_pressure_x10 = obd_data_get_oil_pressure_x10();
    p->boost_x10        = obd_data_get_boost_x10();
    p->brake_temp_x10   = obd_data_get_brake_temp_x10();
    p->load_pct         = obd_data_get_load_pct();
    p->tps              = obd_data_get_tps();
    p->bat_mv           = obd_data_get_bat_mv();
    p->afr_x100         = obd_data_get_afr_x100();
    strncpy(p->name, MASTER_NAME, MASTER_NAME_LEN);   // broadcast the master name
}

// Linked-test ramp control: compute the simulated RPM along the timeline and write it into the RPM override layer.
// The master's get_rpm returns the override -> master_pack broadcasts it to slaves and the master displays it; all gauges stay in sync.
static void master_linktest_task(void *arg) {
    for (;;) {
        if (s_linktest_active) {
            uint16_t thresh = nvs_cfg_get()->rpm_warn_threshold;
            uint32_t peak = (uint32_t)thresh + 200;   // slightly above threshold so the all-gauge flash is clearly visible
            int64_t el_ms = (esp_timer_get_time() - s_linktest_start_us) / 1000;
            int64_t total = LINKTEST_RISE_MS + LINKTEST_HOLD_MS + LINKTEST_FALL_MS;
            if (el_ms < 0 || el_ms >= total) {
                s_linktest_active = false;
                obd_data_rpm_override_set(false, 0);   // release the override, restore real RPM
            } else {
                uint32_t rpm;
                if (el_ms < LINKTEST_RISE_MS) {
                    rpm = (uint32_t)((uint64_t)peak * el_ms / LINKTEST_RISE_MS);
                } else if (el_ms < LINKTEST_RISE_MS + LINKTEST_HOLD_MS) {
                    rpm = peak;
                } else {
                    int64_t f = el_ms - LINKTEST_RISE_MS - LINKTEST_HOLD_MS;
                    rpm = peak - (uint32_t)((uint64_t)peak * f / LINKTEST_FALL_MS);
                }
                obd_data_rpm_override_set(true, (uint16_t)rpm);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));   // advance at 50Hz for a smooth gradient
    }
}

static void master_task(void *arg) {
    espnow_obd_packet_t pkt;
    uint32_t n = 0;
    for (;;) {
        master_pack(&pkt);
        esp_err_t r = esp_now_send(s_broadcast_mac, (const uint8_t *)&pkt, sizeof(pkt));
        if (r != ESP_OK) ESP_LOGW(TAG, "esp_now_send err=%d", r);
        // log a line every ~2s to confirm the broadcast is running (visible even on the bench without a car)
        if ((++n % (2000 / BROADCAST_INTERVAL_MS)) == 0) {
            ESP_LOGI(TAG, "TX seq=%u rpm=%u spd=%u clt=%d (obd=%s)",
                     (unsigned)pkt.seq, pkt.rpm, pkt.speed, pkt.coolant_temp,
                     (pkt.flags & 0x01) ? "conn" : "--");
        }
        // speed up broadcasting during the linked test (100ms -> 20ms) so the slaves' ramped RPM is smooth too
        vTaskDelay(pdMS_TO_TICKS(s_linktest_active ? 20 : BROADCAST_INTERVAL_MS));
    }
}

void espnow_link_start_master(void) {
    wifi_espnow_init();
    s_is_master = true;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));   // receive slave presence/control packets
    xTaskCreate(master_task, "espnow_tx", 3072, NULL, 4, NULL);
    xTaskCreate(master_linktest_task, "espnow_lt", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW MASTER up (broadcast %dms, ch%d)", BROADCAST_INTERVAL_MS, ESPNOW_CHANNEL);
}

// ========================= Slave =========================
static void apply_packet(const espnow_obd_packet_t *p) {
    s_rx_linktest = (p->flags & 0x02) != 0;   // master is running a linked test -> slaves take part in rendering during TEST
    obd_data_set_rpm(p->rpm);
    obd_data_set_speed(p->speed);
    obd_data_set_coolant_temp(p->coolant_temp);
    obd_data_set_intake_temp(p->intake_temp);
    obd_data_set_oil_temp(p->oil_temp);
    obd_data_set_oil_pressure_x10(p->oil_pressure_x10);
    obd_data_set_boost_x10(p->boost_x10);
    obd_data_set_brake_temp_x10(p->brake_temp_x10);
    obd_data_set_load_pct(p->load_pct);
    obd_data_set_tps(p->tps);
    obd_data_set_bat_mv(p->bat_mv);
    obd_data_set_afr_x100(p->afr_x100);
    // Notify the UI task via the event queue (avoids races from direct cross-task calls)
    app_event_send(APP_EVT_ESPNOW_SYNC_SLOT, p->sweep_step);
    app_event_send(APP_EVT_ESPNOW_INTRO_STEP, p->intro_step);
    memcpy(s_master_name, p->name, MASTER_NAME_LEN);
    s_master_name[MASTER_NAME_LEN - 1] = '\0';   // remember the master name (shown on the info page)
}

// Master side: received a slave presence -> record/refresh the online slave
static void handle_presence(const uint8_t *mac, const espnow_presence_t *pr) {
    if (!mac) return;
    int free_idx = -1, match = -1, oldest = 0;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us != 0 && memcmp(s_slaves[i].mac, mac, 6) == 0) { match = i; break; }
        if (s_slaves[i].last_us == 0 && free_idx < 0) free_idx = i;
        if (s_slaves[i].last_us < s_slaves[oldest].last_us) oldest = i;
    }
    int idx = (match >= 0) ? match : (free_idx >= 0 ? free_idx : oldest);
    memcpy(s_slaves[idx].mac, mac, 6);
    s_slaves[idx].position = pr->position;
    s_slaves[idx].last_us = esp_timer_get_time();
}

// Slave side: received the master's OBD packet -> write the cache
static void handle_obd(const espnow_obd_packet_t *p) {
    s_last_rx_us = esp_timer_get_time();
    apply_packet(p);
    static uint32_t rx_n = 0;
    if ((++rx_n % 20) == 0) {
        ESP_LOGI(TAG, "RX seq=%u rpm=%u spd=%u clt=%d", (unsigned)p->seq, p->rpm, p->speed, p->coolant_temp);
    }
}

// Linked control packet:
//   master receives TEST_START -> start the RPM ramp; receives THRESH_SET -> send event (UI task writes NVS + relays)
//   slave receives THRESH_SET  -> send event (UI task writes NVS, no relay); TEST is master-driven, slave ignores it
static void handle_ctrl(const espnow_ctrl_packet_t *c) {
    if (c->cmd == ESPNOW_CTRL_TEST_START) {
        if (s_is_master) {
            s_linktest_start_us = esp_timer_get_time();
            s_linktest_active = true;
            ESP_LOGI(TAG, "Linked test started (ctrl from slave)");
        }
    } else if (c->cmd == ESPNOW_CTRL_THRESH_SET) {
        app_event_send(APP_EVT_ESPNOW_THRESH_SYNC, (uint32_t)c->arg);
    }
}

static void handle_rx(const uint8_t *mac, const uint8_t *data, int len) {
    if (len == (int)sizeof(espnow_obd_packet_t)) {
        if (s_is_master) return;   // the master does not process OBD packets
        const espnow_obd_packet_t *p = (const espnow_obd_packet_t *)data;
        if (p->magic == ESPNOW_MAGIC && p->version == ESPNOW_VER) handle_obd(p);
    } else if (len == (int)sizeof(espnow_ctrl_packet_t)) {
        const espnow_ctrl_packet_t *c = (const espnow_ctrl_packet_t *)data;
        if (c->magic == ESPNOW_MAGIC && c->version == ESPNOW_VER) handle_ctrl(c);
    } else if (len == (int)sizeof(espnow_presence_t)) {
        if (!s_is_master) return;  // only the master tallies slaves
        const espnow_presence_t *pr = (const espnow_presence_t *)data;
        if (pr->magic == ESPNOW_MAGIC && pr->version == ESPNOW_VER) handle_presence(mac, pr);
    }
}

// The recv callback signature changed in IDF 5.0; support both versions.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    // Slave MAC filter: once bound to a master MAC, only accept packets from that MAC.
    if (!s_is_master && info) {
        const nvs_user_cfg_t *cfg = nvs_cfg_get();
        if (cfg->espnow_master_mac[0] != 0) {  // bound
            bool mac_match = true;
            for (int i = 0; i < 6; i++) {
                if (info->src_addr[i] != cfg->espnow_master_mac[i]) {
                    mac_match = false;
                    break;
                }
            }
            if (!mac_match) return;  // ignore packets from other masters
        }
    }
    handle_rx(info ? info->src_addr : NULL, data, len);
}
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len) {
    // Slave MAC filter: once bound to a master MAC, only accept packets from that MAC.
    if (!s_is_master && mac) {
        const nvs_user_cfg_t *cfg = nvs_cfg_get();
        if (cfg->espnow_master_mac[0] != 0) {  // bound
            bool mac_match = true;
            for (int i = 0; i < 6; i++) {
                if (mac[i] != cfg->espnow_master_mac[i]) {
                    mac_match = false;
                    break;
                }
            }
            if (!mac_match) return;  // ignore packets from other masters
        }
    }
    handle_rx(mac, data, len);
}
#endif

// Slave: periodically broadcast presence (including this gauge's position) for the master's handshake tally
static void slave_presence_task(void *arg) {
    espnow_presence_t pr = { .magic = ESPNOW_MAGIC, .version = ESPNOW_VER };
    for (;;) {
        pr.position = nvs_device_position_get();
        esp_now_send(s_broadcast_mac, (const uint8_t *)&pr, sizeof(pr));
        vTaskDelay(pdMS_TO_TICKS(PRESENCE_INTERVAL_MS));
    }
}

void espnow_link_start_slave(void) {
    wifi_espnow_init();
    s_is_master = false;
    // A slave also needs the broadcast peer to be able to send presence
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    xTaskCreate(slave_presence_task, "espnow_pres", 2560, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW SLAVE up (listening ch%d, presence %dms)", ESPNOW_CHANNEL, PRESENCE_INTERVAL_MS);
}

bool espnow_link_slave_has_data(void) {
    if (s_last_rx_us == 0) return false;
    return (esp_timer_get_time() - s_last_rx_us) < 2000000; // data within 2s counts as online
}

const char *espnow_link_get_master_name(void) {
    return s_master_name;   // empty string = no master data received yet
}

// Slave: MAC of the currently bound master (all-zero = unbound)
const uint8_t *espnow_link_get_bound_master_mac(void) {
    return nvs_cfg_get()->espnow_master_mac;
}

// Slave: bind to a specific master MAC (the ESP-NOW MAC read during BLE pairing)
void espnow_link_bind_master(const uint8_t mac[6]) {
    if (!mac) return;
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    memcpy(cfg.espnow_master_mac, mac, 6);
    nvs_cfg_set(&cfg);
    ESP_LOGI(TAG, "Bound to master MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             cfg.espnow_master_mac[0], cfg.espnow_master_mac[1], cfg.espnow_master_mac[2],
             cfg.espnow_master_mac[3], cfg.espnow_master_mac[4], cfg.espnow_master_mac[5]);
}

// Slave: unbind the master (return to accept-any mode)
void espnow_link_unbind_master(void) {
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    memset(cfg.espnow_master_mac, 0, 6);
    nvs_cfg_set(&cfg);
    ESP_LOGI(TAG, "Unbound master MAC (receive from any master)");
}

// Master: number of slaves currently online (reported within the last MG_SLAVE_TIMEOUT_US)
uint8_t espnow_master_online_slaves(void) {
    int64_t now = esp_timer_get_time();
    uint8_t n = 0;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us != 0 && (now - s_slaves[i].last_us) < MG_SLAVE_TIMEOUT_US) n++;
    }
    return n;
}

// ========================= Multi-gauge linked sync =========================
// Trigger the linked test: the master starts the RPM ramp directly; a slave sends a request to the
// master, which drives it centrally (all gauges stay in sync).
void espnow_link_trigger_linked_test(void) {
    if (s_is_master) {
        s_linktest_start_us = esp_timer_get_time();
        s_linktest_active = true;
        ESP_LOGI(TAG, "Linked test started (local)");
    } else {
        espnow_ctrl_packet_t c = { .magic = ESPNOW_MAGIC, .version = ESPNOW_VER,
                                   .cmd = ESPNOW_CTRL_TEST_START, .arg = 0 };
        esp_now_send(s_broadcast_mac, (const uint8_t *)&c, sizeof(c));
    }
}

// The local user changed the threshold -> broadcast it to the other gauges. When the master
// receives such a packet from a slave it relays it, covering all slaves.
void espnow_link_broadcast_threshold(uint16_t thresh) {
    espnow_ctrl_packet_t c = { .magic = ESPNOW_MAGIC, .version = ESPNOW_VER,
                               .cmd = ESPNOW_CTRL_THRESH_SET, .arg = thresh };
    esp_now_send(s_broadcast_mac, (const uint8_t *)&c, sizeof(c));
}

// Whether a linked test is in progress: master = local ramp state, slave = the last flag received
// from the master's broadcast. During TEST a gauge takes part in the gradient rendering even if it
// has LINKED FLASH turned off, guaranteeing "TEST on any gauge syncs all gauges".
bool espnow_link_linktest_active(void) {
    return s_is_master ? s_linktest_active : s_rx_linktest;
}

// Apply a threshold synced from another gauge (called by the UI task): write local NVS; the master
// additionally relays it to the other slaves. ESP-NOW does not loop back one's own sends, so the
// relay cannot form a loop.
void espnow_link_apply_synced_threshold(uint16_t thresh) {
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    if (cfg.rpm_warn_threshold != thresh) {
        cfg.rpm_warn_threshold = thresh;
        nvs_cfg_set(&cfg);
    }
    if (s_is_master) {
        espnow_link_broadcast_threshold(thresh);
    }
}
