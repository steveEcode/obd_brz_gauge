#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_obd_dsp/ota_wifi_server.h"
#include "freertos/task.h"
static bool lifecycle, s_active, s_maintenance, sink_open, api_open, theme_busy, job;
static bool fail_lifecycle, fail_queue, fail_http, fail_job, fail_format;
static ota_wifi_state_t state;
static void *s_httpd;
static int s_sink;
static unsigned formats, stops, resets, releases, notifications;
static bool lifecycle_try_take(void)
{ if (lifecycle || fail_lifecycle) return false; lifecycle = true; return true; }
static void lifecycle_give(void) { assert(lifecycle); lifecycle = false; }
static ota_wifi_state_t get_state(void) { return state; }
static bool upload_sink_is_open(void *p) { assert(p == &s_sink); return sink_open; }
static bool api_v1_upload_busy(void) { return api_open; }
static bool theme_lib_busy(void) { return theme_busy; }
static TaskHandle_t xTaskGetCurrentTaskHandle_(void) { return (void *)1; }
#define xTaskGetCurrentTaskHandle xTaskGetCurrentTaskHandle_
static void xTaskNotifyGive(TaskHandle_t t) { assert(t == (void *)1); notifications++; }
static unsigned ulTaskNotifyTake(int clear, TickType_t wait)
{ assert(clear == pdTRUE && (wait == 0 || wait == portMAX_DELAY));
  unsigned n = notifications; notifications = 0; assert(wait == 0 || n); return n; }
static esp_err_t httpd_queue_work(void *h, void (*fn)(void *), void *arg)
{ assert(h && lifecycle && !job); if (fail_queue) return ESP_ERR_NO_MEM; fn(arg); return ESP_OK; }
static esp_err_t httpd_stop(void *h)
{ assert(h && lifecycle && s_maintenance && s_active && !job); stops++;
  return fail_http ? ESP_FAIL : ESP_OK; }
static bool storage_job_lock(TickType_t wait)
{ assert(wait == 0 && !s_httpd && !job && lifecycle && s_active && s_maintenance);
  job = !fail_job; return job; }
static void storage_job_unlock(void) { assert(job); job = false; }
static void api_v1_reset(void) { assert(job && !s_httpd); resets++; }
static void upload_release(void) { assert(job && !s_httpd); releases++; }
static esp_err_t sd_card_format(char *detail, size_t capacity)
{ assert(job && !s_httpd && lifecycle && s_active && s_maintenance); formats++;
  if (fail_format) { snprintf(detail, capacity, "create FAT filesystem: disk error"); return ESP_FAIL; }
  return ESP_OK; }
static const char *esp_err_to_name_(esp_err_t err) { (void)err; return "ESP_FAIL"; }
#define esp_err_to_name esp_err_to_name_
#include "sd_format.inc"
static void reset(void)
{
    lifecycle = s_maintenance = sink_open = api_open = theme_busy = job = false;
    fail_lifecycle = fail_queue = fail_http = fail_job = fail_format = false;
    s_active = true; s_httpd = (void *)2; state = OTA_WIFI_STATE_READY;
    formats = stops = resets = releases = notifications = 0;
}
int main(void)
{
    char detail[480]; bool reboot;
    for (unsigned i = 0; i < 8; i++) {
        reset(); reboot = true;
        switch (i) {
        case 0: fail_lifecycle = true; break;
        case 1: s_active = false; break;
        case 2: s_httpd = NULL; break;
        case 3: s_maintenance = true; break;
        case 4: sink_open = true; break;
        case 5: api_open = true; break;
        case 6: theme_busy = true; break;
        case 7: state = OTA_WIFI_STATE_RECEIVING; break;
        }
        assert(ota_wifi_server_format_sd(detail, sizeof(detail), &reboot) == ESP_ERR_INVALID_STATE);
        assert(!reboot && !formats && !stops && !lifecycle && !job);
    }
    reset(); fail_queue = true;
    assert(ota_wifi_server_format_sd(detail, sizeof(detail), &reboot) == ESP_ERR_NO_MEM);
    assert(!reboot && !lifecycle && !s_maintenance && !formats && !stops);
    reset(); assert(ota_wifi_server_format_sd(detail, sizeof(detail), NULL) == ESP_ERR_INVALID_STATE);
    assert(!lifecycle && !stops);
    for (unsigned i = 0; i < 4; i++) {
        reset(); fail_http = i == 1; fail_job = i == 2; fail_format = i == 3;
        if (i == 3) state = OTA_WIFI_STATE_ERROR; // Error screen can repair an unmountable filesystem.
        esp_err_t err = ota_wifi_server_format_sd(detail, sizeof(detail), &reboot);
        assert((err == ESP_OK) == (i == 0));
        assert(reboot && lifecycle && s_maintenance && s_active && !job && stops == 1);
        assert(formats == (unsigned)(i == 0 || i == 3));
        assert(resets == formats && releases == formats);
        if (i) assert(detail[0]);
        if (i == 1) assert(s_httpd); else assert(!s_httpd);
        bool again = true;
        assert(ota_wifi_server_format_sd(detail, sizeof(detail), &again) == ESP_ERR_INVALID_STATE && !again);
        assert(stops == 1); // No second destructive operation or takeover until reboot.
    }
    return 0;
}
