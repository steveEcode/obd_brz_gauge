#pragma once
#include "diskio_impl.h"
#include "sdmmc_cmd.h"
void ff_diskio_register_sdmmc(BYTE drive, sdmmc_card_t *card);
