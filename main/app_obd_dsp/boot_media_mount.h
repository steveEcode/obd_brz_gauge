#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mount the bootmedia partition (SPIFFS); returns true on success
bool boot_media_mount(void);

// Unmount
void boot_media_unmount(void);

// Check whether the boot_block media files exist
bool boot_media_has_block_video(void);

// Restore the active boot_block pair from the previous backup if an update was interrupted.
bool boot_media_recover_previous_if_needed(void);

// Commit the staged incoming boot_block pair into the active slot, preserving a previous backup.
bool boot_media_commit_incoming_update(void);

// Reset the bootmedia partition so a fresh animation can be written.
bool boot_media_remove_active_block(void);

// Erase only the 4 KB manifest sector to invalidate the current animation.
// Much faster than a full partition erase; the data region is erased later
// by boot_media_erase_data_region() once the incoming size is known.
bool boot_media_invalidate_manifest(void);

// Erase the bin region needed by a logical [manifest][bin] upload.
// Call after boot_media_invalidate_manifest() and once both sizes are known.
bool boot_media_erase_data_region(uint32_t total_size, uint32_t manifest_size);

// Get available SPIFFS space in bytes; returns 0 if not mounted
size_t boot_media_get_free_space(void);


/* Raw-partition access for the boot animation uploader / player. */
// Write one logical [manifest][bin] segment. The manifest is stored in the
// fixed 4 KB slot and the binary starts at physical offset 0x1000.
esp_err_t boot_media_raw_write(uint32_t abs_offset, const uint8_t *data, size_t len,
                               uint32_t manifest_size);
esp_err_t boot_media_raw_read_manifest(uint8_t *buf, size_t size);
esp_err_t boot_media_raw_read_bin(uint8_t *buf, size_t size, size_t *out_len);
size_t boot_media_raw_manifest_max(void);

#ifdef __cplusplus
}
#endif
