#include "ui_theme_navigation.h"

#include <stdatomic.h>
#include <stddef.h>

#include "app_tasks.h"
#include "app_obd_dsp/lvgl_runtime.h"
#include "app_obd_dsp/perf_stats.h"
#include "theme_engine/theme_interface.h"

/* Deliberately keep this extension independent of SquareLine's full ui.h. */
extern lv_obj_t *ui_ScreenPageThemeGauge;
extern uint8_t ui_theme_gauge_page_index;
void ui_ScreenPageThemeGauge_attach(lv_obj_t *screen);

static theme_page_prepared_t *s_job;
static lv_obj_t *s_origin;
static uint8_t s_origin_index;
static uint8_t s_target_index;
static bool s_cancelled;
static atomic_bool s_ready;

static void origin_changed(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) s_origin = NULL;
    if (code == LV_EVENT_DELETE || code == LV_EVENT_SCREEN_UNLOAD_START)
        s_cancelled = true;
}

static void prepare_worker(void *arg)
{
    PERF_BEGIN(images_start);
    theme_page_prepare_run(arg);
    PERF_END(PERF_THEME_NAV_IMAGES, images_start, 1);
    atomic_store_explicit(&s_ready, true, memory_order_release);
    lvgl_runtime_wake();
    vTaskDelete(NULL);
}

static void prepare_poll(lv_timer_t *timer)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) return;
    const bool show = !s_cancelled && s_origin && lv_scr_act() == s_origin &&
                      ui_ScreenPageThemeGauge == s_origin &&
                      ui_theme_gauge_page_index == s_origin_index;
    lv_obj_t *old = s_origin;
    theme_page_prepared_t *job = s_job;
    const uint8_t target = s_target_index;
    if (s_origin) lv_obj_remove_event_cb(s_origin, origin_changed);
    s_origin = NULL;
    s_job = NULL;
    lv_timer_del(timer);
    if (!show) {
        theme_page_prepare_discard(job);
        return;
    }
    /* Only this LVGL callback publishes widgets. Failed/stale jobs leave
     * the old screen, page index, video, and input demand untouched. */
    PERF_BEGIN(publish_start);
    lv_obj_t *screen = theme_page_prepare_finish(job);
    PERF_END(PERF_THEME_NAV_PUBLISH, publish_start, 1);
    if (!screen) return;
    ui_theme_gauge_page_index = target;
    ui_ScreenPageThemeGauge_attach(screen);
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, old == lv_scr_act());
}

bool ui_theme_navigation_request(uint8_t index)
{
    if (s_job || !ui_ScreenPageThemeGauge || lv_scr_act() != ui_ScreenPageThemeGauge)
        return false;
    if (index == ui_theme_gauge_page_index) return true;
    const char *id = theme_page_list_at(index);
    if (!id) return false;
    PERF_BEGIN(begin_start);
    theme_page_prepared_t *job = theme_page_prepare_begin(id);
    PERF_END(PERF_THEME_NAV_BEGIN, begin_start, 1);
    if (!job) return false;
    lv_timer_t *timer = lv_timer_create(prepare_poll, 16, NULL);
    if (!timer) {
        theme_page_prepare_discard(job);
        return false;
    }
    s_job = job;
    s_origin = ui_ScreenPageThemeGauge;
    s_origin_index = ui_theme_gauge_page_index;
    s_target_index = index;
    s_cancelled = false;
    atomic_store_explicit(&s_ready, false, memory_order_relaxed);
    lv_obj_add_event_cb(s_origin, origin_changed, LV_EVENT_ALL, NULL);
    if (!app_task_start(APP_TASK_THEME_PAGE_PREP, prepare_worker, job)) {
        lv_obj_remove_event_cb(s_origin, origin_changed);
        s_origin = NULL;
        s_job = NULL;
        lv_timer_del(timer);
        theme_page_prepare_discard(job);
        return false;
    }
    return true;
}

bool ui_theme_navigation_busy(void)
{
    return s_job != NULL;
}

void ui_theme_navigation_cancel(void)
{
    if (s_job) s_cancelled = true;
}
