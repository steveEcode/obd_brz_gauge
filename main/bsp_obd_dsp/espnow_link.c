// 三连表 ESP-NOW 互联: 主表读 OBD 后广播, 从表接收并显示。
// 一主多从, 广播(1对多), 与 BLE 共存(主表)。步骤1: 广播+无MAC过滤(同车单主表场景足够)。

#include "espnow_link.h"
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

// 扫表/开机动画同步: 主表读 / 从表写 (实现在 ui.c, 避免在此重引 lvgl 头)
extern int  ui_sweep_get_step(void);
extern void ui_sweep_set_step(int step);
extern int  ui_intro_get_step(void);
extern void ui_intro_set_step(int step);

#define TAG "espnow_link"

#define ESPNOW_CHANNEL          1       // 主从必须同信道(STA 不连AP时固定在此)
#define ESPNOW_MAGIC            0x4F42  // 'OB' 包头校验
#define ESPNOW_VER              4       // v4: 追加 intro_step(开机动画同步)
#define MASTER_NAME_LEN         12
static const char MASTER_NAME[] = "SkyGauge";   // 主表广播的名字(从表显示用); 后续可做成可配置
#define BROADCAST_INTERVAL_MS   100     // 主表广播周期(10Hz, 仪表足够)
#define PRESENCE_INTERVAL_MS    500     // 从表上报"存在"周期
#define MG_MAX_SLAVES           4       // 主表最多跟踪的从表数
#define MG_SLAVE_TIMEOUT_US     2000000 // 从表 2s 内有上报视为在线

static bool s_is_master = false;

// 从表→主表 "存在"包(小; 与 OBD 数据包按长度区分)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  position;   // 从表位置 1/2/3
} espnow_presence_t;

// 主表侧: 在线从表记录
static struct { uint8_t mac[6]; uint8_t position; int64_t last_us; } s_slaves[MG_MAX_SLAVES];

// recv 回调前向声明(定义在文件后段, 主表初始化处要用; 签名随 IDF 版本)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len);
#endif

static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static volatile int64_t s_last_rx_us = 0;
static uint32_t s_tx_seq = 0;

// 广播包: 镜像 obd_data_cache 的可用字段(packed, 主从一致)。
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;             // bit0: 主表已连上 ELM327
    uint32_t seq;               // 递增序号(丢包诊断)
    uint16_t rpm;
    uint8_t  speed;
    uint8_t  sweep_step;        // 主表当前扫表进度(0=无), 从表跟随实现同步扫表
    uint8_t  intro_step;        // 开机动画进度(0=无,1..4,255=done), 从表跟随
    int16_t  coolant_temp;
    int16_t  intake_temp;
    int16_t  oil_temp;
    int16_t  oil_pressure_x10;
    int16_t  boost_x10;
    int16_t  brake_temp_x10;
    int16_t  load_pct;
    int16_t  tps;
    int32_t  bat_mv;
    char     name[MASTER_NAME_LEN];  // 主表名字(从表信息页显示 "SLAVE: <name>")
} espnow_obd_packet_t;

static char s_master_name[MASTER_NAME_LEN] = {0};  // 从表侧: 最近收到的主表名字

// ---- WiFi + ESP-NOW 底层初始化(主从共用) ----
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
    esp_wifi_set_ps(WIFI_PS_NONE);   // 关省电, 否则 ESP-NOW 接收会丢/延迟
    ESP_ERROR_CHECK(esp_now_init());
}

// ========================= 主表 =========================
static void master_pack(espnow_obd_packet_t *p) {
    p->magic   = ESPNOW_MAGIC;
    p->version = ESPNOW_VER;
    p->flags   = elm327_ble_is_connected() ? 0x01 : 0x00;
    p->seq     = ++s_tx_seq;
    p->rpm              = obd_data_get_rpm();
    p->speed            = obd_data_get_speed();
    p->sweep_step       = (uint8_t)ui_sweep_get_step();   // 广播扫表进度供从表同步
    p->intro_step       = (uint8_t)ui_intro_get_step();   // 广播开机动画进度供从表同步
    p->coolant_temp     = obd_data_get_coolant_temp();
    p->intake_temp      = obd_data_get_intake_temp();
    p->oil_temp         = obd_data_get_oil_temp();
    p->oil_pressure_x10 = obd_data_get_oil_pressure_x10();
    p->boost_x10        = obd_data_get_boost_x10();
    p->brake_temp_x10   = obd_data_get_brake_temp_x10();
    p->load_pct         = obd_data_get_load_pct();
    p->tps              = obd_data_get_tps();
    p->bat_mv           = obd_data_get_bat_mv();
    strncpy(p->name, MASTER_NAME, MASTER_NAME_LEN);   // 广播主表名字
}

static void master_task(void *arg) {
    espnow_obd_packet_t pkt;
    uint32_t n = 0;
    for (;;) {
        master_pack(&pkt);
        esp_err_t r = esp_now_send(s_broadcast_mac, (const uint8_t *)&pkt, sizeof(pkt));
        if (r != ESP_OK) ESP_LOGW(TAG, "esp_now_send err=%d", r);
        // 每 ~2s 打一行, 便于确认广播在跑(台架无车时也能看到)
        if ((++n % (2000 / BROADCAST_INTERVAL_MS)) == 0) {
            ESP_LOGI(TAG, "TX seq=%u rpm=%u spd=%u clt=%d (obd=%s)",
                     (unsigned)pkt.seq, pkt.rpm, pkt.speed, pkt.coolant_temp,
                     (pkt.flags & 0x01) ? "conn" : "--");
        }
        vTaskDelay(pdMS_TO_TICKS(BROADCAST_INTERVAL_MS));
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

    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));   // 接收从表 presence(握手统计)
    xTaskCreate(master_task, "espnow_tx", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "ESP-NOW MASTER up (broadcast %dms, ch%d)", BROADCAST_INTERVAL_MS, ESPNOW_CHANNEL);
}

// ========================= 从表 =========================
static void apply_packet(const espnow_obd_packet_t *p) {
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
    ui_sweep_set_step(p->sweep_step);   // 跟随主表扫表进度, 实现三连表同步扫表
    ui_intro_set_step(p->intro_step);   // 跟随主表开机动画进度
    memcpy(s_master_name, p->name, MASTER_NAME_LEN);
    s_master_name[MASTER_NAME_LEN - 1] = '\0';   // 记录主表名字(信息页显示)
}

// 主表侧: 收到从表 presence → 记录/更新在线从表
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

// 从表侧: 收到主表 OBD 包 → 写缓存
static void handle_obd(const espnow_obd_packet_t *p) {
    s_last_rx_us = esp_timer_get_time();
    apply_packet(p);
    static uint32_t rx_n = 0;
    if ((++rx_n % 20) == 0) {
        ESP_LOGI(TAG, "RX seq=%u rpm=%u spd=%u clt=%d", (unsigned)p->seq, p->rpm, p->speed, p->coolant_temp);
    }
}

static void handle_rx(const uint8_t *mac, const uint8_t *data, int len) {
    if (len == (int)sizeof(espnow_obd_packet_t)) {
        if (s_is_master) return;   // 主表不处理 OBD 包
        const espnow_obd_packet_t *p = (const espnow_obd_packet_t *)data;
        if (p->magic == ESPNOW_MAGIC && p->version == ESPNOW_VER) handle_obd(p);
    } else if (len == (int)sizeof(espnow_presence_t)) {
        if (!s_is_master) return;  // 只有主表统计从表
        const espnow_presence_t *pr = (const espnow_presence_t *)data;
        if (pr->magic == ESPNOW_MAGIC && pr->version == ESPNOW_VER) handle_presence(mac, pr);
    }
}

// recv 回调签名在 IDF 5.0 变更, 兼容两版
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    handle_rx(info ? info->src_addr : NULL, data, len);
}
#else
static void recv_cb(const uint8_t *mac, const uint8_t *data, int len) {
    handle_rx(mac, data, len);
}
#endif

// 从表: 周期广播 presence(含本机位置), 供主表握手统计
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
    // 从表也需广播 peer 才能发 presence
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
    return (esp_timer_get_time() - s_last_rx_us) < 2000000; // 2s 内有数据视为在线
}

const char *espnow_link_get_master_name(void) {
    return s_master_name;   // 空串=尚未收到主表数据
}

// 主表: 当前在线从表数(近 MG_SLAVE_TIMEOUT_US 内有上报)
uint8_t espnow_master_online_slaves(void) {
    int64_t now = esp_timer_get_time();
    uint8_t n = 0;
    for (int i = 0; i < MG_MAX_SLAVES; i++) {
        if (s_slaves[i].last_us != 0 && (now - s_slaves[i].last_us) < MG_SLAVE_TIMEOUT_US) n++;
    }
    return n;
}
