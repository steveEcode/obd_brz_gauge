#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mount the bootmedia partition (SPIFFS); returns true on success
bool boot_media_mount(void);

// Unmount
void boot_media_unmount(void);

// Check whether the boot_block media files exist
bool boot_media_has_block_video(void);

#ifdef __cplusplus
}
#endif
