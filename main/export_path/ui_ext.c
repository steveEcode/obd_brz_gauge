// ================================================================
//  ui_ext.c — hand-written extension logic for ui.c
//
//  Migration plan: gradually move the hand-written logic from ui.c my_timerMain() into this file.
//  Migrate one module at a time (showroom / boot_anim / sweep / rpm_warn);
//  move the corresponding static variables along with it, ui.c accesses them via extern.
//
//  Migration completed:
//  - disp_item system → ui_disp_item.c/h (data-item metadata and helpers)
//
//  Current state: framework in place, ui_ext_tick() is empty, logic still lives in ui.c.
//  TODO: migrate showroom state machine (~200 lines)
//  TODO: migrate boot animation (~200 lines)
//  TODO: migrate sweep animation (~150 lines)
//  TODO: migrate rpm warning flash (~50 lines)
// ================================================================

#include "ui_ext.h"
#include "ui.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "app_obd_dsp/boot_block_player.h"
#include "app_obd_dsp/boot_media_mount.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_ext";

void ui_ext_init(void)
{
    ESP_LOGI(TAG, "ui_ext initialized");
}

void ui_ext_tick(void)
{
    // TODO: migrate the following logic from ui.c my_timerMain() into this function:
    // 1. Showroom state machine (showroom_active, slot, tick, sync)
    // 2. Boot animation (video, RACE/AS/ONE, logo delay)
    // 3. Sweep animation (sweep_step, backlight)
    // 4. RPM warning flash (rpm_flash)
    //
    // Migration steps:
    //   a) move the relevant static variables from ui.c to the top of this file
    //   b) move the relevant functions from ui.c into this file
    //   c) add extern declarations for the variables/functions still accessed from ui.c
    //   d) remove the migrated code blocks from my_timerMain()
    //   e) verify with a build
}

bool ui_ext_showroom_is_active(void)
{
    // TODO: after migration, return s_showroom_active internal to ui_ext.c
    extern bool ui_showroom_is_active(void);
    return ui_showroom_is_active();
}

void ui_ext_showroom_fake_data(void)
{
    // TODO: after migrating showroom_fake_data from ui.c here, drop the extern
    // Currently: ui.c's my_timerMain calls showroom_fake_data() directly; this is a stub
}

void ui_ext_showroom_sync_slot(uint8_t slot)
{
    // TODO: after migration, operate directly on ui_ext.c internal state
    extern void ui_showroom_set_page_from_sync(int sweep_step);
    ui_showroom_set_page_from_sync(200 + slot);
}

void ui_ext_showroom_request_enter(void)
{
    // TODO: after migration, set ui_ext.c's internal pending_enter directly
    extern void ui_showroom_set_active(bool en);
    ui_showroom_set_active(true);
}

int ui_ext_intro_get_step(void)
{
    extern int ui_intro_get_step(void);
    return ui_intro_get_step();
}

void ui_ext_intro_set_step(int step)
{
    extern void ui_intro_set_step(int step);
    ui_intro_set_step(step);
}

int ui_ext_sweep_get_step(void)
{
    extern int ui_sweep_get_step(void);
    return ui_sweep_get_step();
}

void ui_ext_rpm_flash_test(void)
{
    // TODO: after migration, operate directly on ui_ext.c's internal test counter
}
