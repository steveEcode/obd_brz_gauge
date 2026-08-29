#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Raw-partition write access to theme_0, for the /ota/theme uploader.
// theme_0 is a single self-contained 4MB blob (manifest + assets + layout,
// already packed by pack_theme.py / the app's themePacker.ts) -- unlike
// bootmedia there is no separate manifest region with its own offset, so
// this module is just find-partition + erase + write.

// Find the theme_0 partition; returns true on success.
bool theme_mount(void);

// Get theme_0's total size in bytes; returns 0 if not mounted.
size_t theme_get_partition_size(void);

// Erase the whole theme_0 partition.
bool theme_erase_all(void);

// Write `len` bytes at `offset` into theme_0. `data` must be in
// internal RAM or gets copied through one before the flash write (PSRAM
// sources are not safe while the flash cache is disabled).
esp_err_t theme_raw_write(uint32_t offset, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
