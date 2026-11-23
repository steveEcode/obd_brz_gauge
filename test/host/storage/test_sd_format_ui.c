#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_obd_dsp/ota_wifi_server.h"
#include "freertos/FreeRTOS.h"
typedef struct { void *data; } lv_event_t;
typedef struct obj {
    struct obj *parent;
    void (*click)(lv_event_t *);
    void *data;
    bool alive, hidden, disabled;
    char text[512];
} lv_obj_t;
typedef struct timer { bool alive; void (*fn)(struct timer *); } lv_timer_t;
static lv_obj_t objects[100], *s_format_button, *s_format_status, *s_format_dialog, *s_format_close;
static lv_timer_t timer_storage, *s_format_timer;
static unsigned object_count, tasks, timers, formats, restarts;
static bool s_format_busy, s_format_finished, s_format_reboot, fail_task, fail_timer;
static bool worker_context, service_reboot;
static esp_err_t s_format_result, service_result;
static portMUX_TYPE s_format_mux;
static char s_format_detail[480];
static int ui_font_FontTypoderSize16;
static void (*pending_task)(void *);
static const char *TAG = "test";
#define ESP_LOGI(tag, ...) ((void)(tag))
#define LV_PART_MAIN 0
#define LV_OBJ_FLAG_SCROLLABLE 1
#define LV_OBJ_FLAG_GESTURE_BUBBLE 2
#define LV_OBJ_FLAG_HIDDEN 4
#define LV_STATE_DISABLED 1
#define LV_LABEL_LONG_WRAP 0
#define LV_TEXT_ALIGN_CENTER 0
#define LV_ALIGN_CENTER 0
#define LV_ALIGN_BOTTOM_MID 1
#define LV_ALIGN_TOP_MID 2
#define LV_ALIGN_BOTTOM_LEFT 3
#define LV_ALIGN_BOTTOM_RIGHT 4
#define LV_EVENT_CLICKED 0
#define LV_OPA_90 230
#define LV_PCT(n) (n)
#define lv_color_hex(n) (n)
#define lv_obj_set_size(o,w,h) ((void)(o))
#define lv_obj_set_width(o,w) ((void)(o))
#define lv_obj_center(o) ((void)(o))
#define lv_obj_align(o,a,x,y) ((void)(o))
#define lv_obj_set_style_text_font(o,f,p) ((void)(f))
#define lv_obj_set_style_text_align(o,a,p) ((void)(o))
#define lv_obj_set_style_bg_color(o,c,p) ((void)(o))
#define lv_obj_set_style_bg_opa(o,a,p) ((void)(o))
#define lv_label_set_long_mode(o,m) ((void)(o))
static lv_obj_t *lv_layer_top(void) { return NULL; }
static lv_obj_t *lv_obj_create(lv_obj_t *parent)
{ assert(!worker_context && object_count < 100); lv_obj_t *o = &objects[object_count++];
  *o = (lv_obj_t){.alive = true, .parent = parent}; return o; }
#define lv_btn_create lv_obj_create
#define lv_label_create lv_obj_create
static void lv_obj_clean(lv_obj_t *o)
{ assert(!worker_context && o && o->alive); for (unsigned i = 0; i < object_count; i++)
  if (objects[i].alive && objects[i].parent == o) { lv_obj_clean(&objects[i]); objects[i].alive = false; } }
static void lv_obj_del(lv_obj_t *o) { lv_obj_clean(o); o->alive = false; }
static void lv_label_set_text(lv_obj_t *o, const char *text)
{ assert(!worker_context && o && o->alive); snprintf(o->text, sizeof(o->text), "%s", text); }
static void lv_obj_clear_flag(lv_obj_t *o, int flag)
{ assert(o && o->alive); if (flag & LV_OBJ_FLAG_HIDDEN) o->hidden = false; }
static void lv_obj_add_flag(lv_obj_t *o, int flag)
{ assert(o && o->alive); if (flag & LV_OBJ_FLAG_HIDDEN) o->hidden = true; }
static void lv_obj_add_state(lv_obj_t *o, int state) { assert(state == 1); o->disabled = true; }
static void lv_obj_clear_state(lv_obj_t *o, int state) { assert(state == 1); o->disabled = false; }
static void lv_obj_add_event_cb(lv_obj_t *o, void (*fn)(lv_event_t *), int event, void *data)
{ (void)event; o->click = fn; o->data = data; }
static void *lv_event_get_user_data(lv_event_t *e) { return e->data; }
static lv_timer_t *lv_timer_create(void (*fn)(lv_timer_t *), unsigned period, void *data)
{ assert(period == 100 && !data && !timer_storage.alive); timers++;
  if (fail_timer) return NULL; timer_storage = (lv_timer_t){true, fn}; return &timer_storage; }
static void lv_timer_del(lv_timer_t *t) { assert(!worker_context && t->alive); t->alive = false; }
static BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                              void *arg, unsigned prio, void *handle)
{ assert(!strcmp(name, "sd_format") && stack == 8192 && !arg && prio == 2 && !handle);
  tasks++; if (fail_task) return pdFAIL; pending_task = fn; return pdPASS; }
esp_err_t ota_wifi_server_format_sd(char *detail, size_t size, bool *reboot)
{ assert(worker_context); formats++; *reboot = service_reboot;
  snprintf(detail, size, "%s", service_result ? "Upload/storage busy" : ""); return service_result; }
static void vTaskDelay(TickType_t ticks) { assert(worker_context && ticks == pdMS_TO_TICKS(5000)); }
static void esp_restart(void) { assert(worker_context); restarts++; }
static void vTaskDelete(void *task) { assert(worker_context && !task); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "ESP_FAIL"; }
#include "sd_format_ui.inc"
static void reset(void)
{
    object_count = tasks = timers = formats = restarts = 0; memset(objects, 0, sizeof(objects));
    timer_storage = (lv_timer_t){0}; s_format_timer = NULL; pending_task = NULL;
    s_format_button = lv_obj_create(NULL); s_format_dialog = s_format_status = s_format_close = NULL;
    s_format_busy = s_format_finished = s_format_reboot = fail_task = fail_timer = worker_context = false;
    service_result = ESP_OK; service_reboot = true; memset(s_format_detail, 0, sizeof(s_format_detail));
}
static void click(const char *text)
{
    for (unsigned i = 0; i < object_count; i++) if (objects[i].alive && !strcmp(objects[i].text, text)) {
        lv_obj_t *button = objects[i].parent; assert(button && button->click && !button->hidden);
        lv_event_t event = {.data = button->data}; button->click(&event); return;
    }
    assert(!"button not found");
}
int main(void)
{
    reset(); show_format_sd(NULL);
    assert(s_format_dialog && !tasks && !formats);
    show_format_sd(NULL); assert(!tasks); click("Cancel");
    assert(!s_format_dialog && !tasks && !formats);
    reset(); show_format_sd(NULL); fail_timer = true; click("ERASE");
    assert(!s_format_busy && !tasks && !s_format_close->hidden); click("OK");
    assert(!s_format_dialog && !formats);
    reset(); show_format_sd(NULL); fail_task = true; click("ERASE");
    assert(!s_format_busy && !s_format_button->disabled && !timer_storage.alive);
    click("OK"); assert(!s_format_dialog && !formats);
    for (unsigned i = 0; i < 3; i++) {
        reset(); show_format_sd(NULL); click("ERASE");
        assert(tasks == 1 && !formats && s_format_busy && s_format_button->disabled);
        assert(s_format_close->hidden && s_format_dialog && s_format_timer);
        show_format_sd(NULL); assert(tasks == 1);
        if (i) service_result = ESP_FAIL;
        service_reboot = i != 1;
        worker_context = true; pending_task(NULL); worker_context = false;
        assert(formats == 1 && s_format_finished && restarts == (unsigned)service_reboot);
        format_sd_progress(s_format_timer);
        assert(!s_format_timer && !timer_storage.alive);
        assert(s_format_busy == service_reboot && s_format_close->hidden == service_reboot);
        if (!service_reboot) { click("OK"); assert(!s_format_dialog && !s_format_button->disabled); }
    }
    return 0;
}
