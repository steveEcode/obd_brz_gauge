#pragma once

#include "sdkconfig.h"
#include <stdint.h>

/* These intervals can overlap (UI/theme are inside lv_timer_handler).
 * The sum of their times is not CPU utilization or end-to-end latency. */
typedef enum {
    PERF_LVGL_WAIT,
    PERF_LVGL_HANDLER,
    PERF_LCD_SUBMIT,
    PERF_UI_TICK,
    PERF_THEME_TICK,
    PERF_ELM_RUN,
    PERF_SD_READ,
    PERF_SD_WRITE,
    PERF_VIDEO_FULLSCREEN,
    PERF_VIDEO_COMPOSITE,
    PERF_RC_SNAPSHOT,
    PERF_LCD_WAIT,
    PERF_THEME_NAV_BEGIN,
    PERF_THEME_NAV_IMAGES,
    PERF_THEME_NAV_PUBLISH,
    PERF_METRIC_COUNT
} perf_metric_t;

#if CONFIG_OBD_PERF_STATS
#include "esp_timer.h"
void perf_stats_start(void);
void perf_stats_record(perf_metric_t metric, int64_t duration_us, uint32_t units);
#define PERF_BEGIN(name) int64_t name = esp_timer_get_time()
#define PERF_RECORD(metric, elapsed, units) perf_stats_record((metric), (elapsed), (units))
#define PERF_END(metric, name, units) PERF_RECORD((metric), esp_timer_get_time() - (name), (units))
#else
static inline void perf_stats_start(void) {}
#define PERF_BEGIN(name) do {} while (0)
#define PERF_RECORD(metric, elapsed, units) do {} while (0)
#define PERF_END(metric, name, units) do {} while (0)
#endif
