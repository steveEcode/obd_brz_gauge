// boot_media_mount.c — 挂载 bootmedia SPIFFS 分区
#include "app_obd_dsp/boot_media_mount.h"

#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "boot_media";

#define BOOTMEDIA_PARTITION_LABEL "bootmedia"
#define BOOTMEDIA_BASE_PATH       "/bootmedia"

static bool s_mounted = false;

bool boot_media_mount(void)
{
    if (s_mounted) return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = BOOTMEDIA_BASE_PATH,
        .partition_label = BOOTMEDIA_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (label=%s): %s", BOOTMEDIA_PARTITION_LABEL, esp_err_to_name(ret));
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SPIFFS mounted at %s", BOOTMEDIA_BASE_PATH);
    return true;
}

void boot_media_unmount(void)
{
    if (!s_mounted) return;
    esp_vfs_spiffs_unregister(BOOTMEDIA_PARTITION_LABEL);
    s_mounted = false;
}

bool boot_media_has_block_video(void)
{
    if (!s_mounted) return false;
    struct stat st;
    return (stat("/bootmedia/boot_block.txt", &st) == 0 &&
            stat("/bootmedia/boot_block.bin", &st) == 0);
}
