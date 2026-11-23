/* Real wrapper, fake SDK boundary. No actual SD or per-IO heap allocations. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp_obd_dsp/sd_io_guard.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *, void *, size_t, size_t);
esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *, const void *, size_t, size_t);
struct sd_test_mutex { bool held; };
static struct sd_test_mutex mutex;
static unsigned allocations, calls, fail_call;
static bool fail_alloc = true;
static esp_err_t sdk_error = ESP_ERR_TIMEOUT;
static void *external;
static void *task = (void *)1;
static unsigned char disk[32 * 4096];
static size_t last_sector, last_count;

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ assert(storage); return &mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait)
{ assert(m == &mutex && !m->held && wait == portMAX_DELAY); m->held = true; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{ assert(m == &mutex && m->held); m->held = false; return pdTRUE; }
void *heap_caps_aligned_alloc(size_t alignment, size_t n, uint32_t caps)
{ assert(alignment == 16 && n == 4096 && caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  allocations++; return fail_alloc ? NULL : aligned_alloc(alignment, n); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 16000; }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{ return caps & MALLOC_CAP_8BIT ? 8000 : 4000; }
bool esp_ptr_dma_capable(const void *p) { return (uintptr_t)p % 16 == 0; }
bool esp_ptr_external_ram(const void *p) { return p == external; }
static bool aligned(int slot, const void *p, size_t n)
{ assert(slot == 1 && n); return (uintptr_t)p % 16 == 0; }
const char *esp_err_to_name(esp_err_t err)
{ return err == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : "ESP_ERR_NO_MEM"; }
static esp_err_t sdk_io(sdmmc_card_t *card, void *p, size_t sector, size_t count, bool write)
{
    last_sector = sector; last_count = count; calls++;
    if (calls == fail_call) return sdk_error;
    assert(aligned(1, p, count * card->csd.sector_size) && !esp_ptr_external_ram(p));
    assert(sector + count <= 32);
    size_t offset = sector * card->csd.sector_size, n = count * card->csd.sector_size;
    if (write) memcpy(disk + offset, p, n); else memcpy(p, disk + offset, n);
    return ESP_OK;
}
esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *c, void *p, size_t s, size_t n)
{ return sdk_io(c, p, s, n, false); }
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *c, const void *p, size_t s, size_t n)
{ return sdk_io(c, (void *)p, s, n, true); }

int main(void)
{
    sdmmc_card_t card = {.host = {.slot = 1, .check_buffer_alignment = aligned},
        .csd = {.capacity = 32, .sector_size = 512}, .real_freq_khz = 20000};
    _Alignas(16) unsigned char buffer[32 * 4096 + 16];
    sd_io_trace_t trace, inner, other, copy;
    sd_io_guard_init(); // Reservation failed: SDK fallback reports its actual allocation error.
    assert(allocations == 1);
    sd_io_trace_begin(&trace); fail_call = 1; sdk_error = ESP_ERR_NO_MEM;
    assert(__wrap_sdmmc_read_sectors(&card, buffer + 1, 3, 1) == ESP_ERR_NO_MEM);
    assert(trace.error == ESP_ERR_NO_MEM && trace.sector == 3);
    sd_io_trace_end(&trace);
    fail_alloc = false; sd_io_guard_init(); sd_io_guard_init();
    assert(allocations == 2); fail_call = 0;
    for (size_t i = 0; i < sizeof(disk); i++) disk[i] = (unsigned char)(i * 37 + i / 512);
    for (size_t sector_size = 512; sector_size <= 4096; sector_size *= 8) {
        card.csd.sector_size = sector_size;
        calls = 0;
        // Leading partial FatFs sector leaves the next DMA destination misaligned.
        unsigned char *p = buffer + 499;
        assert(__wrap_sdmmc_read_sectors(&card, p, 2, 20) == ESP_OK);
        assert(!memcmp(p, disk + 2 * sector_size, 20 * sector_size));
        assert(calls == (20 * sector_size + 4095) / 4096);
        memset(p, 0xA5, 20 * sector_size);
        assert(__wrap_sdmmc_write_sectors(&card, p, 2, 20) == ESP_OK);
        assert(!memcmp(p, disk + 2 * sector_size, 20 * sector_size));
    }
    card.csd.sector_size = 512;
    external = buffer; calls = 0;
    assert(__wrap_sdmmc_read_sectors(&card, buffer, 0, 20) == ESP_OK && calls == 3);
    external = NULL; calls = 0;
    assert(__wrap_sdmmc_read_sectors(&card, buffer, 0, 20) == ESP_OK && calls == 1);
    assert(__wrap_sdmmc_read_sectors(&card, buffer, SIZE_MAX, 0) == ESP_OK && calls == 1);
    assert(__wrap_sdmmc_read_sectors(&card, buffer, 31, 2) == ESP_ERR_INVALID_SIZE && calls == 1);
    sd_io_trace_begin(&trace);
    task = (void *)2; sd_io_trace_begin(&other); task = (void *)1;
    sd_io_trace_begin(&inner); calls = 0; fail_call = 2; sdk_error = ESP_ERR_TIMEOUT;
    assert(__wrap_sdmmc_read_sectors(&card, buffer + 1, 3, 20) == ESP_ERR_TIMEOUT);
    assert(last_sector == 11 && last_count == 8);
    assert(trace.error == ESP_ERR_TIMEOUT && trace.sector == 11 && trace.count == 8);
    assert(inner.error == ESP_ERR_TIMEOUT && other.error == ESP_OK);
    assert(sd_io_trace_current(&copy) && copy.sector == 11);
    fail_call = ++calls + 1; sdk_error = ESP_ERR_NO_MEM;
    assert(__wrap_sdmmc_write_sectors(&card, buffer, 0, 1) == ESP_ERR_NO_MEM);
    assert(trace.error == ESP_ERR_TIMEOUT && trace.operation == 'R'); // first failure wins
    char detail[256], tiny[4] = {1,2,3,4};
    sd_io_trace_format(&trace, detail, sizeof(detail));
    assert(strstr(detail, "sd=ESP_ERR_TIMEOUT(0x107) op=R lba=11 sectors=8 khz=20000"));
    assert(strstr(detail, "internal=16000/8000 dma=4000"));
    sd_io_trace_format(&trace, tiny, 3); assert(tiny[2] == 0 && tiny[3] == 4);
    sd_io_trace_end(&inner); sd_io_trace_end(&trace);
    assert(!sd_io_trace_current(&copy));
    task = (void *)2; sd_io_trace_end(&other);
    assert(!sd_io_trace_current(&copy));
    assert(allocations == 2 && !mutex.held);
    return 0;
}
