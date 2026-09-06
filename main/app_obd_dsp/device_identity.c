#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "esp_log.h"

static const char *TAG = "device_identity";

#ifndef OBD_GAUGE_GIT_BRANCH
#define OBD_GAUGE_GIT_BRANCH "unknown"
#endif

#ifndef OBD_GAUGE_GIT_COUNT
#define OBD_GAUGE_GIT_COUNT 0
#endif

#ifndef OBD_GAUGE_BUILD_TAG
#define OBD_GAUGE_BUILD_TAG "unknown-0-unknown"
#endif

#define OBD_GAUGE_BOARD_NAME        "Waveshare ESP32-S3-Touch-LCD-1.85"
#define OBD_GAUGE_BOARD_VARIANT     "obd_brz_gauge"
#define OBD_GAUGE_LCD_NAME          "ST77916"
#define OBD_GAUGE_FLASH_MB          16u
#define OBD_GAUGE_PSRAM_MB          8u
#define OBD_GAUGE_OTA_SLOTS         2u
#define OBD_GAUGE_BOOTMEDIA_SLOTS   1u
#define OBD_GAUGE_BOOTMEDIA_FORMAT   1u

static const device_identity_t s_identity = {
    .board_name = OBD_GAUGE_BOARD_NAME,
    .board_variant = OBD_GAUGE_BOARD_VARIANT,
    .lcd_name = OBD_GAUGE_LCD_NAME,
    .screen_width = EXAMPLE_LCD_WIDTH,
    .screen_height = EXAMPLE_LCD_HEIGHT,
    .color_bits = EXAMPLE_LCD_COLOR_BITS,
    .flash_mb = OBD_GAUGE_FLASH_MB,
    .psram_mb = OBD_GAUGE_PSRAM_MB,
    .ota_slots = OBD_GAUGE_OTA_SLOTS,
    .bootmedia_slots = OBD_GAUGE_BOOTMEDIA_SLOTS,
    .bootmedia_format_version = OBD_GAUGE_BOOTMEDIA_FORMAT,
};

// /ota/info 只返回手机 App 实际会用到的字段（硬件兼容性校验 + build_tag/branch/count），
// 其余字段（project/version/git/built/idf/slot/theme）App 端未读取，删掉以保证 512 字节内不截断。
static char s_manifest_json[512];

const device_identity_t *device_identity_get(void)
{
    return &s_identity;
}

const char *device_identity_manifest_json(void)
{
    if (s_manifest_json[0] != '\0') {
        return s_manifest_json;
    }

    int written = snprintf(s_manifest_json, sizeof(s_manifest_json),
             "{"
             "\"device\":{"
             "\"board\":\"%s\","
             "\"variant\":\"%s\","
             "\"lcd\":\"%s\","
             "\"screen\":{\"w\":%u,\"h\":%u,\"bpp\":%u},"
             "\"flash_mb\":%u,"
             "\"psram_mb\":%u,"
             "\"ota_slots\":%u,"
             "\"bootmedia_slots\":%u,"
             "\"bootmedia_format\":%u"
             "},"
             "\"firmware\":{"
             "\"build_tag\":\"%s\","
             "\"branch\":\"%s\","
             "\"count\":%u"
             "}"
             "}",
             s_identity.board_name,
             s_identity.board_variant,
             s_identity.lcd_name,
             s_identity.screen_width,
             s_identity.screen_height,
             s_identity.color_bits,
             s_identity.flash_mb,
             s_identity.psram_mb,
             s_identity.ota_slots,
             s_identity.bootmedia_slots,
             s_identity.bootmedia_format_version,
             OBD_GAUGE_BUILD_TAG,
             OBD_GAUGE_GIT_BRANCH,
             (unsigned)OBD_GAUGE_GIT_COUNT);

    if (written < 0 || written >= (int)sizeof(s_manifest_json)) {
        ESP_LOGE(TAG, "manifest JSON truncated (len=%d, cap=%u)", written, (unsigned)sizeof(s_manifest_json));
    }

    s_manifest_json[sizeof(s_manifest_json) - 1] = '\0';
    return s_manifest_json;
}
