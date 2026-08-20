#pragma once

#include <stdbool.h>
#include <stddef.h>

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

// Get available SPIFFS space in bytes; returns 0 if not mounted
size_t boot_media_get_free_space(void);

#ifdef __cplusplus
}
#endif
