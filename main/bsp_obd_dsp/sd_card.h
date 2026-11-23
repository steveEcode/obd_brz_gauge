#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * sd_card - SDMMC slot 1. V1: 1-bit (CLK GPIO14 / CMD GPIO17 / D0 GPIO16).
 * V2/V3: 4-bit (CLK GPIO3 / CMD GPIO4 / D0..D3 GPIO2/GPIO1/GPIO6/GPIO5).
 *
 * - Mount tries 40 MHz, then 20 MHz. Mount failure NEVER formats the card.
 * - No card-detect pin: sd_card_poll() probes for insertion while absent and
 *   checks CMD13 while mounted; two consecutive failures mean "removed".
 * - All file I/O that may touch PSRAM goes through the shared 16 KB internal
 *   DMA bounce buffer (S3 SDMMC cannot DMA into PSRAM). Use sd_card_read() /
 *   sd_card_write() instead of fread()/fwrite() whenever the buffer may be in
 *   PSRAM.
 * - Writes that must survive power loss use sd_card_write_file_atomic()
 *   (write "<path>.part", fsync, rename).
 * -------------------------------------------------------------------------- */

#define SD_MOUNT_POINT   "/sdcard"
#define SD_OBD_ROOT      SD_MOUNT_POINT "/OBD"
#define SD_DIR_IMPORT    SD_OBD_ROOT "/IMPORT"
#define SD_DIR_THEMES    SD_OBD_ROOT "/THEMES"
#define SD_DIR_VIDEO     SD_OBD_ROOT "/VIDEO"
#define SD_DIR_BOOT      SD_OBD_ROOT "/BOOT"
#define SD_DIR_CFG       SD_OBD_ROOT "/CFG"
#define SD_DIR_CATALOG   SD_OBD_ROOT "/CATALOG"
#define SD_DIR_EXPORT    SD_OBD_ROOT "/EXPORT"
#define SD_IMPORT_LOG    SD_DIR_IMPORT "/IMPORT.LOG"
#define SD_BOOT_OBV      SD_DIR_BOOT "/boot.obv"       /* OBV1/OBV2 boot video */
#define SD_BOOT_JPEG     SD_DIR_BOOT "/boot.jpg"       /* baseline JPEG boot still */
#define SD_BOOT_LEGACY   SD_DIR_BOOT "/bootmedia.raw"  /* raw legacy image: [manifest 4K][bin] */

#define SD_IO_BUF_SIZE   (16 * 1024)

typedef enum {
    SD_STATE_DISABLED = 0, /* no SD slot on this board */
    SD_STATE_ABSENT,       /* no card / mount failed, polling for insertion */
    SD_STATE_MOUNTED,
    SD_STATE_ERROR,        /* card present but unusable (e.g. not FAT, exFAT) */
    SD_STATE_EJECTED,      /* user ejected; not remounted until reinserted */
} sd_state_t;

/* Best-effort mount at boot; creates the /OBD tree. Never blocks boot on failure. */
esp_err_t sd_card_init(void);
/* Call from the low-priority worker at the delay returned below. */
void sd_card_poll(void);
/* Milliseconds until the next useful poll; mounted cards are checked each second. */
uint32_t sd_card_poll_wait_ms(void);
/* Flush and unmount; state becomes EJECTED until the card is pulled and reinserted. */
esp_err_t sd_card_eject(void);
/* DESTRUCTIVE. Only from the quiesced OTA maintenance worker, with the storage
 * job lock held and all file users stopped. Erases the whole card, creates one
 * FAT32 partition (32 KiB clusters on >=4 GiB cards; smaller cards adapt), then
 * remounts and recreates /OBD. Tiny cards (<64 MiB) use FAT16/12. Never automatic.
 * Always reboot afterwards, including failures. */
esp_err_t sd_card_format(char *detail, size_t capacity);


sd_state_t  sd_card_get_state(void);
bool        sd_card_is_mounted(void);
uint32_t    sd_card_get_freq_khz(void);
uint64_t    sd_card_get_total_bytes(void);
uint64_t    sd_card_get_free_bytes(void);

/* Shared 16 KB internal DMA buffer. take() returns NULL on timeout. */
uint8_t *sd_card_io_buf_take(TickType_t wait);
void     sd_card_io_buf_give(void);

/* fread/fwrite through the bounce buffer; dst/src may live in PSRAM. */
size_t sd_card_read(FILE *f, void *dst, size_t len);
size_t sd_card_write(FILE *f, const void *src, size_t len);

/* Whole-file helpers (paths are absolute, under SD_MOUNT_POINT). */
esp_err_t sd_card_write_file(const char *path, const void *data, size_t len);
esp_err_t sd_card_write_file_atomic(const char *path, const void *data, size_t len);
/* Read into a caller buffer; *out_len = bytes read. ESP_ERR_INVALID_SIZE if larger. */
esp_err_t sd_card_read_file(const char *path, void *buf, size_t buf_len, size_t *out_len);
/* Read into a fresh PSRAM buffer (caller frees). Fails if larger than max_len. */
esp_err_t sd_card_read_file_alloc(const char *path, size_t max_len, void **out, size_t *out_len);
esp_err_t sd_card_remove(const char *path);
esp_err_t sd_card_rename(const char *src, const char *dst);
esp_err_t sd_card_mkdir(const char *path);
bool      sd_card_exists(const char *path);
/* Append one CRLF-terminated line (IMPORT.LOG). */
esp_err_t sd_card_append_line(const char *path, const char *line);

#ifdef __cplusplus
}
#endif
