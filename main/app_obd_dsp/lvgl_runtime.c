#include "app_obd_dsp/lvgl_runtime.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp_obd_dsp/tpms_ble_client.h"
#include "bsp_obd_dsp/elm327_ble_client.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "media/video_player.h"
#include "app_obd_dsp/perf_stats.h"

#define LVGL_TASK_MAX_DELAY_MS 500u
#define LVGL_TASK_MIN_DELAY_MS 2u

static _Atomic(TaskHandle_t) s_task;

void lvgl_runtime_wake(void)
{
    TaskHandle_t task = atomic_load_explicit(&s_task, memory_order_acquire);
    if (task) xTaskNotify(task, 1u, eSetBits);
}

void lvgl_runtime_task(void *mux)
{
    assert(mux && !atomic_load_explicit(&s_task, memory_order_relaxed));
    atomic_store_explicit(&s_task, xTaskGetCurrentTaskHandle(), memory_order_release);
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        PERF_BEGIN(perf_wait_start);
        if (xSemaphoreTake((SemaphoreHandle_t)mux, portMAX_DELAY) == pdTRUE) {
            PERF_END(PERF_LVGL_WAIT, perf_wait_start, 1);
            PERF_BEGIN(perf_handler_start);
            /* Check the current session even on the first pass: publication
             * before task registration must not lose the first frame. */
            video_player_ui_step();
            static uint32_t tpms_tick;
            if (lv_tick_elaps(tpms_tick) >= 250) {
                tpms_tick = lv_tick_get();
                nvs_user_cfg_t cfg = nvs_cfg_read();
                bool obd_configured = cfg.ble_device_name[0] != '\0';
                for (unsigned i = 0; i < 6; ++i) obd_configured |= cfg.ble_obd_mac[i] != 0;
                tpms_ble_service_step(cfg.device_role == ESPNOW_ROLE_SLAVE || !obd_configured ||
                                      elm327_ble_is_connected());
            }
            delay_ms = lv_timer_handler();
            PERF_END(PERF_LVGL_HANDLER, perf_handler_start, 1);
            xSemaphoreGive((SemaphoreHandle_t)mux);
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        else if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;
        /* No entry clear: a frame published after ui_step but before this wait
         * is latched. Clear on exit coalesces producers; regular timers still
         * use the original bounded deadline. Frame events may wake earlier. */
        xTaskNotifyWait(0, UINT32_MAX, NULL, pdMS_TO_TICKS(delay_ms));
    }
}
