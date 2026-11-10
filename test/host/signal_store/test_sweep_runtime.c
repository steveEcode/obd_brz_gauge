#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "export_path/ui_sweep.h"

static int64_t now_us;
static int brightness_calls;
static uint8_t brightness;
static bool s_boot_done, s_showroom_active, s_showroom_pending_enter;
static int s_showroom_pending_slot;
static int64_t esp_timer_get_time(void) { return now_us; }
static void Set_Backlight(uint8_t value) { ++brightness_calls; brightness = value; }
#include "sweep_runtime.inc"

static float tick_at(int64_t ms, bool slave, uint8_t configured)
{
    now_us = ms * 1000;
    return ui_ext_sweep_tick(slave, configured);
}

static void check_ratio(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.000001f);
}

static void test_trigger_and_expiry(void)
{
    assert(!ui_ext_sweep_active());
    assert(tick_at(0, false, 80) == -1.0f && brightness_calls == 0);
    ui_ext_sweep_trigger(true, true); /* Slaves cannot initiate a sweep. */
    assert(!ui_ext_sweep_active());
    ui_ext_sweep_trigger(true, false); /* Connection deferred during boot. */
    assert(s_sweep_pending && !ui_ext_sweep_active());
    s_boot_done = true;
    s_sweep_pending = false;
    sweep_start(); /* Same helper called by the deferred boot destination. */
    check_ratio(tick_at(0, false, 80), 0.04f);
    assert(brightness == 3 && brightness_calls == 1);
    check_ratio(tick_at(16, false, 80), 0.056f);
    assert(ui_sweep_get_step() == 1 && brightness_calls == 1);
    tick_at(100, false, 80);
    assert(ui_ext_sweep_get_step() == 3);
    assert(ui_ext_sweep_active());
    tick_at(1520, false, 80);
    assert(ui_sweep_get_step() == 39 && brightness == 100 && brightness_calls == 2);
    tick_at(1720, false, 80);
    assert(brightness == 3 && brightness_calls == 3);
    assert(tick_at(1920, false, 80) == -1.0f);
    /* The same tick must clear IN_SWEEP, return live ratio, and restore BL. */
    assert(!ui_ext_sweep_active() && ui_sweep_get_step() == 0);
    assert(brightness == 80 && brightness_calls == 4);
    assert(tick_at(2000, false, 80) == -1.0f && brightness_calls == 4);
    ui_ext_sweep_trigger(true, false); /* Already connected: no retrigger. */
    assert(!ui_ext_sweep_active());
    ui_ext_sweep_trigger(false, false);
    ui_ext_sweep_trigger(true, false);
    assert(ui_ext_sweep_active());
    check_ratio(tick_at(2000, false, 5), 0.04f);
    assert(tick_at(4000, false, 5) == -1.0f);
    assert(brightness == 100); /* Existing invalid-config fallback preserved. */
}

static void test_slave_and_showroom(void)
{
    now_us = 5000000;
    ui_showroom_set_page_from_sync(1);
    check_ratio(tick_at(5000, true, 65), 0.04f);
    check_ratio(tick_at(5016, true, 65), 0.056f);
    assert(ui_sweep_get_step() == 1); /* Interpolation is not published state. */
    tick_at(8000, true, 65);
    assert(ui_ext_sweep_active() && brightness == 3);
    ui_showroom_set_page_from_sync(39);
    tick_at(8000, true, 65);
    assert(brightness == 100);
    ui_showroom_set_page_from_sync(0);
    const int before_restore = brightness_calls;
    assert(tick_at(8000, true, 65) == -1.0f && brightness == 65);
    assert(brightness_calls == before_restore + 1);
    tick_at(9000, true, 65);
    assert(brightness_calls == before_restore + 1);
    ui_showroom_set_page_from_sync(10);
    tick_at(9000, true, 65);
    ui_showroom_set_page_from_sync(202); /* Showroom request cancels sweep. */
    assert(s_showroom_pending_enter && s_showroom_pending_slot == -1);
    assert(!ui_ext_sweep_active());
    assert(tick_at(9000, true, 65) == -1.0f && brightness == 65);
    s_showroom_active = true;
    s_sweep_step = 203; /* Encoding used by the showroom tick, not a sweep. */
    ui_showroom_set_page_from_sync(10); /* Ignore normal sweep in showroom. */
    assert(ui_sweep_get_step() == 203 && !ui_ext_sweep_active());
    ui_showroom_set_page_from_sync(205);
    assert(s_showroom_pending_slot == 5);
    s_showroom_active = false;
    ui_showroom_set_page_from_sync(100); /* Legacy showroom encoding. */
    assert(ui_sweep_get_step() == 0);
    ui_showroom_set_page_from_sync(255);
    assert(!ui_ext_sweep_active());
}

int main(void)
{
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageThemeGauge, true, true, true) == 16);
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageSettings, true, false, false) == 16);
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageThemeGauge, false, false, false) == 16);
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageTemp, false, false, false) == 33);
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageSettings, false, false, false) == 200);
    assert(ui_refresh_period_ms_for_screen(ui_ScreenPageRpm, false, true, false) == 25);
    test_trigger_and_expiry();
    test_slave_and_showroom();
    puts("production sweep wrappers: trigger, publication, expiry, brightness, showroom OK");
    return 0;
}
