#pragma once
// ================================================================
//  ui_ext.h — hand-written extension logic for ui.c (not overwritten by SquareLine)
//
//  All hand-written UI logic (showroom / boot animation / sweep / RPM warning)
//  lives in ui_ext.c; ui.c only calls it through the functions below.
//  This keeps the hand-written code from being lost when SquareLine Studio re-exports ui.c.
// ================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sweep animation ----
   Values ramp from min to max (~1s) → hold at max (0.5s) → backlight flashes once (max 0.2s → min 0.2s);
   on the same tick after the flash, the configured brightness is restored and real values are switched back in.
   Backlight stays at minimum (SWEEP_BL_MIN) for the whole sweep. Backlight is exported via the sweep step;
   master and slaves derive the same brightness from the same step → the triple-gauge flash stays in sync. */
#define SWEEP_RPM_PEAK   8000   // sweep peak RPM
#define SWEEP_SPEED_PEAK 999    // sweep peak speed
#define SWEEP_TICK_MS    40     // refresh period during sweep (high rate keeps digits ramping smoothly)
#define SWEEP_STEPS_UP   25     // 25×40ms ≈ 1s to ramp from min to max
#define SWEEP_STEPS_HOLD 13     // 13×40ms ≈ 0.5s hold at max
#define SWEEP_STEPS_FLASH 5     // 5×40ms ≈ 0.2s per flash segment (one high and one low segment)
#define SWEEP_BL_MIN     3      // backlight during sweep & the low flash segment (%), tunable
#define SWEEP_BL_MAX     100    // backlight for the high flash segment (%)
#define SWEEP_TOTAL      (SWEEP_STEPS_UP + SWEEP_STEPS_HOLD + 2*SWEEP_STEPS_FLASH)

// Whether the sweep animation is running right now (replaces the old IN_SWEEP macro).
bool ui_ext_sweep_active(void);

// Current sweep progress (0=off, 1..SWEEP_TOTAL=running, 200..209=showroom slot broadcast).
int  ui_ext_sweep_get_step(void);

// Called every timer tick from my_timerMain where the old sweep block lived.
// Advances the sweep (master only), applies/restores the backlight, and returns the
// ramp ratio (0..1) while sweeping, or -1.0f when not sweeping.
float ui_ext_sweep_tick(bool is_slave, uint8_t configured_brightness);

// Called once per tick from my_timerMain: the master triggers the sweep the instant the
// OBD BLE connects (deferring until boot finishes if needed). No-op for slaves.
void ui_ext_sweep_trigger(bool ble_now, bool is_slave);

/* ---- Showroom mode ----
   Minimal design: each gauge runs a fixed-timing loop independently, no per-page ESP-NOW sync needed. */
bool ui_ext_showroom_is_active(void);
void ui_ext_showroom_handle_tap(void);    // 10 rapid taps on the version page enter showroom
void ui_ext_showroom_tick(bool is_slave); // the whole showroom state machine (moved out of my_timerMain)

/* ---- Boot animation / video / intro ---- */
bool ui_ext_boot_video_tick(void);        // video boot mode; returns true to make my_timerMain return early
void ui_ext_intro_tick(bool is_slave);    // RACE/AS/ONE boot animation state machine
void ui_ext_no_signal_update(bool signal_ok); // "NO SIGNAL" overlay on gauge pages

/* ---- RPM warning flash (migrated from ui.c my_timerMain) ---- */
void ui_ext_rpm_flash_tick(uint16_t usRpm, bool in_sweep); // strobe + linked-ramp rendering (called where the old inline block lived)
bool ui_ext_rpm_is_flashing(void);      // stable strobing flag → fast refresh period
bool ui_ext_rpm_link_ramp_active(void); // linked multi-gauge ramp in progress → fast refresh period
bool ui_ext_rpm_warn_possible(void);    // any flash mode enabled → OBD snapshot gate

// Flash period (ms per phase): red and black each last this long; toggle frequency = 1000/(2×period).
// 25ms → 20Hz fast flash. Lower if not fast enough (15→33Hz), raise if too dazzling.
#define RPM_FLASH_PERIOD_MS 25

/* ---- Lifecycle ---- */
void ui_ext_init(void);   // called in app_main after ui_init
void ui_ext_tick(void);   // hand-written hook called from ui_event_easter_egg_background (kept for compatibility)

#ifdef __cplusplus
}
#endif
