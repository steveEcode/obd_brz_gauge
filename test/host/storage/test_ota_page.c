#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_obd_dsp/ota_wifi_server.h"
typedef struct { void (*callback)(void *); } lv_timer_t;
typedef struct { int unused; } lv_obj_t;
#define LV_PART_MAIN 0
#define LV_STATE_DEFAULT 0
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define pdMS_TO_TICKS(ms) (ms)
static const char *TAG = "test";
static bool s_format_busy;
static lv_obj_t *s_format_dialog;
static lv_timer_t timer_storage, *s_start_timer;
static lv_obj_t label, theme, *s_label_wifi_ssid = &label, *ui_ScreenPageThemeGauge = &theme;
static bool link_stopped, stop_ready, timer_fail, start_ok = true;
static unsigned timers, timer_deletes, object_deletes, starts, reboots, delays;
static char label_text[80];
static bool espnow_link_pause_for_ota(void) { return link_stopped; }
bool ota_wifi_server_start(ota_wifi_info_t *info) {
    assert(link_stopped && !ui_ScreenPageThemeGauge && !s_start_timer);
    starts++; snprintf(info->ssid, sizeof(info->ssid), "test-ssid"); return start_ok;
}
bool ota_wifi_server_stop(void) { return stop_ready; }
static lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), unsigned ms, void *arg)
{ assert(ms == 100 && !arg && !s_start_timer); (void)cb; if (timer_fail) return NULL; timers++; return &timer_storage; }
static void lv_timer_del(lv_timer_t *timer) { assert(timer == &timer_storage); timer_deletes++; }
static lv_obj_t *lv_scr_act(void) { return &label; }
static void lv_obj_del(lv_obj_t *obj) { assert(obj == &theme); object_deletes++; }
static unsigned lv_color_hex(unsigned color) { return color; }
static void lv_obj_set_style_text_color(lv_obj_t *obj, unsigned color, int part)
{ assert(obj == &label); (void)color; (void)part; }
static void lv_label_set_text(lv_obj_t *obj, const char *text)
{ assert(obj == &label); snprintf(label_text, sizeof(label_text), "%s", text); }
static void vTaskDelay(unsigned ms) { assert(ms == 150); delays++; }
static void esp_restart(void) { reboots++; }
#include "ota_page.inc"
int main(void)
{
    timer_fail = true; start_ota_when_ready(NULL);
    assert(!s_start_timer && !starts && !strcmp(label_text, "WiFi: Failed to start"));
    timer_fail = false;
    start_ota_when_ready(NULL);
    assert(s_start_timer && timers == 1 && !starts && !object_deletes);
    start_ota_when_ready(s_start_timer);
    assert(timers == 1 && !starts && !object_deletes);
    exit_ota_mode(); assert(s_start_timer && !reboots && !timer_deletes);
    link_stopped = true; start_ota_when_ready(s_start_timer);
    assert(!s_start_timer && timer_deletes == 1 && object_deletes == 1 && starts == 1);
    assert(!strcmp(label_text, "WiFi: test-ssid"));
    stop_ready = true;
    s_format_busy = true; exit_ota_mode(); assert(!reboots);
    s_format_busy = false; s_format_dialog = &label; exit_ota_mode(); assert(!reboots);
    s_format_dialog = NULL; exit_ota_mode(); assert(reboots == 1 && delays == 1);
    /* Leaving while startup is deferred cancels the retry before reboot. */
    link_stopped = false; start_ota_when_ready(NULL); assert(s_start_timer);
    exit_ota_mode(); assert(!s_start_timer && timer_deletes == 2 && reboots == 2);
    return 0;
}
