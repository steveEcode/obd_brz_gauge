// boot_media_mount.c — mount the bootmedia SPIFFS partition
#include "app_obd_dsp/boot_media_mount.h"

#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "boot_media";

#define BOOTMEDIA_PARTITION_LABEL "bootmedia"
#define BOOTMEDIA_BASE_PATH       "/bootmedia"
#define BOOTMEDIA_TXN_MARKER      "/bootmedia/.bootmedia_txn.lock"
#define BOOTMEDIA_ACTIVE_TXT      "/bootmedia/boot_block.txt"
#define BOOTMEDIA_ACTIVE_BIN      "/bootmedia/boot_block.bin"
#define BOOTMEDIA_STAGING_TXT     "/bootmedia/boot_block.txt.new"
#define BOOTMEDIA_STAGING_BIN     "/bootmedia/boot_block.bin.new"
#define BOOTMEDIA_BACKUP_TXT      "/bootmedia/boot_block.txt.prev"
#define BOOTMEDIA_BACKUP_BIN      "/bootmedia/boot_block.bin.prev"

static bool s_mounted = false;

static bool path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_if_exists(const char *path)
{
    if (path_exists(path)) {
        remove(path);
    }
}

static bool rename_without_overwrite(const char *from, const char *to)
{
    if (!path_exists(from)) {
        return true;
    }
    if (path_exists(to)) {
        return false;
    }
    return rename(from, to) == 0;
}

static bool rename_overwriting_existing(const char *from, const char *to)
{
    if (!path_exists(from)) {
        return true;
    }
    if (path_exists(to)) {
        remove(to);
    }
    return rename(from, to) == 0;
}

static bool read_marker_phase(char *phase, size_t phase_len)
{
    if (phase_len == 0 || !path_exists(BOOTMEDIA_TXN_MARKER)) {
        return false;
    }

    FILE *marker = fopen(BOOTMEDIA_TXN_MARKER, "rb");
    if (!marker) {
        return false;
    }

    char line[64] = {0};
    bool ok = fgets(line, sizeof(line), marker) != NULL;
    fclose(marker);
    if (!ok) {
        return false;
    }

    char *value = strchr(line, '=');
    if (!value) {
        return false;
    }
    *value++ = '\0';
    line[strcspn(line, "\r\n\t ")] = '\0';
    value[strcspn(value, "\r\n\t ")] = '\0';
    if (strcmp(line, "phase") != 0) {
        return false;
    }

    strncpy(phase, value, phase_len - 1);
    phase[phase_len - 1] = '\0';
    return true;
}

static bool write_marker_phase(const char *phase)
{
    FILE *marker = fopen(BOOTMEDIA_TXN_MARKER, "wb");
    if (!marker) {
        return false;
    }

    bool ok = fprintf(marker, "phase=%s\n", phase) > 0;
    fclose(marker);
    return ok;
}

bool boot_media_mount(void)
{
    if (!s_mounted) {
        esp_vfs_spiffs_conf_t conf = {
            .base_path = BOOTMEDIA_BASE_PATH,
            .partition_label = BOOTMEDIA_PARTITION_LABEL,
            .max_files = 4,  // 减少文件数以降低内存占用
            .format_if_mount_failed = false,
        };

        esp_err_t ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SPIFFS mount failed (label=%s): %s", BOOTMEDIA_PARTITION_LABEL, esp_err_to_name(ret));
            return false;
        }

        s_mounted = true;
        ESP_LOGI(TAG, "SPIFFS mounted at %s", BOOTMEDIA_BASE_PATH);

        // 打印可用空间
        size_t total = 0, used = 0;
        esp_spiffs_info(BOOTMEDIA_PARTITION_LABEL, &total, &used);
        ESP_LOGI(TAG, "SPIFFS total: %zu, used: %zu, free: %zu", total, used, total - used);
    }
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
    return (stat(BOOTMEDIA_ACTIVE_TXT, &st) == 0 &&
            stat(BOOTMEDIA_ACTIVE_BIN, &st) == 0);
}

bool boot_media_recover_previous_if_needed(void)
{
    if (!s_mounted) {
        return false;
    }

    char phase[16] = {0};
    bool has_txn = read_marker_phase(phase, sizeof(phase));
    bool has_prev = path_exists(BOOTMEDIA_BACKUP_TXT) || path_exists(BOOTMEDIA_BACKUP_BIN);
    bool has_staging = path_exists(BOOTMEDIA_STAGING_TXT) || path_exists(BOOTMEDIA_STAGING_BIN);
    bool has_active = boot_media_has_block_video();

    if (!has_txn && !has_prev && !has_staging) {
        return has_active;
    }

    ESP_LOGW(TAG, "Recovering bootmedia transaction (txn=%d phase=%s prev=%d staging=%d active=%d)",
             has_txn, has_txn ? phase : "-", has_prev, has_staging, has_active);

    if (has_txn && strcmp(phase, "committed") == 0) {
        if (!has_active && has_prev) {
            if (path_exists(BOOTMEDIA_BACKUP_TXT)) {
                rename_overwriting_existing(BOOTMEDIA_BACKUP_TXT, BOOTMEDIA_ACTIVE_TXT);
            }
            if (path_exists(BOOTMEDIA_BACKUP_BIN)) {
                rename_overwriting_existing(BOOTMEDIA_BACKUP_BIN, BOOTMEDIA_ACTIVE_BIN);
            }
        } else {
            remove_if_exists(BOOTMEDIA_BACKUP_TXT);
            remove_if_exists(BOOTMEDIA_BACKUP_BIN);
        }

        remove_if_exists(BOOTMEDIA_STAGING_TXT);
        remove_if_exists(BOOTMEDIA_STAGING_BIN);
        remove_if_exists(BOOTMEDIA_TXN_MARKER);
        return boot_media_has_block_video();
    }

    if (path_exists(BOOTMEDIA_BACKUP_TXT)) {
        rename_overwriting_existing(BOOTMEDIA_BACKUP_TXT, BOOTMEDIA_ACTIVE_TXT);
    }
    if (path_exists(BOOTMEDIA_BACKUP_BIN)) {
        rename_overwriting_existing(BOOTMEDIA_BACKUP_BIN, BOOTMEDIA_ACTIVE_BIN);
    }

    remove_if_exists(BOOTMEDIA_STAGING_TXT);
    remove_if_exists(BOOTMEDIA_STAGING_BIN);
    remove_if_exists(BOOTMEDIA_TXN_MARKER);

    return boot_media_has_block_video();
}

bool boot_media_commit_incoming_update(void)
{
    if (!s_mounted) {
        return false;
    }

    if (!path_exists(BOOTMEDIA_STAGING_TXT) || !path_exists(BOOTMEDIA_STAGING_BIN)) {
        ESP_LOGW(TAG, "Bootmedia commit skipped: staging files missing");
        return false;
    }

    if (!write_marker_phase("precommit")) {
        ESP_LOGW(TAG, "Bootmedia marker write failed before commit");
        return false;
    }

    if (path_exists(BOOTMEDIA_ACTIVE_TXT)) {
        if (!rename_without_overwrite(BOOTMEDIA_ACTIVE_TXT, BOOTMEDIA_BACKUP_TXT)) {
            ESP_LOGE(TAG, "Failed to back up boot_block.txt");
            return false;
        }
    }
    if (path_exists(BOOTMEDIA_ACTIVE_BIN)) {
        if (!rename_without_overwrite(BOOTMEDIA_ACTIVE_BIN, BOOTMEDIA_BACKUP_BIN)) {
            ESP_LOGE(TAG, "Failed to back up boot_block.bin");
            return false;
        }
    }

    if (!write_marker_phase("committed")) {
        ESP_LOGW(TAG, "Bootmedia marker write failed after backup");
        return false;
    }

    if (!rename_without_overwrite(BOOTMEDIA_STAGING_TXT, BOOTMEDIA_ACTIVE_TXT) ||
        !rename_without_overwrite(BOOTMEDIA_STAGING_BIN, BOOTMEDIA_ACTIVE_BIN)) {
        ESP_LOGE(TAG, "Failed to promote staged bootmedia files");
        return false;
    }

    remove_if_exists(BOOTMEDIA_BACKUP_TXT);
    remove_if_exists(BOOTMEDIA_BACKUP_BIN);
    remove_if_exists(BOOTMEDIA_TXN_MARKER);

    ESP_LOGI(TAG, "Bootmedia update committed");

    // 上传完成后在后台整理碎片，不影响上传速度
    esp_err_t err = esp_spiffs_gc(BOOTMEDIA_PARTITION_LABEL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GC failed: %s", esp_err_to_name(err));
    }

    return true;
}

bool boot_media_remove_active_block(void)
{
    if (!s_mounted) {
        return false;
    }

    // Unmount first
    boot_media_unmount();

    // Format the entire partition to ensure a clean slate
    esp_err_t err = esp_spiffs_format(BOOTMEDIA_PARTITION_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
        // Try to remount anyway
        boot_media_mount();
        return false;
    }

    // Remount
    if (!boot_media_mount()) {
        return false;
    }

    ESP_LOGI(TAG, "Bootmedia partition formatted and remounted");
    return true;
}

size_t boot_media_get_free_space(void)
{
    if (!s_mounted) return 0;
    size_t total = 0, used = 0;
    esp_spiffs_info(BOOTMEDIA_PARTITION_LABEL, &total, &used);
    return total > used ? total - used : 0;
}
