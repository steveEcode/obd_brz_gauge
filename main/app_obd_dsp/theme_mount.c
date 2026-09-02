// theme_mount.c — raw-partition write access to theme_0 for the /ota/theme
// uploader. theme_0 is a single self-contained blob (manifest + assets +
// layout, entirely produced by pack_theme.py / the app's themePacker.ts)
// -- unlike bootmedia there's no separate manifest region at its own fixed
// offset, so there's no split-write ordering trick needed: erase once,
// write the whole blob in one pass. An interrupted write just leaves
// theme_0 corrupt, which theme_loader.c already handles today by falling
// back to the built-in default theme -- no new failure mode introduced.
#include "app_obd_dsp/theme_mount.h"

#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "theme_mount";

#define THEME_PARTITION_LABEL  "theme_0"
#define THEME_WRITE_SCRATCH    16384  // 16 KB internal-RAM scratch buffer.
                                       // PSRAM sources are not safe during
                                       // flash operations; copy into this
                                       // buffer first.

static const esp_partition_t *s_part = NULL;
static uint8_t s_flash_write_buf[THEME_WRITE_SCRATCH];

static const esp_partition_t *get_partition(void)
{
    if (s_part) {
        return s_part;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       (esp_partition_subtype_t)0x82,
                                       THEME_PARTITION_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "theme_0 partition not found");
        return NULL;
    }
    return s_part;
}

bool theme_mount(void)
{
    return get_partition() != NULL;
}

size_t theme_get_partition_size(void)
{
    const esp_partition_t *p = get_partition();
    return p ? p->size : 0;
}

bool theme_erase_all(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) {
        return false;
    }

    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

esp_err_t theme_raw_write(uint32_t offset, const uint8_t *data, size_t len)
{
    const esp_partition_t *p = get_partition();
    if (!p || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint64_t)offset + len > p->size) {
        ESP_LOGE(TAG, "write out of range: off=%lu len=%zu part_size=%lu",
                 (unsigned long)offset, len, (unsigned long)p->size);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t copied = 0;
    while (copied < len) {
        size_t copy_len = len - copied;
        if (copy_len > sizeof(s_flash_write_buf)) {
            copy_len = sizeof(s_flash_write_buf);
        }
        memcpy(s_flash_write_buf, data + copied, copy_len);
        esp_err_t err = esp_partition_write(p, offset + copied, s_flash_write_buf, copy_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed at offset=%lu: %s",
                     (unsigned long)(offset + copied), esp_err_to_name(err));
            return err;
        }
        copied += copy_len;
    }
    return ESP_OK;
}
