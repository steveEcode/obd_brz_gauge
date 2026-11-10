#include "perf_stats.h"

#if CONFIG_OBD_PERF_STATS
#include "perf_hist.h"

#include <inttypes.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *const s_names[PERF_METRIC_COUNT] = {
    "lvgl_wait", "lvgl_handler", "lcd_submit", "ui_tick", "theme_tick", "elm_run",
    "sd_read", "sd_write", "video_fullscreen", "video_composite", "rc_snapshot", "lcd_wait",
    "theme_nav_begin", "theme_nav_images", "theme_nav_publish"
};
static perf_hist_t s_hist[PERF_METRIC_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;

void perf_stats_record(perf_metric_t metric, int64_t duration_us, uint32_t units)
{
    if ((unsigned)metric >= PERF_METRIC_COUNT || duration_us < 0) return;
    uint32_t duration = duration_us > UINT32_MAX ? UINT32_MAX : (uint32_t)duration_us;
    portENTER_CRITICAL(&s_mux);
    perf_hist_add(&s_hist[metric], duration, units);
    portEXIT_CRITICAL(&s_mux);
}

static void perf_task(void *arg)
{
    (void)arg;
    int64_t previous = esp_timer_get_time();
    perf_hist_t snapshot[PERF_METRIC_COUNT];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t now = esp_timer_get_time();
        uint64_t window = (uint64_t)(now - previous);
        previous = now;
        portENTER_CRITICAL(&s_mux);
        for (unsigned i = 0; i < PERF_METRIC_COUNT; i++) {
            snapshot[i] = s_hist[i];
            perf_hist_reset(&s_hist[i]);
        }
        portEXIT_CRITICAL(&s_mux);
        /* Formatting, heap queries, and UART output never hold the metrics lock. */
        for (unsigned i = 0; i < PERF_METRIC_COUNT; i++) {
            const perf_hist_t *h = &snapshot[i];
            if (!h->samples) continue;
            ESP_LOGI("perf", "window_us=%" PRIu64 " metric=%s n=%" PRIu32
                     " total_us=%" PRIu64 " min_us=%" PRIu32 " avg_us=%" PRIu64
                     " p95_le_us=%" PRIu32 " max_us=%" PRIu32 " units=%" PRIu64,
                     window, s_names[i], h->samples, h->total_us, h->min_us,
                     h->total_us / h->samples, perf_hist_percentile_upper(h, 95), h->max_us, h->units);
        }
        ESP_LOGI("perf", "heap internal_free=%u internal_min=%u internal_largest=%u"
                 " psram_free=%u psram_min=%u psram_largest=%u logger_stack_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

void perf_stats_start(void)
{
    if (s_started) return;
    for (unsigned i = 0; i < PERF_METRIC_COUNT; i++) perf_hist_reset(&s_hist[i]);
    if (xTaskCreate(perf_task, "perf_stats", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGW("perf", "cannot create performance logger");
        return;
    }
    s_started = true;
}
#endif
