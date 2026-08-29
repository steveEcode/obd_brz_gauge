#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#ifndef OBD_GAUGE_GIT_HASH
#define OBD_GAUGE_GIT_HASH "unknown"
#endif

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
#define OBD_GAUGE_THEME_SLOTS       1u
#define OBD_GAUGE_THEME_PARTITION_SIZE  (4u * 1024u * 1024u)

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

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    const char *project_name = (app && app->project_name[0] != '\0') ? app->project_name : s_identity.board_variant;
    const char *project_version = (app && app->version[0] != '\0') ? app->version : "unknown";
    const char *running_label = running ? running->label : "unknown";
    const char *idf_version = (app && app->idf_ver[0] != '\0') ? app->idf_ver : "unknown";

    snprintf(s_manifest_json, sizeof(s_manifest_json),
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
             "\"bootmedia_format\":%u,"
             "\"theme\":{\"slots\":%u,\"partition_size\":%u,\"writable\":true}"
             "},"
             "\"firmware\":{"
             "\"project\":\"%s\","
             "\"version\":\"%s\","
             "\"build_tag\":\"%s\","
             "\"git\":\"%s\","
             "\"branch\":\"%s\","
             "\"count\":%u,"
             "\"built\":\"%s %s\","
             "\"idf\":\"%s\","
             "\"slot\":\"%s\""
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
             OBD_GAUGE_THEME_SLOTS,
             OBD_GAUGE_THEME_PARTITION_SIZE,
             project_name,
             project_version,
             OBD_GAUGE_BUILD_TAG,
             OBD_GAUGE_GIT_HASH,
             OBD_GAUGE_GIT_BRANCH,
             (unsigned)OBD_GAUGE_GIT_COUNT,
             __DATE__,
             __TIME__,
             idf_version,
             running_label);

    s_manifest_json[sizeof(s_manifest_json) - 1] = '\0';
    return s_manifest_json;
}
