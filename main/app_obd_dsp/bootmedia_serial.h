#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* app_main only: reserve before LCD/BLE/Wi-Fi, then start after storage/UI are
 * ready. Both calls are idempotent; start retries preparation if it failed.
 * The receiver uses an internal stack because RPCs can write flash/NVS. */
esp_err_t bootmedia_serial_prepare(void);

/* USB Serial/JTAG bridge used by the browser flasher for SD boot-media upload. */
esp_err_t bootmedia_serial_start(void);

#ifdef __cplusplus
}
#endif
