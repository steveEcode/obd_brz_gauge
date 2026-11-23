#pragma once
#include "esp_err.h"
typedef unsigned char BYTE;
#define FF_DRV_NOT_USED 0xff
esp_err_t ff_diskio_get_drive(BYTE *drive);
void ff_diskio_register(BYTE drive, const void *impl);
#define ff_diskio_unregister(drive) ff_diskio_register(drive, NULL)
