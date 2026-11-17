#include "skyhub_client.h"
#include "nvs_app.h"
#include "app_obd_dsp/signal_store.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

#define RX_QUEUE_LEN 16
#define SCAN_US INT64_C(15000000)
#define PAIR_US INT64_C(8000000)
static const uint8_t broadcast[6]={255,255,255,255,255,255};
enum { RX_PACKET, CMD_SCAN, CMD_BIND, CMD_UNBIND };
typedef struct {
    uint8_t cmd, mac[6], len;
    int64_t received_us;
    uint8_t bytes[SH_MAX_FRAME];
} message_t;
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_bytes[RX_QUEUE_LEN*sizeof(message_t)];
static QueueHandle_t s_queue;
static portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
static skyhub_status_t s_status;
static bool s_running, s_stop;
static uint8_t s_self[6];
static uint32_t s_session, s_seq;

static void publish(const skyhub_status_t *status)
{
    portENTER_CRITICAL(&s_mux);
    s_status=*status;
    portEXIT_CRITICAL(&s_mux);
}
void skyhub_client_snapshot(skyhub_status_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_mux); *out=s_status; portEXIT_CRITICAL(&s_mux);
}
static bool stopping(void)
{
    portENTER_CRITICAL(&s_mux); bool stop=s_stop; portEXIT_CRITICAL(&s_mux); return stop;
}
static bool enqueue(message_t *m)
{
    portENTER_CRITICAL(&s_mux); bool accepting=s_running && !s_stop; portEXIT_CRITICAL(&s_mux);
    return accepting && s_queue && xQueueSend(s_queue,m,0)==pdTRUE;
}
bool skyhub_client_receive(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    /* Return true for our namespace even while stopped; never fall through to
     * legacy gauge control parsing. No decoding or flash work on WiFi task. */
    if (!data || len<2 || data[0]!='S' || data[1]!='H') return false;
    if (!mac || len>SH_MAX_FRAME) return true;
    message_t m={.cmd=RX_PACKET,.len=(uint8_t)len,.received_us=esp_timer_get_time()};
    memcpy(m.mac,mac,6); memcpy(m.bytes,data,len); enqueue(&m); return true;
}
bool skyhub_client_scan(void) { message_t m={.cmd=CMD_SCAN}; return enqueue(&m); }
bool skyhub_client_bind(const uint8_t mac[6])
{
    if (!sh_mac_valid(mac)) return false;
    message_t m={.cmd=CMD_BIND}; memcpy(m.mac,mac,6); return enqueue(&m);
}
bool skyhub_client_unbind(void) { message_t m={.cmd=CMD_UNBIND}; return enqueue(&m); }
static void send_command(uint8_t type, const uint8_t target[6], uint32_t nonce)
{
    sh_frame_t f={.type=type,.session=s_session,.seq=++s_seq,.nonce=nonce};
    memcpy(f.target,target,6);
    uint8_t bytes[SH_MAX_FRAME]; size_t len=sh_encode(&f,bytes,sizeof(bytes));
    if (len) esp_now_send(broadcast,bytes,len);
}
static void worker(void *arg)
{
    (void)arg;
    skyhub_status_t status; skyhub_client_snapshot(&status);
    uint8_t pending[6]={0};
    uint32_t nonce=0;
    int64_t scan_until=0, pair_until=0, next_scan=0, next_pair=0, last_data=0;
    sh_order_t order={0};
    message_t m;
    while (!stopping()) {
        int64_t now=esp_timer_get_time();
        for (int n=0;n<RX_QUEUE_LEN && !stopping();n++) {
            if (xQueueReceive(s_queue,&m,0)!=pdTRUE) break;
            if (m.cmd==CMD_SCAN) {
                status.count=0; scan_until=now+SCAN_US; next_scan=0;
                snprintf(status.status,sizeof(status.status),"SEARCHING SKY HUB...");
            } else if (m.cmd==CMD_BIND) {
                bool candidate=false;
                for (size_t i=0;i<status.count;i++) {
                    const skyhub_candidate_t *c=&status.candidates[i];
                    if (!memcmp(c->mac,m.mac,6) && (!sh_mac_valid(c->owner) || !memcmp(c->owner,s_self,6))) candidate=true;
                }
                if (!candidate) {
                    snprintf(status.status,sizeof(status.status),"HUB BUSY; HOLD ITS BOOT 3S");
                    continue;
                }
                memcpy(pending,m.mac,6); nonce=esp_random(); if (!nonce) nonce=1;
                pair_until=now+PAIR_US; next_pair=0;
                snprintf(status.status,sizeof(status.status),"PAIRING...");
            } else if (m.cmd==CMD_UNBIND) {
                if (nvs_app_erase_key("hub","mac")!=ESP_OK) {
                    snprintf(status.status,sizeof(status.status),"SAVE FAILED; RETRY"); continue;
                }
                if (sh_mac_valid(status.bound_mac)) {
                    for (int i=0;i<3;i++) send_command(SH_UNPAIR,status.bound_mac,esp_random());
                }
                memset(status.bound_mac,0,6); pair_until=0; last_data=0; order=(sh_order_t){0};
                signal_store_hub_apply(0,NULL); status.active_mask=0;
                snprintf(status.status,sizeof(status.status),"HUB FORGOTTEN");
            } else {
                sh_frame_t f;
                if (!sh_mac_valid(m.mac) || now-m.received_us>=SH_TIMEOUT_US || !sh_decode(m.bytes,m.len,&f)) continue;
                bool ours=!memcmp(f.target,s_self,6);
                if (f.type==SH_ANNOUNCE && now<scan_until && (ours || !memcmp(f.target,broadcast,6))) {
                    size_t i;
                    for (i=0;i<status.count;i++) if (!memcmp(status.candidates[i].mac,m.mac,6)) break;
                    if (i>=SKYHUB_SCAN_MAX) continue;
                    if (i==status.count) status.count++;
                    skyhub_candidate_t *c=&status.candidates[i];
                    memcpy(c->mac,m.mac,6); memcpy(c->owner,f.owner,6); c->mask=f.mask;
                    c->busy=sh_mac_valid(f.owner) && memcmp(f.owner,s_self,6)!=0;
                } else if (f.type==SH_PAIR_ACK && ours && now<pair_until &&
                           !memcmp(m.mac,pending,6) && f.nonce==nonce) {
                    if (nvs_app_set_blob("hub","mac",pending,6)!=ESP_OK) {
                        snprintf(status.status,sizeof(status.status),"SAVE FAILED; RETRY"); pair_until=0; continue;
                    }
                    if (sh_mac_valid(status.bound_mac) && memcmp(status.bound_mac,pending,6))
                        send_command(SH_UNPAIR,status.bound_mac,esp_random());
                    signal_store_hub_apply(0,NULL); status.active_mask=0;
                    memcpy(status.bound_mac,pending,6); pair_until=0; scan_until=0;
                    order=(sh_order_t){0}; last_data=0;
                    snprintf(status.status,sizeof(status.status),"PAIRED; WAITING FOR DATA");
                } else if (f.type==SH_DATA && ours && sh_mac_valid(status.bound_mac) &&
                           !memcmp(m.mac,status.bound_mac,6) && sh_order_accept(&order,&f)) {
                    signal_store_hub_apply(f.mask,f.values);
                    int32_t current[3];
                    status.active_mask=signal_store_hub_snapshot(current);
                    last_data=m.received_us;
                    snprintf(status.status,sizeof(status.status),status.active_mask ? "CONNECTED" : "CONNECTED; NO VALID SENSOR");
                }
            }
        }
        now=esp_timer_get_time();
        if (pair_until && now>=pair_until) {
            pair_until=0; snprintf(status.status,sizeof(status.status),"PAIR TIMEOUT; RETRY");
        }
        if (now<scan_until && now>=next_scan) { send_command(SH_DISCOVER,broadcast,0); next_scan=now+500000; }
        if (now<pair_until && now>=next_pair) { send_command(SH_PAIR,pending,nonce); next_pair=now+400000; }
        if (last_data && now-last_data>=SH_TIMEOUT_US) {
            last_data=0; status.active_mask=0; signal_store_hub_apply(0,NULL);
            snprintf(status.status,sizeof(status.status),"HUB OFFLINE; OBD FALLBACK");
        }
        if (status.scanning && now>=scan_until && !pair_until && !last_data)
            snprintf(status.status,sizeof(status.status),status.count ? "SELECT A HUB MAC" : "NO HUB FOUND; SCAN AGAIN");
        status.online=last_data!=0;
        status.scanning=now<scan_until; status.pairing=now<pair_until;
        publish(&status);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    signal_store_hub_apply(0,NULL);
    status.available=false; status.online=false; status.scanning=false; status.pairing=false; status.active_mask=0;
    publish(&status);
    portENTER_CRITICAL(&s_mux); s_running=false; portEXIT_CRITICAL(&s_mux);
    vTaskDelete(NULL);
}
void skyhub_client_start(void)
{
    if (!s_queue) s_queue=xQueueCreateStatic(RX_QUEUE_LEN,sizeof(message_t),s_queue_bytes,&s_queue_storage);
    if (!s_queue || esp_wifi_get_mac(WIFI_IF_STA,s_self)!=ESP_OK) return;
    skyhub_status_t status={.available=true};
    size_t len=6;
    if (nvs_app_get_blob("hub","mac",status.bound_mac,&len)!=ESP_OK || len!=6 || !sh_mac_valid(status.bound_mac))
        memset(status.bound_mac,0,6);
    snprintf(status.status,sizeof(status.status),sh_mac_valid(status.bound_mac) ? "SAVED; WAITING FOR HUB" : "NOT PAIRED");
    s_session=esp_random(); if (!s_session) s_session=1;
    publish(&status);
    portENTER_CRITICAL(&s_mux); s_stop=false; s_running=true; portEXIT_CRITICAL(&s_mux);
    if (xTaskCreate(worker,"skyhub_rx",4096,NULL,4,NULL)!=pdPASS) {
        portENTER_CRITICAL(&s_mux); s_running=false; s_status.available=false; portEXIT_CRITICAL(&s_mux);
        ESP_LOGE("skyhub", "worker creation failed");
    }
}
bool skyhub_client_pause(void)
{
    portENTER_CRITICAL(&s_mux); s_stop=true; bool stopped=!s_running; portEXIT_CRITICAL(&s_mux);
    return stopped;
}
