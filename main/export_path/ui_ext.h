#pragma once
// ================================================================
//  ui_ext.h — hand-written extension logic for ui.c (not overwritten by SquareLine)
//
//  All hand-written UI logic (showroom / boot animation / sweep / RPM warning)
//  should live in ui_ext.c; ui.c only calls it via ui_ext_tick().
//  This keeps the hand-written code from being lost when SquareLine Studio re-exports ui.c.
// ================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once every 50 ms by my_timerMain
void ui_ext_tick(void);

// Initialization (called in app_main after ui_init)
void ui_ext_init(void);

// Showroom state query (used by the poll task)
bool ui_ext_showroom_is_active(void);

// Showroom fake data generation (called by the poll task in showroom mode)
void ui_ext_showroom_fake_data(void);

// ESP-NOW sync interface (called by espnow_link.c)
void ui_ext_showroom_sync_slot(uint8_t slot);
void ui_ext_showroom_request_enter(void);

// Boot animation sync interface
int  ui_ext_intro_get_step(void);
void ui_ext_intro_set_step(int step);
int  ui_ext_sweep_get_step(void);

// RPM warning test (called by the settings page)
void ui_ext_rpm_flash_test(void);

#ifdef __cplusplus
}
#endif
