#include "ui_sweep.h"

void ui_sweep_clock_start(ui_sweep_clock_t *clock, int64_t now_ms)
{
    clock->start_ms = now_ms;
    clock->synced_step = 1;
    clock->running = true;
}

void ui_sweep_clock_sync(ui_sweep_clock_t *clock, int step, int64_t now_ms)
{
    if (step <= 0 || step > SWEEP_TOTAL) {
        clock->running = false;
        clock->synced_step = 0;
        return;
    }
    const int64_t anchor = now_ms - (int64_t)(step - 1) * SWEEP_TICK_MS;
    /* A repeated packet must not restart the visual clock. Catch up to a newer
     * step if necessary, but never pull a running ramp backward. A lower step
     * is treated as a restarted sweep, matching the existing snapshot contract.
     * The slot alone cannot distinguish a restart from an out-of-order packet;
     * no stronger transport ordering guarantee is assumed here. */
    if (!clock->running || step < clock->synced_step || anchor < clock->start_ms)
        clock->start_ms = anchor;
    clock->synced_step = (uint8_t)step;
    clock->running = true;
}

ui_sweep_frame_t ui_sweep_clock_sample(ui_sweep_clock_t *clock, bool is_slave,
                                     int64_t now_ms)
{
    ui_sweep_frame_t frame = { .ratio = -1.0f };
    if (!clock->running) return frame;
    const int64_t elapsed = now_ms > clock->start_ms ? now_ms - clock->start_ms : 0;
    if (!is_slave && elapsed >= (int64_t)SWEEP_TOTAL * SWEEP_TICK_MS) {
        clock->running = false;
        clock->synced_step = 0;
        return frame;
    }
    /* Only the presentation ratio runs between slave messages. Backlight and
     * termination keep following the master's authoritative coarse step. */
    frame.step = is_slave ? clock->synced_step : (uint8_t)(elapsed / SWEEP_TICK_MS + 1);
    /* Preserve the original 1/25 first sample and all 40 ms grid samples. */
    frame.ratio = elapsed >= (SWEEP_STEPS_UP - 1) * SWEEP_TICK_MS
        ? 1.0f : (float)(elapsed + SWEEP_TICK_MS) / (SWEEP_STEPS_UP * SWEEP_TICK_MS);
    frame.backlight = frame.step > SWEEP_STEPS_UP + SWEEP_STEPS_HOLD &&
                      frame.step <= SWEEP_STEPS_UP + SWEEP_STEPS_HOLD + SWEEP_STEPS_FLASH
        ? SWEEP_BL_MAX : SWEEP_BL_MIN;
    return frame;
}
