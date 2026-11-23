/*
 * SD card driver for ESP32-S3 (SDMMC slot 1).
 * Hardware (V1 Waveshare 1.85): 1-bit, CLK=GPIO14, CMD=GPIO17, D0=GPIO16,
 * D3 = TCA9554 EXIO3 with a 10K pull-up (high from power-on, so SD mode is
 * selected even before EXIO_Init()).
 * Hardware (V2/V3): 4-bit, CLK=GPIO3, CMD=GPIO4, D0..D3=GPIO2/GPIO1/GPIO6/GPIO5.
 *
 * - Mount tries 40 MHz, then 20 MHz. Mount failure NEVER formats the card.
 * - V2/V3 start in 4-bit and fall back to 1-bit with DAT1..DAT3 parked high.
 * - No card-detect pin: while absent/error/ejected, sd_card_poll() probes the
 *   bus every few seconds; while mounted it sends CMD13 and declares the card
 *   removed after two consecutive failures.
 * - One 16 KB internal DMA-capable bounce buffer is allocated at init and
 *   shared through sd_card_io_buf_take()/give() (S3 SDMMC cannot DMA to PSRAM).
 */

#include "sdkconfig.h"
#include "sd_card.h"
#include "sd_io_guard.h"
#include "app_obd_dsp/perf_stats.h"

#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "sd_card";

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "bsp_obd_dsp/hw/hw_board.h"

/* SD pins come from the selected board profile (bsp_obd_dsp/hw/hw_board.h).
 * V1 has a 1-bit TF slot; V2/V3 are wired 4-bit. */
#define SD_PIN_CLK          HW_SD_PIN_CLK
#define SD_PIN_CMD          HW_SD_PIN_CMD
#define SD_PIN_D0           HW_SD_PIN_D0
#if HW_SD_BUS_WIDTH > 1
#define SD_PIN_D1           HW_SD_PIN_D1
#define SD_PIN_D2           HW_SD_PIN_D2
#define SD_PIN_D3           HW_SD_PIN_D3
#endif


#define SD_MAX_FILES        6
#define SD_ALLOC_UNIT       (32 * 1024)
#define SD_FREQ_FAST_KHZ    SDMMC_FREQ_HIGHSPEED   /* 40 MHz */
#define SD_FREQ_SLOW_KHZ    SDMMC_FREQ_DEFAULT     /* 20 MHz */

#define SD_PROBE_ABSENT_MS  3000    /* insertion probe cadence while absent */
#define SD_PROBE_ERROR_MS   10000   /* retry cadence for an unmountable card */
#define SD_REMOVE_FAILS     2       /* consecutive CMD13 failures = removed */
#define SD_PATH_MAX         264

static SemaphoreHandle_t s_lock;        /* guards mount/unmount/state changes */
static SemaphoreHandle_t s_io_mutex;    /* guards s_io_buf */
static StaticSemaphore_t s_lock_storage, s_io_mutex_storage;
static uint8_t          *s_io_buf;      /* SD_IO_BUF_SIZE, DMA | INTERNAL, never freed */
static sdmmc_card_t     *s_card;
static volatile sd_state_t s_state = SD_STATE_ABSENT;
static bool              s_inited;
static uint32_t          s_freq_khz;
static uint32_t          s_pref_khz = SD_FREQ_FAST_KHZ;
#if HW_SD_BUS_WIDTH > 1
static bool              s_pref_4bit = true;  /* last width that worked; retry it first */
#endif
static uint8_t           s_fail_cnt;    /* CMD13 failures (mounted) / probe misses (ejected) */
static TickType_t        s_last_probe;

static esp_err_t ensure_directories(void);

/* Tags whose E/W lines would spam the console on every no-card probe. */
static const char *const s_quiet_tags[] = {
    "vfs_fat_sdmmc", "sdmmc_common", "sdmmc_sd", "sdmmc_init", "sdmmc_cmd",
    "sdmmc_req", "sdmmc_periph", "diskio_sdmmc", "vfs_fat",
};
#define QUIET_N (sizeof(s_quiet_tags) / sizeof(s_quiet_tags[0]))

static void quiet_logs(bool quiet, esp_log_level_t saved[QUIET_N])
{
    for (size_t i = 0; i < QUIET_N; i++) {
        if (quiet) {
            saved[i] = esp_log_level_get(s_quiet_tags[i]);
            esp_log_level_set(s_quiet_tags[i], ESP_LOG_NONE);
        } else {
            esp_log_level_set(s_quiet_tags[i], saved[i]);
        }
    }
}

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

#if HW_SD_BUS_WIDTH > 1
/* sdmmc_host_deinit() resets slot pins to floating. Before a 1-bit attempt,
 * hold DAT1..DAT3 as pulled-up inputs: DAT3 must idle high or the card selects
 * SPI mode instead of SD and stops answering on CMD. */
static void park_data_pins_high(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(SD_PIN_D1) | BIT64(SD_PIN_D2) | BIT64(SD_PIN_D3),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}
#endif

static void slot_config(sdmmc_slot_config_t *slot, bool four_bit)
{
    sdmmc_slot_config_t s = SDMMC_SLOT_CONFIG_DEFAULT();
    s.clk = SD_PIN_CLK;
    s.cmd = SD_PIN_CMD;
    s.d0  = SD_PIN_D0;
#if HW_SD_BUS_WIDTH > 1
    if (four_bit) {
        s.width = 4;
        s.d1 = SD_PIN_D1;
        s.d2 = SD_PIN_D2;
        s.d3 = SD_PIN_D3;
    } else {
        s.width = 1;
    }
    s.d4 = s.d5 = s.d6 = s.d7 = GPIO_NUM_NC;
#else
    (void)four_bit;
    s.width = 1;
    s.d1 = s.d2 = s.d3 = s.d4 = s.d5 = s.d6 = s.d7 = GPIO_NUM_NC;
#endif
    s.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    *slot = s;
}

static void host_config(sdmmc_host_t *host, uint32_t khz, bool four_bit)
{
    sdmmc_host_t h = SDMMC_HOST_DEFAULT();
#if HW_SD_BUS_WIDTH > 1
    h.flags = (four_bit ? SDMMC_HOST_FLAG_4BIT : SDMMC_HOST_FLAG_1BIT) |
              SDMMC_HOST_FLAG_DEINIT_ARG;
#else
    (void)four_bit;
    h.flags = SDMMC_HOST_FLAG_1BIT | SDMMC_HOST_FLAG_DEINIT_ARG;
#endif
    h.max_freq_khz = (int)khz;
    *host = h;
}

/* Mount FAT at one frequency/width. ESP_FAIL = card answered but no usable FAT. */
static esp_err_t mount_at(uint32_t khz, bool four_bit)
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,      /* NEVER format the user's card */
        .max_files = SD_MAX_FILES,
        .allocation_unit_size = SD_ALLOC_UNIT,
    };
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    host_config(&host, khz, four_bit);
    slot_config(&slot, four_bit);
#if HW_SD_BUS_WIDTH > 1
    if (!four_bit) park_data_pins_high();
#endif

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &card);
    if (err == ESP_OK) {
        s_card = card;
        s_freq_khz = card->real_freq_khz > 0 ? (uint32_t)card->real_freq_khz : khz;
    }
    return err;
}

/* Called with s_lock held. Tries preferred freq, then 20 MHz, then 1-bit. */
static esp_err_t do_mount_locked(bool quiet)
{
    esp_log_level_t saved[QUIET_N];
    if (quiet) quiet_logs(true, saved);

    bool four_bit = false;
#if HW_SD_BUS_WIDTH > 1
    four_bit = s_pref_4bit;
#endif
    esp_err_t err = mount_at(s_pref_khz, four_bit);
    /* No card at all answers with a timeout during identification (400 kHz);
     * a lower bus clock cannot help then, so only fall back on other errors. */
    if (err != ESP_OK && err != ESP_FAIL && s_pref_khz != SD_FREQ_SLOW_KHZ &&
        !(quiet && err == ESP_ERR_TIMEOUT)) {
        if (!quiet) ESP_LOGW(TAG, "mount at %u kHz failed (%s), retrying at %u kHz",
                             (unsigned)s_pref_khz, esp_err_to_name(err), (unsigned)SD_FREQ_SLOW_KHZ);
        err = mount_at(SD_FREQ_SLOW_KHZ, four_bit);
        if (err == ESP_OK) s_pref_khz = SD_FREQ_SLOW_KHZ;
    }
#if HW_SD_BUS_WIDTH > 1
    /* 4-bit can fail on miswired DAT1..DAT3 while 1-bit still works; retry with
     * those pins parked high (see park_data_pins_high()). */
    if (four_bit && err != ESP_OK && err != ESP_FAIL && !(quiet && err == ESP_ERR_TIMEOUT)) {
        if (!quiet) ESP_LOGW(TAG, "4-bit mount failed (%s), retrying 1-bit", esp_err_to_name(err));
        err = mount_at(s_pref_khz, false);
        if (err == ESP_OK) s_pref_4bit = false;
    }
#endif

    if (quiet) quiet_logs(false, saved);

    if (err == ESP_OK) {
        s_state = SD_STATE_MOUNTED;
        s_fail_cnt = 0;
        ESP_LOGI(TAG, "mounted %s: %s, %u MB, %u kHz", SD_MOUNT_POINT,
                 s_card->cid.name, (unsigned)(((uint64_t)s_card->csd.capacity *
                 s_card->csd.sector_size) >> 20), (unsigned)s_freq_khz);
    } else if (err == ESP_FAIL) {
        s_state = SD_STATE_ERROR;
        ESP_LOGW(TAG, "card present but no FAT volume (exFAT/unformatted?) - not formatting");
    } else {
        s_state = SD_STATE_ABSENT;
        if (!quiet) ESP_LOGI(TAG, "no SD card (%s)", esp_err_to_name(err));
    }
    s_last_probe = xTaskGetTickCount();
    return err;
}

/* Called with s_lock held. */
static void unmount_locked(void)
{
    if (s_card) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        if (err != ESP_OK) ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
        s_card = NULL;
    }
    s_freq_khz = 0;
}

/* Destructive maintenance is deliberately separate from mount/recovery. The
 * raw disk registration works even when the old filesystem cannot be mounted.
 * No VFS FILE may be alive here; caller has quiesced OTA and holds job lock. */
esp_err_t sd_card_format(char *detail, size_t capacity)
{
    if (detail && capacity) detail[0] = '\0';
    if (!s_lock || !s_io_mutex) return ESP_ERR_INVALID_STATE;
    lock();
    xSemaphoreTake(s_io_mutex, portMAX_DELAY);
    sd_io_trace_t trace;
    sd_io_trace_begin(&trace);
    const char *stage = "unmount";
    esp_err_t err = ESP_OK;
    FRESULT fr = FR_OK;
    BYTE drive = FF_DRV_NOT_USED;
    bool host_up = false;
    void *work = NULL;
    sdmmc_card_t *card = NULL;
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    if (s_card) {
        err = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    s_state = SD_STATE_ERROR;
    s_freq_khz = 0;
    if (err != ESP_OK) goto done;
    /* Conservative 1-bit / 20 MHz during destructive writes on every board. */
    host_config(&host, SD_FREQ_SLOW_KHZ, false);
    slot_config(&slot, false);
#if HW_SD_BUS_WIDTH > 1
    park_data_pins_high();
#endif
    stage = "allocate format buffers";
    card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    work = heap_caps_malloc(4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!card || !work) { err = ESP_ERR_NO_MEM; goto done; }
    stage = "initialize SD host";
    err = host.init();
    if (err != ESP_OK) goto done;
    host_up = true;
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) goto done;
    stage = "identify SD card";
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) goto done;
    stage = "register SD disk";
    err = ff_diskio_get_drive(&drive);
    if (err != ESP_OK) goto done;
    ff_diskio_register_sdmmc(drive, card);
    char path[4] = {(char)('0' + drive), ':', 0};
    /* Auto-detected physical-drive mapping: f_mkfs creates a fresh single
     * MBR partition. Exclude FM_EXFAT even though read support is enabled. */
    uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
#if FF_MULTI_PARTITION
    if (VolToPart[drive].pd != drive || VolToPart[drive].pt != 0) {
        stage = "unsupported SD partition mapping";
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
#endif
    MKFS_PARM opt = {.fmt = bytes >= 64ULL * 1024 * 1024 ? FM_FAT32 : FM_FAT,
                     .n_fat = 2, .align = (1024 * 1024) / card->csd.sector_size, .n_root = 0,
                     .au_size = bytes >= 4ULL * 1024 * 1024 * 1024 ? 32768 : 0};
    stage = "create FAT filesystem";
    fr = f_mkfs(path, &opt, work, 4096);
    if (fr != FR_OK) err = ESP_FAIL;
done:
    if (drive != FF_DRV_NOT_USED) ff_diskio_unregister(drive);
    if (host_up) sdmmc_host_deinit();
    free(work);
    free(card);
    if (err == ESP_OK) {
        stage = "remount formatted SD";
        s_pref_khz = SD_FREQ_SLOW_KHZ;
        err = do_mount_locked(false);
    }
    xSemaphoreGive(s_io_mutex);
    unlock();
    if (err == ESP_OK) {
        stage = "create /OBD directories";
        err = ensure_directories();
    }
    sd_io_trace_end(&trace);
    if (err != ESP_OK && detail && capacity) {
        snprintf(detail, capacity, "%s: %s(0x%x) fatfs=%d", stage,
                 esp_err_to_name(err), (unsigned)err, (int)fr);
        size_t used = strlen(detail);
        sd_io_trace_format(&trace, detail + used, capacity - used);
    }
    return err;
}

/* Bare bus probe without mounting (used while EJECTED to notice removal). */
static bool probe_card_present_locked(void)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    /* Presence probing only needs CMD, so stay in 1-bit regardless of the
     * configured width; DAT3 parked high keeps the card in SD mode. */
    host_config(&host, SD_FREQ_SLOW_KHZ, false);
    slot_config(&slot, false);
#if HW_SD_BUS_WIDTH > 1
    park_data_pins_high();
#endif

    esp_log_level_t saved[QUIET_N];
    quiet_logs(true, saved);
    bool present = false;
    if (host.init() == ESP_OK) {
        if (sdmmc_host_init_slot(host.slot, &slot) == ESP_OK) {
            sdmmc_card_t *card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_DEFAULT);
            if (card) {
                present = (sdmmc_card_init(&host, card) == ESP_OK);
                free(card);
            }
        }
        sdmmc_host_deinit();        /* we are the only SDMMC user */
    }
    quiet_logs(false, saved);
    return present;
}

static uint32_t ms_since(TickType_t t)
{
    return (uint32_t)((xTaskGetTickCount() - t) * portTICK_PERIOD_MS);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

esp_err_t sd_card_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_io_mutex) s_io_mutex = xSemaphoreCreateMutexStatic(&s_io_mutex_storage);
    if (!s_lock || !s_io_mutex) return ESP_ERR_NO_MEM;
    sd_io_guard_init();
    if (!s_io_buf) {
        s_io_buf = heap_caps_malloc(SD_IO_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_io_buf) ESP_LOGW(TAG, "no internal DMA RAM for bounce buffer; PSRAM I/O will be slow");
    }

    lock();
    s_inited = true;
    if (s_state == SD_STATE_MOUNTED) {
        unlock();
        return ESP_OK;
    }
    esp_err_t err = do_mount_locked(false);
    unlock();

    if (err == ESP_OK) ensure_directories();
    return err;
}

void sd_card_poll(void)
{
    if (!s_lock || !s_inited) return;
    bool mounted_now = false;

    lock();
    switch (s_state) {
    case SD_STATE_MOUNTED:
        if (s_card && sdmmc_get_status(s_card) == ESP_OK) {
            s_fail_cnt = 0;
        } else if (++s_fail_cnt >= SD_REMOVE_FAILS) {
            ESP_LOGW(TAG, "card removed");
            s_state = SD_STATE_ABSENT;          /* stop new users first */
            vTaskDelay(pdMS_TO_TICKS(100));     /* let in-flight I/O fail out */
            unmount_locked();
            s_fail_cnt = 0;
            s_last_probe = xTaskGetTickCount();
        }
        break;
    case SD_STATE_ABSENT:
        if (ms_since(s_last_probe) >= SD_PROBE_ABSENT_MS) {
            mounted_now = (do_mount_locked(true) == ESP_OK);
        }
        break;
    case SD_STATE_ERROR:
        if (ms_since(s_last_probe) >= SD_PROBE_ERROR_MS) {
            esp_err_t err = do_mount_locked(true);
            mounted_now = (err == ESP_OK);
            if (err != ESP_OK && err != ESP_FAIL) ESP_LOGI(TAG, "unusable card removed");
        }
        break;
    case SD_STATE_EJECTED:
        if (ms_since(s_last_probe) >= SD_PROBE_ABSENT_MS) {
            s_last_probe = xTaskGetTickCount();
            if (probe_card_present_locked()) {
                s_fail_cnt = 0;
            } else if (++s_fail_cnt >= SD_REMOVE_FAILS) {
                ESP_LOGI(TAG, "ejected card pulled; waiting for insertion");
                s_state = SD_STATE_ABSENT;
                s_fail_cnt = 0;
            }
        }
        break;
    default:
        break;
    }
    unlock();

    if (mounted_now) ensure_directories();
}

uint32_t sd_card_poll_wait_ms(void)
{
    uint32_t interval_ms;
    uint32_t elapsed_ms;
    uint32_t wait_ms;

    /* A concurrent eject can only shorten this hint to 1 s; no lock is needed
     * on the usual mounted path. Non-mounted deadlines need a coherent pair. */
    if (!s_lock || !s_inited || s_state == SD_STATE_MOUNTED) return 1000;
    lock();
    switch (s_state) {
    case SD_STATE_MOUNTED: interval_ms = 1000; break;
    case SD_STATE_ERROR:   interval_ms = SD_PROBE_ERROR_MS; break;
    case SD_STATE_ABSENT:
    case SD_STATE_EJECTED: interval_ms = SD_PROBE_ABSENT_MS; break;
    default:               interval_ms = 1000; break;
    }
    if (s_state == SD_STATE_MOUNTED) {
        wait_ms = interval_ms;
    } else {
        elapsed_ms = ms_since(s_last_probe);
        wait_ms = elapsed_ms >= interval_ms ? 1 : interval_ms - elapsed_ms;
    }
    unlock();
    return wait_ms;
}

esp_err_t sd_card_eject(void)
{
    /* Cheap early-out: callable from the LVGL task without waiting on a probe. */
    if (!s_lock || s_state != SD_STATE_MOUNTED) return ESP_ERR_INVALID_STATE;
    lock();
    if (s_state != SD_STATE_MOUNTED) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_state = SD_STATE_EJECTED;
    unmount_locked();
    s_fail_cnt = 0;
    s_last_probe = xTaskGetTickCount();
    unlock();
    ESP_LOGI(TAG, "ejected; safe to remove");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

sd_state_t  sd_card_get_state(void)       { return s_state; }
bool        sd_card_is_mounted(void)      { return s_state == SD_STATE_MOUNTED; }
uint32_t    sd_card_get_freq_khz(void)    { return sd_card_is_mounted() ? s_freq_khz : 0; }

uint64_t sd_card_get_total_bytes(void)
{
    if (!sd_card_is_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK) return total;
    sdmmc_card_t *c = s_card;
    return c ? (uint64_t)c->csd.capacity * c->csd.sector_size : 0;
}

uint64_t sd_card_get_free_bytes(void)
{
    if (!sd_card_is_mounted()) return 0;
    uint64_t total = 0, free_b = 0;
    return esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_b) == ESP_OK ? free_b : 0;
}

static esp_err_t ensure_directories(void)
{
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    static const char *const dirs[] = {
        SD_OBD_ROOT, SD_DIR_IMPORT, SD_DIR_THEMES, SD_DIR_VIDEO,
        SD_DIR_BOOT, SD_DIR_CFG, SD_DIR_CATALOG, SD_DIR_EXPORT,
    };
    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (sd_card_mkdir(dirs[i]) != ESP_OK) {
            ESP_LOGW(TAG, "cannot create %s", dirs[i]);
            ret = ESP_FAIL;
        }
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Bounce buffer                                                       */
/* ------------------------------------------------------------------ */

uint8_t *sd_card_io_buf_take(TickType_t wait)
{
    if (!s_io_buf || !s_io_mutex) return NULL;
    return xSemaphoreTake(s_io_mutex, wait) == pdTRUE ? s_io_buf : NULL;
}

void sd_card_io_buf_give(void)
{
    if (s_io_mutex && xSemaphoreGetMutexHolder(s_io_mutex) == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(s_io_mutex);
    }
}

/* Direct transfer is fine for internal, word-aligned memory. If this task
 * already holds the bounce buffer (e.g. it passes the buffer itself, or nests),
 * go direct too: the driver then bounces per sector, slow but deadlock-free. */
static bool use_direct(const void *p)
{
    if (!s_io_buf) return true;
    if (esp_ptr_dma_capable(p) && (((uintptr_t)p & 3u) == 0)) return true;
    return xSemaphoreGetMutexHolder(s_io_mutex) == xTaskGetCurrentTaskHandle();
}

#if CONFIG_OBD_PERF_STATS
static size_t sd_card_read_impl(FILE *f, void *dst, size_t len);

size_t sd_card_read(FILE *f, void *dst, size_t len)
{
    PERF_BEGIN(perf_io_start);
    size_t transferred = sd_card_read_impl(f, dst, len);
    PERF_END(PERF_SD_READ, perf_io_start, (uint32_t)transferred);
    return transferred;
}

static size_t sd_card_read_impl(FILE *f, void *dst, size_t len)
#else
size_t sd_card_read(FILE *f, void *dst, size_t len)
#endif
{
    if (!f || !dst || len == 0) return 0;
    if (use_direct(dst)) return fread(dst, 1, len, f);

    uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
    if (!bb) return fread(dst, 1, len, f);
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > SD_IO_BUF_SIZE) n = SD_IO_BUF_SIZE;
        size_t got = fread(bb, 1, n, f);
        memcpy((uint8_t *)dst + done, bb, got);
        done += got;
        if (got < n) break;
    }
    sd_card_io_buf_give();
    return done;
}

#if CONFIG_OBD_PERF_STATS
static size_t sd_card_write_impl(FILE *f, const void *src, size_t len);

size_t sd_card_write(FILE *f, const void *src, size_t len)
{
    PERF_BEGIN(perf_io_start);
    size_t transferred = sd_card_write_impl(f, src, len);
    PERF_END(PERF_SD_WRITE, perf_io_start, (uint32_t)transferred);
    return transferred;
}

static size_t sd_card_write_impl(FILE *f, const void *src, size_t len)
#else
size_t sd_card_write(FILE *f, const void *src, size_t len)
#endif
{
    if (!f || !src || len == 0) return 0;
    if (use_direct(src)) return fwrite(src, 1, len, f);

    uint8_t *bb = sd_card_io_buf_take(portMAX_DELAY);
    if (!bb) return fwrite(src, 1, len, f);
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > SD_IO_BUF_SIZE) n = SD_IO_BUF_SIZE;
        memcpy(bb, (const uint8_t *)src + done, n);
        size_t put = fwrite(bb, 1, n, f);
        done += put;
        if (put < n) break;
    }
    sd_card_io_buf_give();
    return done;
}

/* ------------------------------------------------------------------ */
/* Whole-file helpers                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t write_whole(const char *path, const void *data, size_t len, bool sync)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open %s: errno %d", path, errno);
        return ESP_FAIL;
    }
    setvbuf(f, NULL, _IONBF, 0);    /* large transfers go straight to FATFS */
    size_t put = len ? sd_card_write(f, data, len) : 0;
    bool ok = (put == len);
    if (ok && sync) {
        ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        ESP_LOGW(TAG, "write %s failed (%u/%u)", path, (unsigned)put, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    return write_whole(path, data, len, false);
}

esp_err_t sd_card_write_file_atomic(const char *path, const void *data, size_t len)
{
    if (!path || (!data && len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    char part[SD_PATH_MAX];
    if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = write_whole(part, data, len, true);
    if (err != ESP_OK) {
        unlink(part);
        return err;
    }
    return sd_card_rename(part, path);
}

esp_err_t sd_card_read_file(const char *path, void *buf, size_t buf_len, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!path || (!buf && buf_len)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if ((size_t)st.st_size > buf_len) return ESP_ERR_INVALID_SIZE;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    setvbuf(f, NULL, _IONBF, 0);
    size_t want = (size_t)st.st_size;
    size_t got = want ? sd_card_read(f, buf, want) : 0;
    fclose(f);
    if (got != want) return ESP_FAIL;
    if (out_len) *out_len = got;
    return ESP_OK;
}

esp_err_t sd_card_read_file_alloc(const char *path, size_t max_len, void **out, size_t *out_len)
{
    if (!path || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    size_t sz = (size_t)st.st_size;
    if (sz > max_len) return ESP_ERR_INVALID_SIZE;

    /* +1 so text callers can NUL-terminate in place. */
    uint8_t *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc(sz + 1, MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t got = 0;
    esp_err_t err = sd_card_read_file(path, buf, sz, &got);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return err;
    }
    buf[got] = 0;
    *out = buf;
    if (out_len) *out_len = got;
    return ESP_OK;
}

esp_err_t sd_card_remove(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (unlink(path) == 0 || errno == ENOENT) return ESP_OK;
    return ESP_FAIL;
}

/* FAT rename refuses an existing destination: drop it first. The window
 * between unlink and rename is a few ms; .part is complete by then. */
esp_err_t sd_card_rename(const char *src, const char *dst)
{
    if (!src || !dst) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    struct stat st;
    /* Never remove the installed file before proving the source exists. */
    if (stat(src, &st) != 0) return ESP_FAIL;
    if (S_ISDIR(st.st_mode)) return ESP_ERR_INVALID_ARG;
    if (strcmp(src, dst) == 0) return ESP_OK;

    if (stat(dst, &st) != 0) {
        if (errno != ENOENT) return ESP_FAIL;
        if (rename(src, dst) == 0) return ESP_OK;
        ESP_LOGW(TAG, "rename %s -> %s: errno %d", src, dst, errno);
        return ESP_FAIL;
    }
    if (S_ISDIR(st.st_mode)) return ESP_ERR_INVALID_ARG;

    /* FatFs cannot rename over an existing entry. Retain a recoverable copy
     * until replacement succeeds; never overwrite an earlier recovery file.
     * Callers serialize this sequence with the storage job lock. This protects
     * runtime failures, not power-loss atomicity of FAT directory writes. */
    char backup[SD_PATH_MAX];
    int n = snprintf(backup, sizeof(backup), "%s.obd-bak", dst);
    if (n < 0 || n >= (int)sizeof(backup)) return ESP_ERR_INVALID_SIZE;
    if (stat(backup, &st) == 0) return ESP_ERR_INVALID_STATE;
    if (errno != ENOENT) return ESP_FAIL;
    if (rename(dst, backup) != 0) {
        ESP_LOGW(TAG, "backup %s -> %s: errno %d", dst, backup, errno);
        return ESP_FAIL;
    }
    if (rename(src, dst) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s: errno %d", src, dst, errno);
        if (rename(backup, dst) != 0) {
            ESP_LOGE(TAG, "restore %s -> %s failed: errno %d; backup retained", backup, dst, errno);
        }
        return ESP_FAIL;
    }
    if (unlink(backup) != 0) {
        ESP_LOGW(TAG, "replacement committed; backup retained at %s: errno %d", backup, errno);
    }
    return ESP_OK;
}

esp_err_t sd_card_mkdir(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return ESP_OK;
    return ESP_FAIL;
}

bool sd_card_exists(const char *path)
{
    if (!path || !sd_card_is_mounted()) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t sd_card_append_line(const char *path, const char *line)
{
    if (!path || !line) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(path, "ab");
    if (!f) return ESP_FAIL;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    bool ok = fwrite(line, 1, n, f) == n && fwrite("\r\n", 1, 2, f) == 2;
    if (fclose(f) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

#else /* !CONFIG_OBD_SD_SLOT_SDMMC_1BIT: board without an SD slot */

esp_err_t   sd_card_init(void)               { ESP_LOGI(TAG, "no SD slot on this board"); return ESP_ERR_NOT_SUPPORTED; }
void        sd_card_poll(void)               { }
uint32_t    sd_card_poll_wait_ms(void)        { return 1000; }
esp_err_t   sd_card_eject(void)              { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_format(char *detail, size_t capacity)
{ if (detail && capacity) snprintf(detail, capacity, "SD slot disabled"); return ESP_ERR_NOT_SUPPORTED; }
sd_state_t  sd_card_get_state(void)          { return SD_STATE_DISABLED; }
bool        sd_card_is_mounted(void)         { return false; }
uint32_t    sd_card_get_freq_khz(void)       { return 0; }
uint64_t    sd_card_get_total_bytes(void)    { return 0; }
uint64_t    sd_card_get_free_bytes(void)     { return 0; }
uint8_t    *sd_card_io_buf_take(TickType_t wait) { (void)wait; return NULL; }
void        sd_card_io_buf_give(void)        { }
size_t sd_card_read(FILE *f, void *dst, size_t len)        { (void)f; (void)dst; (void)len; return 0; }
size_t sd_card_write(FILE *f, const void *src, size_t len) { (void)f; (void)src; (void)len; return 0; }
esp_err_t sd_card_write_file(const char *path, const void *data, size_t len)
{ (void)path; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_write_file_atomic(const char *path, const void *data, size_t len)
{ (void)path; (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_read_file(const char *path, void *buf, size_t buf_len, size_t *out_len)
{ (void)path; (void)buf; (void)buf_len; if (out_len) *out_len = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_read_file_alloc(const char *path, size_t max_len, void **out, size_t *out_len)
{ (void)path; (void)max_len; if (out) *out = NULL; if (out_len) *out_len = 0; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_remove(const char *path)                { (void)path; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_rename(const char *src, const char *dst) { (void)src; (void)dst; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_card_mkdir(const char *path)                 { (void)path; return ESP_ERR_NOT_SUPPORTED; }
bool      sd_card_exists(const char *path)                { (void)path; return false; }
esp_err_t sd_card_append_line(const char *path, const char *line)
{ (void)path; (void)line; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OBD_SD_SLOT_SDMMC_1BIT */
