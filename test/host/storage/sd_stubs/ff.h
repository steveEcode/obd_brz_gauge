#pragma once
#include <stdint.h>
#define FF_MULTI_PARTITION 1
#define FM_FAT 1
#define FM_FAT32 2
#define FM_EXFAT 4
#define FR_OK 0
#define FR_DISK_ERR 1
typedef int FRESULT;
typedef struct { uint8_t fmt, n_fat; unsigned align, n_root, au_size; } MKFS_PARM;
typedef struct { uint8_t pd, pt; } PARTITION;
extern PARTITION VolToPart[2];
FRESULT f_mkfs(const char *, const MKFS_PARM *, void *, unsigned);
