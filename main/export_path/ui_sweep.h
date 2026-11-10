#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SWEEP_RPM_PEAK    8000
#define SWEEP_SPEED_PEAK  999
#define SWEEP_TICK_MS     40  /* Existing coarse ESP-NOW step duration, not render cadence. */
#define SWEEP_RENDER_MS   16
#define SWEEP_STEPS_UP    25
#define SWEEP_STEPS_HOLD  13
#define SWEEP_STEPS_FLASH 5
#define SWEEP_BL_MIN      3
#define SWEEP_BL_MAX      100
#define SWEEP_TOTAL      (SWEEP_STEPS_UP + SWEEP_STEPS_HOLD + 2 * SWEEP_STEPS_FLASH)

/* LVGL-task-owned presentation state, not persisted or sent on the wire.
 * now_ms is the nonnegative monotonic ESP timer time in milliseconds. */
typedef struct {
    int64_t start_ms;
    uint8_t synced_step;
    bool running;
} ui_sweep_clock_t;

typedef struct {
    float ratio;             /* -1 when inactive; otherwise 0..1 */
    uint8_t step;            /* 0=inactive, 1..SWEEP_TOTAL=existing sync slots */
    uint8_t backlight;
} ui_sweep_frame_t;

#ifdef __cplusplus
extern "C" {
#endif

void ui_sweep_clock_start(ui_sweep_clock_t *clock, int64_t now_ms);
void ui_sweep_clock_sync(ui_sweep_clock_t *clock, int step, int64_t now_ms);
ui_sweep_frame_t ui_sweep_clock_sample(ui_sweep_clock_t *clock, bool is_slave,
                                     int64_t now_ms);

#ifdef __cplusplus
}
#endif
