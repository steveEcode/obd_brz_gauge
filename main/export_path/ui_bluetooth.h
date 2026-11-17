#pragma once
#include "lvgl.h"

/* Hand-written pairing navigation; all entry points run in LVGL context. */
extern lv_obj_t *ui_ScreenPageBluetooth;
extern lv_obj_t *ui_ScreenPageTPMSPairing;
void ui_ScreenPageBluetooth_screen_init(void);
void ui_ScreenPageTPMSPairing_screen_init(void);

extern lv_obj_t *ui_ScreenPageSkyHub;
void ui_ScreenPageSkyHub_screen_init(void);
