#pragma once

#include <stdbool.h>
#include "lvgl.h"

/* Initialize once before registering the LCD completion interrupt. The static
 * semaphore survives all screens/video sessions and does not use task notify 0. */
void lvgl_flush_sync_init(void);

/* LVGL driver wait_cb; called only from task context under the LVGL mutex. */
void lvgl_flush_wait_cb(lv_disp_drv_t *drv);

/* LCD color-transfer completion ISR: release the buffer before waking a waiter.
 * Explicitly requests an ISR yield (the IDF SPI backend ignores callback return
 * values); the return also supports backends that honor the callback contract. */
bool lvgl_flush_complete_isr(lv_disp_drv_t *drv);
