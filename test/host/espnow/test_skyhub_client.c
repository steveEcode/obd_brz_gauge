/* Real queue/worker, wire codec and signal store. Deterministic callback/tick
 * injections verify no flash or signal writes happen on the WiFi caller. */
#include "espnow_test_sdk.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "bsp_obd_dsp/skyhub_client.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "app_obd_dsp/signal_store.h"
#include <setjmp.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (step %u)\n",__FILE__,__LINE__,#x,step); exit(1); } } while (0)
static unsigned step, lock_depth, writes, sends;
static bool in_callback, save_fail, restore_case;
static int64_t now_us=1000000;
static uint8_t saved[6];
static const uint8_t self[6]={2,1,2,3,4,5}, hub[6]={4,6,7,8,9,10}, other[6]={6,1,2,3,4,5};
static uint32_t pending_nonce;
static TaskFunction_t worker;
static jmp_buf done;

void ble_test_enter(portMUX_TYPE *mux) { CHECK(!mux->locked); mux->locked=true; lock_depth++; }
void ble_test_exit(portMUX_TYPE *mux) { CHECK(mux->locked && lock_depth); mux->locked=false; lock_depth--; }
void link_test_log(const char *tag,const char *fmt,...) { (void)tag; (void)fmt; CHECK(!lock_depth); }
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) { (void)caps; return calloc(n,size); }
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_random(void) { return 0x12345678; }
esp_err_t esp_wifi_get_mac(int ifidx,uint8_t mac[6]) { CHECK(ifidx==WIFI_IF_STA); memcpy(mac,self,6); return ESP_OK; }
esp_err_t nvs_app_get_blob(const char *ns,const char *key,void *buf,size_t *len)
{ CHECK(!strcmp(ns,"hub") && !strcmp(key,"mac") && *len==6 && !in_callback); memcpy(buf,saved,6); return ESP_OK; }
esp_err_t nvs_app_set_blob(const char *ns,const char *key,const void *buf,size_t len)
{
    CHECK(!strcmp(ns,"hub") && !strcmp(key,"mac") && len==6 && !in_callback && !lock_depth);
    if (save_fail) return ESP_ERR_INVALID_STATE;
    memcpy(saved,buf,6); writes++; return ESP_OK;
}
esp_err_t nvs_app_erase_key(const char *ns,const char *key)
{
    CHECK(!strcmp(ns,"hub") && !strcmp(key,"mac") && !in_callback && !lock_depth);
    if (save_fail) return ESP_ERR_INVALID_STATE;
    memset(saved,0,6); writes++; return ESP_OK;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t capacity,UBaseType_t size,uint8_t *bytes,StaticQueue_t *storage)
{ *storage=(StaticQueue_t){.capacity=capacity,.size=size,.bytes=bytes}; return storage; }
BaseType_t xQueueSend(QueueHandle_t q,const void *item,TickType_t wait)
{
    CHECK(!lock_depth && !wait); if (q->count==q->capacity) return pdFALSE;
    memcpy(q->bytes+((q->head+q->count)%q->capacity)*q->size,item,q->size); q->count++; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q,void *item,TickType_t wait)
{
    CHECK(!lock_depth && !wait); if (!q->count) return pdFALSE;
    memcpy(item,q->bytes+q->head*q->size,q->size); q->head=(q->head+1)%q->capacity; q->count--; return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *handle)
{ (void)name;(void)stack;(void)arg;(void)priority;(void)handle; worker=fn; return pdPASS; }
void vTaskDelete(TaskHandle_t task) { CHECK(!task && !lock_depth); longjmp(done,1); }
esp_err_t esp_now_send(const uint8_t *mac,const uint8_t *data,size_t len)
{
    CHECK(!lock_depth && !in_callback && mac[0]==255);
    sh_frame_t f; CHECK(sh_decode(data,len,&f)); sends++;
    if (f.type==SH_PAIR) { CHECK(!memcmp(f.target,hub,6)); pending_nonce=f.nonce; }
    return ESP_OK;
}
static void inject(const uint8_t mac[6],sh_frame_t *f)
{
    uint8_t bytes[SH_MAX_FRAME]; size_t len=sh_encode(f,bytes,sizeof(bytes)); CHECK(len);
    unsigned before=writes; uint16_t owned=signal_store_hub_obd_mask();
    in_callback=true; CHECK(skyhub_client_receive(mac,bytes,len)); in_callback=false;
    CHECK(writes==before && signal_store_hub_obd_mask()==owned);
    memset(bytes,0xa5,sizeof(bytes)); // the worker must own a copy
}
static void data(const uint8_t mac[6],uint32_t seq,uint8_t mask,int32_t pressure)
{
    sh_frame_t f={.type=SH_DATA,.mask=mask,.session=777,.seq=seq,.values={90,110,pressure}};
    memcpy(f.target,self,6); inject(mac,&f);
}
void vTaskDelay(TickType_t ticks)
{
    CHECK(!lock_depth && ticks==50); skyhub_status_t status; skyhub_client_snapshot(&status);
    int32_t value;
    if (restore_case) {
        CHECK(status.online && status.active_mask==4 && !memcmp(status.bound_mac,hub,6));
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==225);
        CHECK(!skyhub_client_pause()); step++; now_us+=50000; return;
    }
    switch (step++) {
    case 0: {
        CHECK(status.scanning && !status.count && !writes);
        sh_frame_t f={.type=SH_ANNOUNCE,.mask=4,.session=777,.seq=1}; memset(f.target,255,6); inject(hub,&f); break;
    }
    case 1: CHECK(status.count==1 && !writes); CHECK(skyhub_client_bind(hub)); break;
    case 2: {
        CHECK(status.pairing && pending_nonce && !writes);
        sh_frame_t f={.type=SH_PAIR_ACK,.session=777,.seq=2,.nonce=pending_nonce+1}; memcpy(f.target,self,6); inject(hub,&f); break;
    }
    case 3: {
        CHECK(status.pairing && !writes && !sh_mac_valid(status.bound_mac));
        sh_frame_t f={.type=SH_PAIR_ACK,.session=777,.seq=3,.nonce=pending_nonce}; memcpy(f.target,self,6);
        inject(hub,&f); data(hub,10,4,250); break;
    }
    case 4: {
        CHECK(writes==1 && !memcmp(saved,hub,6) && status.online && status.active_mask==4);
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==250);
        CHECK(!signal_store_get(SIG_OBD_COOLANT_TEMP,&value));
        CHECK(!signal_store_set(SIG_OBD_OIL_PRESSURE,900)); signal_store_invalidate_vehicle();
        data(other,11,4,900); data(hub,10,4,800); data(hub,9,4,700);
        sh_frame_t f={.type=SH_DATA,.session=777,.seq=12,.mask=4,.values={0,0,600}}; memcpy(f.target,other,6); inject(hub,&f); break;
    }
    case 5:
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==250);
        data(hub,11,1,0); break;
    case 6:
        CHECK(status.active_mask==1 && !signal_store_get(SIG_OBD_OIL_PRESSURE,&value));
        CHECK(signal_store_get(SIG_OBD_COOLANT_TEMP,&value) && value==90);
        data(hub,12,4,300); break;
    case 7:
        CHECK(status.active_mask==4 && signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==300);
        data(hub,13,4,450); now_us+=2500000; break; // queue arrival must expire, not refresh on dequeue
    case 8:
        CHECK(!status.online && !status.active_mask && !signal_store_hub_obd_mask());
        CHECK(!signal_store_get(SIG_OBD_OIL_PRESSURE,&value));
        signal_store_set(SIG_SYS_OBD_LINK,SIG_LINK_DATA);
        CHECK(signal_store_set(SIG_OBD_OIL_PRESSURE,500));
        data(hub,14,4,350); break; // automatic reconnect, no discovery or re-pair
    case 9:
        CHECK(status.online && status.active_mask==4);
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==350);
        data(hub,15,4,-96); break;
    case 10:
        CHECK(status.online && status.active_mask==4 && !strcmp(status.status,"CONNECTED"));
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==-96);
        CHECK(signal_store_hub_obd_mask()==(1u<<SIG_OBD_OIL_PRESSURE));
        CHECK(!signal_store_set(SIG_OBD_OIL_PRESSURE,700));
        data(hub,16,4,-10); break; // legitimate -0.1 bar, not a missing-value sentinel
    case 11:
        CHECK(status.online && status.active_mask==4 && !strcmp(status.status,"CONNECTED"));
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&value) && value==-10);
        data(hub,17,0,0); break; // only the Hub's invalid flag releases this valid field
    case 12:
        CHECK(status.online && !status.active_mask && !signal_store_hub_obd_mask());
        CHECK(!strcmp(status.status,"CONNECTED; NO VALID SENSOR"));
        save_fail=true; CHECK(skyhub_client_unbind()); break;
    case 13:
        CHECK(sh_mac_valid(status.bound_mac) && writes==1);
        save_fail=false; CHECK(skyhub_client_unbind()); break;
    case 14:
        CHECK(!sh_mac_valid(status.bound_mac) && !sh_mac_valid(saved) && writes==2);
        data(hub,18,4,999); break;
    case 15:
        CHECK(!signal_store_hub_obd_mask());
        CHECK(!skyhub_client_pause()); CHECK(!skyhub_client_scan()); break;
    default: CHECK(false);
    }
    now_us+=50000;
}
static void run(bool restore)
{
    restore_case=restore; if (restore) memcpy(saved,hub,6);
    signal_store_init(); skyhub_client_start(); CHECK(worker);
    if (restore) data(hub,1,4,225); else CHECK(skyhub_client_scan());
    if (!setjmp(done)) worker(NULL);
    CHECK(skyhub_client_pause()); CHECK(!signal_store_hub_obd_mask());
    skyhub_status_t status; skyhub_client_snapshot(&status); CHECK(!status.available && !status.online);
    CHECK(step==(restore ? 1u : 16u));
}
int main(void)
{
    for (unsigned i=0;i<2;i++) {
        pid_t pid=fork(); CHECK(pid>=0);
        if (!pid) { run(i!=0); _exit(0); }
        int status; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("skyhub real client: pairing, NVS, restore, source filter, timeout, OBD arbitration, OTA stop: OK");
}
