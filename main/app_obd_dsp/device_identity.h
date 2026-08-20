#pragma once

#include <stdint.h>

typedef struct {
    const char *board_name;
    const char *board_variant;
    const char *lcd_name;
    uint16_t screen_width;
    uint16_t screen_height;
    uint8_t color_bits;
    uint8_t flash_mb;
    uint8_t psram_mb;
    uint8_t ota_slots;
    uint8_t bootmedia_slots;
    uint8_t bootmedia_format_version;
} device_identity_t;

const device_identity_t *device_identity_get(void);
const char *device_identity_manifest_json(void);
