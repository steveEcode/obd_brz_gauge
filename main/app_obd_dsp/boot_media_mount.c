// boot_media_mount.c — raw-partition storage for the boot animation.
// The bootmedia partition is a plain `data` partition (not SPIFFS): the boot
// animation is exactly one manifest + one bin, laid out at fixed offsets.
//   offset 0x0000  -> boot_block.txt  (manifest, max 4 KB)
//   offset 0x1000  -> boot_block.bin  (media data)
// Writing goes straight to flash with esp_partition_write (~MB/s), which is
// orders of magnitude faster than SPIFFS for a 6.7 MB upload.
#include "app_obd_dsp/boot_media_mount.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "boot_media";

#define BOOTMEDIA_PARTITION_LABEL "bootmedia"
#define BOOTMEDIA_MANIFEST_OFFSET 0x0000
#define BOOTMEDIA_MANIFEST_MAX    0x1000     // 4 KB
#define BOOTMEDIA_BIN_OFFSET      0x1000     // after the manifest slot
#define BOOTMEDIA_WRITE_SCRATCH   16384    // 16 KB: internal-RAM scratch buffer.
                                            // PSRAM sources are not safe during flash
                                            // operations; copy into this buffer first.

static const esp_partition_t *s_part = NULL;
static uint8_t s_flash_write_buf[BOOTMEDIA_WRITE_SCRATCH];

static const esp_partition_t *get_partition(void)
{
    if (s_part) {
        return s_part;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x82, BOOTMEDIA_PARTITION_LABEL);
    if (!s_part) {
        ESP_LOGE(TAG, "bootmedia partition not found");
        return NULL;
    }
    return s_part;
}

bool boot_media_mount(void)
{
    return get_partition() != NULL;
}

void boot_media_unmount(void)
{
    // Raw partition handle stays valid; nothing to release.
}

static bool manifest_present(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return false;

    uint8_t probe[8] = {0};
    if (esp_partition_read(p, BOOTMEDIA_MANIFEST_OFFSET, probe, sizeof(probe)) != ESP_OK) {
        return false;
    }
    // A valid manifest starts with a printable key like "canvas_width=".
    return probe[0] >= 'a' && probe[0] <= 'z';
}

bool boot_media_has_block_video(void)
{
    return manifest_present();
}

bool boot_media_recover_previous_if_needed(void)
{
    // Raw layout has no backup slot; nothing to recover.
    return boot_media_has_block_video();
}

bool boot_media_commit_incoming_update(void)
{
    // The raw manifest/bin are already written in place by the uploader.
    return boot_media_has_block_video();
}

bool boot_media_remove_active_block(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return false;

    // Erase the whole partition so a fresh animation can be written.
    esp_err_t err = esp_partition_erase_range(p, 0, p->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool boot_media_invalidate_manifest(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return false;

    // Erase only the 4 KB manifest sector.  This immediately invalidates the
    // old animation (manifest_present() will fail) without paying the ~20 s
    // cost of erasing the full 10 MB partition.  The data region is erased
    // on-demand by the uploader once it knows the incoming payload size.
    esp_err_t err = esp_partition_erase_range(p, BOOTMEDIA_MANIFEST_OFFSET, BOOTMEDIA_MANIFEST_MAX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "manifest erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool boot_media_erase_data_region(uint32_t total_size, uint32_t manifest_size)
{
    const esp_partition_t *p = get_partition();
    if (!p || manifest_size == 0 || manifest_size > BOOTMEDIA_MANIFEST_MAX || total_size < manifest_size) {
        return false;
    }

    // The upload is a logical [manifest][bin] stream, while flash has a fixed
    // 4 KB manifest slot.  Erase only the sectors touched by the binary.
    size_t binary_size = total_size - manifest_size;
    if (binary_size == 0) return true;

    uint32_t erase_len = (binary_size + 0xFFFu) & ~0xFFFu;
    if (erase_len > p->size - BOOTMEDIA_BIN_OFFSET) {
        return false;
    }

    esp_err_t err = esp_partition_erase_range(p, BOOTMEDIA_BIN_OFFSET, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data region erase failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

size_t boot_media_get_free_space(void)
{
    const esp_partition_t *p = get_partition();
    if (!p) return 0;
    return p->size - BOOTMEDIA_BIN_OFFSET;
}

/* --- raw read/write helpers used by the uploader and the player --- */

esp_err_t boot_media_raw_write(uint32_t abs_offset, const uint8_t *data, size_t len,
                               uint32_t manifest_size)
{
    const esp_partition_t *p = get_partition();
    if (!p || !data || manifest_size == 0 || manifest_size > BOOTMEDIA_MANIFEST_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // abs_offset is relative to the logical [manifest][bin] stream. Split a
    // segment that crosses the manifest boundary so the binary always starts
    // at the physical 4 KB boundary.
    uint32_t logical_end = abs_offset + (uint32_t)len;
    if (logical_end < abs_offset) return ESP_ERR_INVALID_SIZE;

    size_t consumed = 0;
    while (consumed < len) {
        uint32_t logical_offset = abs_offset + (uint32_t)consumed;
        size_t part_off;
        size_t segment_len;
        if (logical_offset < manifest_size) {
            part_off = BOOTMEDIA_MANIFEST_OFFSET + logical_offset;
            segment_len = manifest_size - logical_offset;
        } else {
            part_off = BOOTMEDIA_BIN_OFFSET + (logical_offset - manifest_size);
            segment_len = len - consumed;
        }
        if (segment_len > len - consumed) segment_len = len - consumed;
        if (part_off + segment_len > p->size) {
            ESP_LOGE(TAG, "raw write out of range: off=%zu len=%zu part_size=%lu",
                     part_off, segment_len, (unsigned long)p->size);
            return ESP_ERR_INVALID_SIZE;
        }

        // esp_partition_write may execute with the flash cache disabled. Copy
        // the source into internal RAM before each write; upload buffers live
        // in PSRAM and are not safe as flash-operation sources on all targets.
        size_t copied = 0;
        while (copied < segment_len) {
            size_t copy_len = segment_len - copied;
            if (copy_len > sizeof(s_flash_write_buf)) copy_len = sizeof(s_flash_write_buf);
            memcpy(s_flash_write_buf, data + consumed + copied, copy_len);
            esp_err_t err = esp_partition_write(p, part_off + copied, s_flash_write_buf, copy_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "raw write failed at logical=%u: %s", logical_offset + (uint32_t)copied,
                         esp_err_to_name(err));
                return err;
            }
            copied += copy_len;
        }
        consumed += segment_len;
    }

    return ESP_OK;
}

esp_err_t boot_media_raw_read_manifest(uint8_t *buf, size_t size)
{
    const esp_partition_t *p = get_partition();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (size > BOOTMEDIA_MANIFEST_MAX) size = BOOTMEDIA_MANIFEST_MAX;
    return esp_partition_read(p, BOOTMEDIA_MANIFEST_OFFSET, buf, size);
}

esp_err_t boot_media_raw_read_bin(uint8_t *buf, size_t size, size_t *out_len)
{
    const esp_partition_t *p = get_partition();
    if (!p) return ESP_ERR_NOT_FOUND;

    size_t avail = p->size - BOOTMEDIA_BIN_OFFSET;
    size_t to_read = size < avail ? size : avail;
    esp_err_t err = esp_partition_read(p, BOOTMEDIA_BIN_OFFSET, buf, to_read);
    if (err != ESP_OK) return err;
    if (out_len) *out_len = to_read;
    return ESP_OK;
}

size_t boot_media_raw_manifest_max(void)
{
    return BOOTMEDIA_MANIFEST_MAX;
}


