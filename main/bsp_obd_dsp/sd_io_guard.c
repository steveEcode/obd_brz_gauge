/* Link-time adapters for the IDF sector APIs, including FatFs cache/metadata
 * accesses. A byte-level bounce buffer alone does not keep disk_read buffers
 * aligned after FatFs has copied the leading partial sector. */
#include "sd_io_guard.h"
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define SECTOR_BOUNCE_SIZE 4096
static uint8_t *s_sector_buf;
static SemaphoreHandle_t s_sector_lock;
static StaticSemaphore_t s_sector_lock_storage;
static portMUX_TYPE s_trace_mux = portMUX_INITIALIZER_UNLOCKED;
static sd_io_trace_t *s_traces;

/* Called once at boot, before mounting any card. Never allocate on the IO path. */
void sd_io_guard_init(void)
{
    if (!s_sector_lock) s_sector_lock = xSemaphoreCreateMutexStatic(&s_sector_lock_storage);
    if (!s_sector_buf) s_sector_buf = heap_caps_aligned_alloc(16, SECTOR_BOUNCE_SIZE,
                                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

void sd_io_trace_begin(sd_io_trace_t *t)
{
    memset(t, 0, sizeof(*t));
    t->owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_trace_mux);
    t->next = s_traces;
    s_traces = t;
    portEXIT_CRITICAL(&s_trace_mux);
}

void sd_io_trace_end(sd_io_trace_t *t)
{
    portENTER_CRITICAL(&s_trace_mux);
    sd_io_trace_t **link = &s_traces;
    while (*link && *link != t) link = &(*link)->next;
    if (*link) *link = t->next;
    t->next = NULL;
    portEXIT_CRITICAL(&s_trace_mux);
}

int sd_io_trace_current(sd_io_trace_t *out)
{
    int found = 0;
    void *owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_trace_mux);
    for (sd_io_trace_t *t = s_traces; t; t = t->next) {
        if (t->owner == owner && t->error != ESP_OK) { *out = *t; found = 1; break; }
    }
    portEXIT_CRITICAL(&s_trace_mux);
    return found;
}

static void record_error(sdmmc_card_t *card, size_t sector, size_t count, char op, esp_err_t err)
{
    if (err == ESP_OK) return;
    const size_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t available = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    size_t dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_trace_mux);
    for (sd_io_trace_t *t = s_traces; t; t = t->next) {
        if (t->owner != owner || t->error != ESP_OK) continue;
        t->error = err;
        t->sector = sector;
        t->count = count;
        t->khz = card->real_freq_khz;
        t->operation = op;
        t->internal_free = available;
        t->internal_largest = largest;
        t->dma_largest = dma;
    }
    portEXIT_CRITICAL(&s_trace_mux);
}

void sd_io_trace_format(const sd_io_trace_t *t, char *out, size_t capacity)
{
    if (!out || !capacity) return;
    out[0] = '\0';
    if (t->error == ESP_OK) return;
    snprintf(out, capacity, " sd=%s(0x%x) op=%c lba=%lu sectors=%lu khz=%lu internal=%lu/%lu dma=%lu",
             esp_err_to_name(t->error), (unsigned)t->error, t->operation,
             (unsigned long)t->sector, (unsigned long)t->count, (unsigned long)t->khz,
             (unsigned long)t->internal_free, (unsigned long)t->internal_largest,
             (unsigned long)t->dma_largest);
}

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t);

static esp_err_t sector_io(sdmmc_card_t *card, void *buffer, size_t sector, size_t count, bool write)
{
    if (count == 0) return ESP_OK;
    size_t block = card->csd.sector_size;
    /* Preserve SDK bounds checks and zero-count semantics before pointer math. */
    if (sector > card->csd.capacity || count > card->csd.capacity - sector ||
        !block || count > SIZE_MAX / block) {
        esp_err_t err = ESP_ERR_INVALID_SIZE;
        record_error(card, sector, count, write ? 'W' : 'R', err);
        return err;
    }
    bool direct = card->host.check_buffer_alignment(card->host.slot, buffer, count * block) &&
                  esp_ptr_dma_capable(buffer) && !esp_ptr_external_ram(buffer);
    esp_err_t err;
    size_t done = 0;
    if (direct || !s_sector_buf || !s_sector_lock || block > SECTOR_BOUNCE_SIZE) {
        /* Safe SDK fallback if the boot-time reservation failed. Report its
         * real ESP_ERR_NO_MEM rather than pretending the card is damaged. */
        err = write ? __real_sdmmc_write_sectors(card, buffer, sector, count) :
                      __real_sdmmc_read_sectors(card, buffer, sector, count);
    } else {
        xSemaphoreTake(s_sector_lock, portMAX_DELAY);
        err = ESP_OK;
        while (done < count) {
            size_t n = count - done;
            if (n > SECTOR_BOUNCE_SIZE / block) n = SECTOR_BOUNCE_SIZE / block;
            uint8_t *p = (uint8_t *)buffer + done * block;
            if (write) memcpy(s_sector_buf, p, n * block);
            err = write ? __real_sdmmc_write_sectors(card, s_sector_buf, sector + done, n) :
                          __real_sdmmc_read_sectors(card, s_sector_buf, sector + done, n);
            if (err != ESP_OK) {
                record_error(card, sector + done, n, write ? 'W' : 'R', err);
                break;
            }
            if (!write) memcpy(p, s_sector_buf, n * block);
            done += n;
        }
        xSemaphoreGive(s_sector_lock);
    }
    record_error(card, sector, count, write ? 'W' : 'R', err);
    return err;
}

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *buffer, size_t sector, size_t count)
{
    return sector_io(card, buffer, sector, count, false);
}
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *buffer, size_t sector, size_t count)
{
    return sector_io(card, (void *)buffer, sector, count, true);
}
