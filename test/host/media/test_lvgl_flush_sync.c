#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "freertos/semphr.h"
#include "app_obd_dsp/lvgl_flush_sync.h"
#include "app_obd_dsp/perf_stats.h"

static SemaphoreHandle_t sem;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t drv = { .draw_buf = &draw_buf };
static _Atomic bool returned;
static unsigned creates, takes, releases, yields, records;
static bool finish_on_take;

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    assert(++creates == 1);
    assert(pthread_mutex_init(&storage->mutex, NULL) == 0);
    assert(pthread_cond_init(&storage->event, NULL) == 0);
    assert(pthread_cond_init(&storage->blocked_event, NULL) == 0);
    return sem = storage;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    assert(handle == sem && ticks == portMAX_DELAY);
    ++takes;
    if (finish_on_take) {
        finish_on_take = false;
        /* ISR lands after wait_cb's flag check but before the actual take. */
        assert(!lvgl_flush_complete_isr(&drv));
    }
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    while (!sem->token) {
        ++sem->blocked;
        assert(pthread_cond_signal(&sem->blocked_event) == 0);
        assert(pthread_cond_wait(&sem->event, &sem->mutex) == 0);
        --sem->blocked;
    }
    sem->token = 0;
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t handle, BaseType_t *woken)
{
    assert(handle == sem);
    /* Model publication ordering: freeing the buffer must precede the signal. */
    assert(!atomic_load(&draw_buf.flushing));
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    bool full = sem->token != 0;
    if (!full) {
        sem->token = 1;
        if (sem->blocked) *woken = pdTRUE;
        assert(pthread_cond_signal(&sem->event) == 0);
    }
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    return full ? pdFALSE : pdTRUE;
}

void lv_disp_flush_ready(lv_disp_drv_t *driver)
{
    assert(driver == &drv);
    ++releases;
    atomic_store(&draw_buf.flushing, 0);
}

void flush_test_yield_from_isr(void) { ++yields; }

#if CONFIG_OBD_PERF_STATS
int64_t esp_timer_get_time(void)
{
    static _Atomic int64_t now;
    return atomic_fetch_add(&now, 10);
}

void perf_stats_record(perf_metric_t metric, int64_t duration_us, uint32_t units)
{
    assert(metric == PERF_LCD_WAIT && duration_us == 10 && units == 1);
    ++records;
}
#endif

static void *waiter(void *arg)
{
    (void)arg;
    lvgl_flush_wait_cb(&drv);
    assert(!atomic_load(&draw_buf.flushing));
    atomic_store(&returned, true);
    return NULL;
}

static void test_blocking_transfer(void)
{
    pthread_t thread;
    unsigned before_takes = takes, before_records = records;
    atomic_store(&returned, false);
    atomic_store(&draw_buf.flushing, 1);
    assert(pthread_create(&thread, NULL, waiter, NULL) == 0);
    assert(pthread_mutex_lock(&sem->mutex) == 0);
    /* Deterministic rendezvous, no sleeps/timing thresholds: the rendering
     * task must genuinely block, even when it first consumes a stale token. */
    while (!sem->blocked) {
        assert(pthread_cond_wait(&sem->blocked_event, &sem->mutex) == 0);
    }
    assert(!atomic_load(&returned));
    assert(pthread_mutex_unlock(&sem->mutex) == 0);
    unsigned before_yields = yields;
    assert(lvgl_flush_complete_isr(&drv));
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&returned) && yields == before_yields + 1);
    assert(takes > before_takes);
    assert(records == before_records + CONFIG_OBD_PERF_STATS);
}

int main(void)
{
    lvgl_flush_sync_init();
    lvgl_flush_sync_init();  /* boot-lifetime object cannot be reset/recreated */
    assert(creates == 1);

    lvgl_flush_wait_cb(&drv);  /* already idle: no semaphore/syscall/metric */
    assert(takes == 0 && releases == 0 && records == 0);

    test_blocking_transfer();

    /* Completion before wait (also models fullscreen video sharing buffers).
     * A token may remain, but idle wait never needs to consume it. */
    atomic_store(&draw_buf.flushing, 1);
    assert(!lvgl_flush_complete_isr(&drv));
    unsigned before_takes = takes;
    lvgl_flush_wait_cb(&drv);
    assert(takes == before_takes);
    test_blocking_transfer();  /* must not treat that old token as DMA-ready */

    /* Back-to-back pre-wait video completions coalesce in the binary semaphore. */
    atomic_store(&draw_buf.flushing, 1);
    assert(!lvgl_flush_complete_isr(&drv));
    atomic_store(&draw_buf.flushing, 1);
    assert(!lvgl_flush_complete_isr(&drv));
    test_blocking_transfer();

    /* The completion at the check/take boundary must not be lost. */
    atomic_store(&draw_buf.flushing, 1);
    finish_on_take = true;
    before_takes = takes;
    unsigned before_records = records;
    lvgl_flush_wait_cb(&drv);
    assert(!finish_on_take && takes == before_takes + 1);
    assert(records == before_records + CONFIG_OBD_PERF_STATS);

    for (unsigned i = 0; i < 256; ++i) {
        if ((i & 1u) == 0) {
            atomic_store(&draw_buf.flushing, 1);
            assert(!lvgl_flush_complete_isr(&drv));
            lvgl_flush_wait_cb(&drv);
        }
        test_blocking_transfer();
    }
    assert(creates == 1);
    printf("LVGL flush sync: %u completions, %u blocking wakes/yields, stats=%d OK\n",
           releases, yields, CONFIG_OBD_PERF_STATS);
    return 0;
}
