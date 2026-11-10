/* Exercise the actual presentation clock, without LVGL/ESP/transport mocks. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "export_path/ui_sweep.h"

static void ratio_is(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.000001f);
}

static void test_master_grid(void)
{
    ui_sweep_clock_t clock = {0};
    assert(ui_sweep_clock_sample(&clock, false, 0).step == 0);
    ui_sweep_clock_start(&clock, 5000);
    /* Independent old callback-based reference at every 40 ms grid point. */
    for (int i = 0; i < 48; ++i) {
        ui_sweep_frame_t f = ui_sweep_clock_sample(&clock, false, 5000 + i * 40);
        assert(f.step == i + 1);
        ratio_is(f.ratio, i < 25 ? (float)(i + 1) / 25 : 1.0f);
        assert(f.backlight == (i >= 38 && i < 43 ? 100 : 3));
    }
    assert(ui_sweep_clock_sample(&clock, false, 6919).step == 48);
    ui_sweep_frame_t end = ui_sweep_clock_sample(&clock, false, 6920);
    assert(end.step == 0 && end.ratio == -1.0f && !clock.running);
    assert(ui_sweep_clock_sample(&clock, false, 9999).ratio == -1.0f);
}

static void test_render_cadence_and_stalls(void)
{
    ui_sweep_clock_t fine = {0}, coarse = {0};
    ui_sweep_clock_start(&fine, 0);
    ui_sweep_clock_start(&coarse, 0);
    float last = -1.0f;
    int different = 0;
    for (int t = 0; t <= 960; t += 16) {
        ui_sweep_frame_t f = ui_sweep_clock_sample(&fine, false, t);
        if (f.ratio != last) ++different;
        assert(f.ratio >= last);
        last = f.ratio;
        /* Calling again at the same time must not advance the animation. */
        ratio_is(ui_sweep_clock_sample(&fine, false, t).ratio, f.ratio);
    }
    assert(different == 61); /* Presentation opportunities, NOT measured FPS. */
    ratio_is(ui_sweep_clock_sample(&fine, false, 960).ratio,
             ui_sweep_clock_sample(&coarse, false, 960).ratio);
    assert(ui_sweep_clock_sample(&coarse, false, 1520).step == 39);
    assert(ui_sweep_clock_sample(&coarse, false, 1720).backlight == 3);
    assert(ui_sweep_clock_sample(&fine, false, 1920).step == 0);
    assert(ui_sweep_clock_sample(&coarse, false, 2500).step == 0);
    /* A very long blocking render must not replay expired frames. */
    ui_sweep_clock_start(&coarse, 3000);
    assert(ui_sweep_clock_sample(&coarse, false, 100000).ratio == -1.0f);
}

static void test_slave_authority(void)
{
    ui_sweep_clock_t clock = {0};
    ui_sweep_clock_sync(&clock, 6, 1000); /* Late join: received ratio = 6/25. */
    ratio_is(ui_sweep_clock_sample(&clock, true, 1000).ratio, 0.24f);
    ratio_is(ui_sweep_clock_sample(&clock, true, 1016).ratio, 0.256f);
    ui_sweep_clock_sync(&clock, 6, 1050); /* Duplicate cannot rewind ramp. */
    ratio_is(ui_sweep_clock_sample(&clock, true, 1050).ratio, 0.29f);
    ui_sweep_clock_sync(&clock, 8, 1100); /* Delayed next packet cannot rewind. */
    ratio_is(ui_sweep_clock_sample(&clock, true, 1100).ratio, 0.34f);
    ui_sweep_clock_sync(&clock, 12, 1120); /* Newer master can jump ahead. */
    ratio_is(ui_sweep_clock_sample(&clock, true, 1120).ratio, 0.48f);
    ui_sweep_frame_t gap = ui_sweep_clock_sample(&clock, true, 10000);
    assert(gap.step == 12 && gap.ratio == 1.0f && gap.backlight == 3);
    assert(clock.running); /* No local flash/end on packet loss. */
    ui_sweep_clock_sync(&clock, 39, 10000);
    assert(ui_sweep_clock_sample(&clock, true, 10000).backlight == 100);
    assert(ui_sweep_clock_sample(&clock, true, 20000).backlight == 100);
    ui_sweep_clock_sync(&clock, 44, 20000);
    assert(ui_sweep_clock_sample(&clock, true, 20000).backlight == 3);
    ui_sweep_clock_sync(&clock, 48, 21000);
    assert(ui_sweep_clock_sample(&clock, true, 22000).step == 48);
    ui_sweep_clock_sync(&clock, 0, 22000);
    assert(ui_sweep_clock_sample(&clock, true, 22000).ratio == -1.0f);
}

static void test_restart_and_large_time(void)
{
    ui_sweep_clock_t clock = {0};
    const int64_t start = INT64_C(5000000000); /* Beyond 32-bit ms uptime. */
    ui_sweep_clock_start(&clock, start);
    ratio_is(ui_sweep_clock_sample(&clock, false, start + 16).ratio, 0.056f);
    assert(ui_sweep_clock_sample(&clock, false, start + 1920).step == 0);
    ui_sweep_clock_sync(&clock, 25, start + 2000);
    ui_sweep_clock_sync(&clock, 1, start + 2100); /* Accepted lower slot: restart. */
    ratio_is(ui_sweep_clock_sample(&clock, true, start + 2100).ratio, 0.04f);
    for (int invalid = -1; invalid <= 255; ++invalid) {
        if (invalid > 0 && invalid <= 48) continue;
        ui_sweep_clock_sync(&clock, invalid, start + 2200);
        assert(!clock.running && clock.synced_step == 0);
        assert(ui_sweep_clock_sample(&clock, true, start + 2200).step == 0);
        ui_sweep_clock_start(&clock, start + 2200);
    }
    ui_sweep_clock_start(&clock, start + 2300);
    ratio_is(ui_sweep_clock_sample(&clock, false, start + 2200).ratio, 0.04f);
}

int main(void)
{
    assert(SWEEP_RENDER_MS == 16 && SWEEP_TICK_MS == 40 && SWEEP_TOTAL == 48);
    test_master_grid();
    test_render_cadence_and_stalls();
    test_slave_authority();
    test_restart_and_large_time();
    puts("sweep clock: grid, elapsed-time cadence, stalls, slave authority, restart OK");
    return 0;
}
