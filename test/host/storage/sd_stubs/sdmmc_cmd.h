#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
typedef struct {
    uint32_t flags;
    int slot, max_freq_khz;
    esp_err_t (*init)(void);
    bool (*check_buffer_alignment)(int, const void *, size_t);
} sdmmc_host_t;
typedef struct {
    int real_freq_khz;
    sdmmc_host_t host;
    struct { char name[8]; } cid;
    struct { uint32_t capacity, sector_size; } csd;
} sdmmc_card_t;
esp_err_t sdmmc_get_status(sdmmc_card_t *card);
esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card);
