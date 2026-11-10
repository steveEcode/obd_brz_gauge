#include "app_obd_dsp/lvgl_flush_sync.h"

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_obd_dsp/perf_stats.h"

static StaticSemaphore_t s_done_storage;
static SemaphoreHandle_t s_done;

void lvgl_flush_sync_init(void)
{
    if (!s_done) s_done = xSemaphoreCreateBinaryStatic(&s_done_storage);
    assert(s_done);
}

void lvgl_flush_wait_cb(lv_disp_drv_t *drv)
{
    if (!drv->draw_buf->flushing) return;
    PERF_BEGIN(start);
    /* The token is a wakeup, not permission to reuse a buffer. A completion
     * before wait_cb (including fullscreen video's shared DMA buffers) can
     * leave a stale token. Always recheck LVGL's authoritative flushing flag. */
    while (drv->draw_buf->flushing) {
        (void)xSemaphoreTake(s_done, portMAX_DELAY);
    }
    PERF_END(PERF_LCD_WAIT, start, 1);
}

bool lvgl_flush_complete_isr(lv_disp_drv_t *drv)
{
    BaseType_t woken = pdFALSE;
    lv_disp_flush_ready(drv);
    (void)xSemaphoreGiveFromISR(s_done, &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
    return woken == pdTRUE;
}
