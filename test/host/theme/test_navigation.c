/* Real LVGL timers/objects and production navigation state machine. Worker
 * scheduling + prepared-page engine are controlled shims; acquisition/discard
 * itself is covered with real engine sources in test_page. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "app_tasks.h"
#include "theme_engine/theme_interface.h"

lv_obj_t *ui_ScreenPageThemeGauge;
uint8_t ui_theme_gauge_page_index;
static TaskFunction_t queued_worker;
static void *queued_arg;
static bool worker_mode, fail_task, fail_begin, fail_finish;
static unsigned begins, runs, finishes, discards, attached, wakes, deleted, heartbeat;

struct theme_page_prepared { bool ran; };
static theme_page_prepared_t *live_job;
static lv_obj_t *last_published;

const char *theme_page_list_at(uint8_t index)
{
    return index == 0 ? "first" : index == 1 ? "second" : NULL;
}
theme_page_prepared_t *theme_page_prepare_begin(const char *id)
{
    assert(!worker_mode && !strcmp(id, "second") && !live_job);
    ++begins;
    if (fail_begin) return NULL;
    live_job = calloc(1, sizeof(*live_job));
    assert(live_job);
    return live_job;
}
void theme_page_prepare_run(theme_page_prepared_t *job)
{
    assert(worker_mode && job == live_job && !job->ran);
    job->ran = true;
    ++runs;
}
lv_obj_t *theme_page_prepare_finish(theme_page_prepared_t *job)
{
    assert(!worker_mode && job == live_job && job->ran);
    ++finishes;
    free(job); live_job = NULL;
    last_published = fail_finish ? NULL : lv_obj_create(NULL);
    return last_published;
}
void theme_page_prepare_discard(theme_page_prepared_t *job)
{
    assert(!worker_mode && job && job == live_job);
    ++discards;
    free(job); live_job = NULL;
}
TaskHandle_t app_task_start(app_task_id_t id, TaskFunction_t fn, void *arg)
{
    assert(!worker_mode && id == APP_TASK_THEME_PAGE_PREP && fn && arg && !queued_worker);
    if (fail_task) return NULL;
    queued_worker = fn; queued_arg = arg;
    return arg;
}
void vTaskDelete(TaskHandle_t task) { assert(worker_mode && !task); }
void lvgl_runtime_wake(void) { assert(worker_mode); ++wakes; }
void ui_ScreenPageThemeGauge_attach(lv_obj_t *screen)
{
    assert(!worker_mode && screen);
    ++attached;
    ui_ScreenPageThemeGauge = screen;
}
#include "../../../../main/export_path/ui_theme_navigation.c"

static void on_deleted(lv_event_t *event) { (void)event; ++deleted; }
static void beat(lv_timer_t *timer) { (void)timer; ++heartbeat; }
static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *data)
{ (void)area; (void)data; lv_disp_flush_ready(drv); }
static void advance(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 4) {
        lv_tick_inc(4);
        lv_timer_handler();
    }
}
static void run_worker(void)
{
    assert(queued_worker);
    TaskFunction_t fn = queued_worker;
    void *arg = queued_arg;
    queued_worker = NULL; queued_arg = NULL;
    worker_mode = true;
    fn(arg);
    worker_mode = false;
}
static lv_obj_t *new_origin(void)
{
    assert(!ui_theme_navigation_busy() && !live_job && !queued_worker);
    ui_theme_gauge_page_index = 0;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, on_deleted, LV_EVENT_DELETE, NULL);
    lv_scr_load(scr);
    ui_ScreenPageThemeGauge = scr;
    return scr;
}

static void test_normal_and_busy(void)
{
    lv_obj_t *old = new_origin();
    const unsigned r = runs, f = finishes, a = attached, d = deleted;
    assert(ui_theme_navigation_request(0) && !ui_theme_navigation_busy());
    assert(!ui_theme_navigation_request(2));
    assert(ui_theme_navigation_request(1) && ui_theme_navigation_busy());
    assert(!ui_theme_navigation_request(1)); /* Bounded: one worker and result. */
    const unsigned beats = heartbeat;
    advance(100);
    assert(heartbeat > beats); /* Existing LVGL timers still execute. */
    assert(runs == r && finishes == f && attached == a);
    assert(lv_scr_act() == old && ui_theme_gauge_page_index == 0);
    run_worker();
    assert(runs == r + 1 && finishes == f && attached == a);
    assert(lv_scr_act() == old); /* Worker must not publish or switch. */
    advance(100);
    assert(!ui_theme_navigation_busy() && !live_job);
    assert(finishes == f + 1 && attached == a + 1);
    assert(ui_theme_gauge_page_index == 1 && lv_scr_act() == last_published);
    assert(ui_ScreenPageThemeGauge == last_published && deleted == d + 1);
}

static void test_failures(void)
{
    lv_obj_t *old = new_origin();
    unsigned d = discards;
    fail_begin = true;
    assert(!ui_theme_navigation_request(1) && !ui_theme_navigation_busy());
    fail_begin = false;
    fail_task = true;
    assert(!ui_theme_navigation_request(1) && !ui_theme_navigation_busy());
    fail_task = false;
    assert(discards == d + 1 && !live_job && !queued_worker);
    advance(100);
    assert(lv_scr_act() == old && ui_theme_gauge_page_index == 0);
    fail_finish = true;
    assert(ui_theme_navigation_request(1));
    run_worker(); advance(100);
    fail_finish = false;
    assert(!ui_theme_navigation_busy() && !live_job);
    assert(lv_scr_act() == old && ui_ScreenPageThemeGauge == old && ui_theme_gauge_page_index == 0);
    /* Error paths removed the delete callback, including after task failure. */
    assert(!lv_obj_remove_event_cb(old, origin_changed));
}

static void test_cancellation(void)
{
    for (int scenario = 0; scenario < 5; ++scenario) {
        lv_obj_t *old = new_origin();
        const unsigned f = finishes, a = attached, d = discards;
        assert(ui_theme_navigation_request(1));
        lv_obj_t *other = NULL;
        if (scenario == 0) {
            ui_theme_navigation_cancel();
        } else if (scenario == 1 || scenario == 2) {
            other = lv_obj_create(NULL);
            lv_scr_load(other);
            if (scenario == 2) lv_obj_del(old); /* Origin pointer no longer valid. */
        } else if (scenario == 3) {
            ui_theme_gauge_page_index = 1;
        } else {
            /* SCREEN_UNLOAD_START itself cancels a leave-and-return. */
            other = lv_obj_create(NULL);
            lv_scr_load(other); lv_scr_load(old);
        }
        advance(100);
        assert(ui_theme_navigation_busy() && live_job); /* No early free. */
        run_worker(); advance(100);
        assert(!ui_theme_navigation_busy() && !live_job);
        assert(finishes == f && attached == a && discards == d + 1);
        if (scenario == 1 || scenario == 2) assert(lv_scr_act() == other);
        else assert(lv_scr_act() == old);
        if (scenario != 2) assert(!lv_obj_remove_event_cb(old, origin_changed));
    }
    ui_ScreenPageThemeGauge = NULL;
    assert(!ui_theme_navigation_request(1));
    ui_ScreenPageThemeGauge = lv_obj_create(NULL); /* Not the active screen. */
    assert(!ui_theme_navigation_request(1));
    ui_theme_navigation_cancel(); /* Idempotent when idle. */
}

int main(void)
{
    lv_init();
    static lv_color_t pixels[360 * 8];
    static lv_disp_draw_buf_t draw;
    static lv_disp_drv_t driver;
    lv_disp_draw_buf_init(&draw, pixels, NULL, 360 * 8);
    lv_disp_drv_init(&driver);
    driver.hor_res = driver.ver_res = 360;
    driver.draw_buf = &draw; driver.flush_cb = flush;
    lv_disp_t *disp = lv_disp_drv_register(&driver);
    lv_timer_t *beat_timer = lv_timer_create(beat, 16, NULL);
    test_normal_and_busy();
    test_failures();
    test_cancellation();
    assert(begins == 9 && runs == 7 && finishes == 2 && discards == 6 && wakes == 7);
    lv_timer_del(beat_timer);
    lv_disp_remove(disp);
    puts("production theme navigation + real LVGL: pending rendering, publication, failure and cancellation OK");
    return 0;
}
