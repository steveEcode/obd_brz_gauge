/* Execute the real SD state machine with virtual RTOS ticks and SDK boundaries.
 * No SD files are created; this proves scheduling policy, not SDMMC hardware. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "bsp_obd_dsp/sd_card.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "bsp_obd_dsp/sd_io_guard.h"
#include "ff.h"
#include "diskio_impl.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
struct sd_test_mutex { bool held; };
static struct sd_test_mutex mutexes[2];
static unsigned mutex_count, lock_count, mount_count, bare_probe_count, directory_count;
static unsigned static_mutex_attempts, fail_static_mutex_attempt;
static TickType_t ticks = UINT32_MAX - 100u, probe_cost_ticks;
static esp_err_t mount_result = ESP_ERR_TIMEOUT, status_result = ESP_OK;
static bool card_present = true;
static sdmmc_card_t card = { .real_freq_khz = 40000, .cid = { "test" }, .csd = { 1024, 512 } };

void sd_io_guard_init(void) {}
void sd_io_trace_begin(sd_io_trace_t *t) { memset(t, 0, sizeof(*t)); }
void sd_io_trace_end(sd_io_trace_t *t) { (void)t; }
void sd_io_trace_format(const sd_io_trace_t *t, char *out, size_t cap)
{ (void)t; if (out && cap) out[0] = '\0'; }
PARTITION VolToPart[2] = {{0, 0}, {1, 0}};
static unsigned formats, raw_registers, raw_unregisters;
static FRESULT format_result = FR_OK;
static MKFS_PARM format_options;
esp_err_t ff_diskio_get_drive(BYTE *drive) { *drive = 1; return ESP_OK; }
void ff_diskio_register_sdmmc(BYTE drive, sdmmc_card_t *c)
{ CHECK(drive == 1 && c); raw_registers++; }
void ff_diskio_register(BYTE drive, const void *impl)
{ CHECK(drive == 1 && !impl); raw_unregisters++; }
FRESULT f_mkfs(const char *path, const MKFS_PARM *opt, void *work, unsigned len)
{ CHECK(!strcmp(path, "1:") && work && len == 4096); formats++; format_options = *opt; return format_result; }

const char *esp_err_to_name(esp_err_t err) { (void)err; return "test"; }
void storage_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
esp_log_level_t esp_log_level_get(const char *tag) { (void)tag; return ESP_LOG_INFO; }
void esp_log_level_set(const char *tag, esp_log_level_t level) { (void)tag; (void)level; }
TickType_t xTaskGetTickCount(void) { return ticks; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)&ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    CHECK(mutex_count < 2);
    return &mutexes[mutex_count++];
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    CHECK(storage);
    if (++static_mutex_attempts == fail_static_mutex_attempt) return NULL;
    return xSemaphoreCreateMutex();
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    CHECK(mutex && !mutex->held && wait == portMAX_DELAY);
    mutex->held = true;
    lock_count++;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    CHECK(mutex && mutex->held);
    mutex->held = false;
    return pdTRUE;
}
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t mutex)
{
    return mutex && mutex->held ? xTaskGetCurrentTaskHandle() : NULL;
}
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
void heap_caps_free(void *ptr) { free(ptr); }
bool esp_ptr_dma_capable(const void *ptr) { (void)ptr; return true; }
esp_err_t esp_vfs_fat_sdmmc_mount(const char *path, const sdmmc_host_t *host,
    const sdmmc_slot_config_t *slot, const esp_vfs_fat_sdmmc_mount_config_t *config,
    sdmmc_card_t **out)
{
    CHECK(mutexes[0].held && !strcmp(path, SD_MOUNT_POINT));
    CHECK(!config->format_if_mount_failed && config->max_files == 6);
    CHECK(host->max_freq_khz == 40000 || host->max_freq_khz == 20000);
    CHECK(slot->width == 1 && slot->clk == 14 && slot->cmd == 17 && slot->d0 == 16);
    mount_count++;
    *out = mount_result == ESP_OK ? &card : NULL;
    return mount_result;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *mounted)
{
    CHECK(mutexes[0].held && mounted == &card && !strcmp(path, SD_MOUNT_POINT));
    return ESP_OK;
}
esp_err_t esp_vfs_fat_info(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    (void)path; *total = *free_bytes = 0; return ESP_OK;
}
esp_err_t sdmmc_get_status(sdmmc_card_t *mounted)
{
    CHECK(mutexes[0].held && mounted == &card);
    return status_result;
}
esp_err_t sdmmc_host_init(void) { CHECK(mutexes[0].held); return ESP_OK; }
esp_err_t sdmmc_host_init_slot(int slot, const sdmmc_slot_config_t *config)
{
    CHECK(slot == 1 && config->width == 1); return ESP_OK;
}
esp_err_t sdmmc_host_deinit(void) { return ESP_OK; }
esp_err_t sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *probe)
{
    CHECK(mutexes[0].held && host->max_freq_khz == 20000 && probe);
    bare_probe_count++;
    ticks += probe_cost_ticks;
    *probe = card;
    return card_present ? ESP_OK : ESP_ERR_TIMEOUT;
}
int sd_test_stat(const char *path, struct stat *info)
{
    CHECK(!strncmp(path, SD_OBD_ROOT, strlen(SD_OBD_ROOT)));
    memset(info, 0, sizeof(*info));
    info->st_mode = (mode_t)0040000; /* existing directory, no host file access */
    directory_count++;
    return 0;
}
int sd_test_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; CHECK(false); return -1; }

#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
static void advance(uint32_t ms) { CHECK(ms % portTICK_PERIOD_MS == 0); ticks += pdMS_TO_TICKS(ms); }
static void check_state(sd_state_t state, uint32_t wait_ms)
{
    CHECK(sd_card_get_state() == state);
    CHECK(sd_card_is_mounted() == (state == SD_STATE_MOUNTED));
    CHECK(sd_card_poll_wait_ms() == wait_ms);
    CHECK(!mutexes[0].held && !mutexes[1].held);
}
#endif

int main(void)
{
    CHECK(sd_card_poll_wait_ms() == 1000);
    sd_card_poll(); /* uninitialized is harmless */
#if CONFIG_OBD_SD_SLOT_SDMMC_1BIT
    fail_static_mutex_attempt = 2;
    CHECK(sd_card_init() == ESP_ERR_NO_MEM && mutex_count == 1 && mount_count == 0);
    fail_static_mutex_attempt = 0;
    CHECK(sd_card_init() == ESP_ERR_TIMEOUT);
    CHECK(mutex_count == 2 && static_mutex_attempts == 3);
    CHECK(mount_count == 2); /* boot's 40/20 MHz fallback */
    check_state(SD_STATE_ABSENT, 3000);
    advance(2000); /* crosses the unsigned RTOS tick rollover */
    CHECK(ticks < 200);
    sd_card_poll();
    CHECK(mount_count == 2);
    check_state(SD_STATE_ABSENT, 1000);
    advance(990); sd_card_poll();
    check_state(SD_STATE_ABSENT, 10);
    advance(10);
    CHECK(sd_card_poll_wait_ms() == 1); /* due deadlines never yield a zero delay */
    sd_card_poll();
    CHECK(mount_count == 3); /* quiet timeout doesn't redundantly try 20 MHz */
    check_state(SD_STATE_ABSENT, 3000);

    mount_result = ESP_FAIL;
    advance(3000); sd_card_poll();
    CHECK(mount_count == 4);
    check_state(SD_STATE_ERROR, 10000);
    advance(9990); sd_card_poll();
    CHECK(mount_count == 4);
    check_state(SD_STATE_ERROR, 10);
    advance(10); sd_card_poll();
    CHECK(mount_count == 5);
    check_state(SD_STATE_ERROR, 10000);
    mount_result = ESP_OK;
    advance(10000); sd_card_poll();
    CHECK(mount_count == 6 && directory_count == 8);
    check_state(SD_STATE_MOUNTED, 1000);
    CHECK(sd_card_init() == ESP_OK && mutex_count == 2 && mount_count == 6);
    unsigned before = lock_count;
    CHECK(sd_card_poll_wait_ms() == 1000 && lock_count == before); /* fast mounted hint */

    status_result = ESP_FAIL;
    advance(1000); sd_card_poll();
    check_state(SD_STATE_MOUNTED, 1000); /* first failure keeps the mount */
    advance(1000); sd_card_poll();
    check_state(SD_STATE_ABSENT, 3000); /* second failure resets probe after unmount */
    status_result = ESP_OK;
    advance(3000); sd_card_poll();
    CHECK(mount_count == 7);
    CHECK(sd_card_eject() == ESP_OK);
    check_state(SD_STATE_EJECTED, 3000);
    CHECK(sd_card_eject() == ESP_ERR_INVALID_STATE);

    advance(2990); sd_card_poll();
    CHECK(bare_probe_count == 0);
    check_state(SD_STATE_EJECTED, 10);
    probe_cost_ticks = pdMS_TO_TICKS(200);
    advance(10); sd_card_poll();
    CHECK(bare_probe_count == 1 && mount_count == 7);
    check_state(SD_STATE_EJECTED, 2800); /* remaining deadline includes probe time */
    card_present = false;
    probe_cost_ticks = 0;
    advance(2800); sd_card_poll();
    check_state(SD_STATE_EJECTED, 3000);
    advance(3000); sd_card_poll();
    CHECK(bare_probe_count == 3);
    check_state(SD_STATE_ABSENT, 3000); /* requires two absent probes after eject */
    advance(3000); sd_card_poll();
    CHECK(mount_count == 8);
    check_state(SD_STATE_MOUNTED, 1000);
    /* Explicit maintenance formatting never enables format-on-mount. */
    char detail[480];
    card_present = true;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_OK);
    CHECK(formats == 1 && raw_registers == 1 && raw_unregisters == 1);
    CHECK(format_options.fmt == FM_FAT && format_options.n_fat == 2 && !format_options.au_size);
    format_result = FR_DISK_ERR;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_FAIL);
    CHECK(strstr(detail, "create FAT filesystem") && strstr(detail, "fatfs=1"));
    CHECK(raw_registers == raw_unregisters && !sd_card_is_mounted());
    format_result = FR_OK;
    card_present = false;
    unsigned previous_formats = formats;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_ERR_TIMEOUT);
    CHECK(formats == previous_formats); /* no card: never attempt destructive IO */
    card_present = true;
    /* Disk init copies capacity, allowing both cluster-size paths. */
    card.csd.capacity = 16u * 1024u * 1024u;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_OK);
    CHECK(format_options.fmt == FM_FAT32 && format_options.au_size == 32768);
    CHECK(format_options.align == 2048 && raw_registers == raw_unregisters);
    card.csd.capacity = 512u * 1024u;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_OK);
    CHECK(format_options.fmt == FM_FAT32 && !format_options.au_size);
    VolToPart[1].pt = 1;
    previous_formats = formats;
    CHECK(sd_card_format(detail, sizeof(detail)) == ESP_ERR_INVALID_STATE);
    CHECK(formats == previous_formats && raw_registers == raw_unregisters);
    puts("SD state deadlines passed (rollover, absent/error/eject/reinsert)");
#else
    (void)formats; (void)format_result; (void)format_options;
    CHECK(sd_card_format(NULL, 0) == ESP_ERR_NOT_SUPPORTED);
    CHECK(sd_card_init() == ESP_ERR_NOT_SUPPORTED);
    CHECK(sd_card_get_state() == SD_STATE_DISABLED);
    CHECK(!sd_card_is_mounted() && !mutex_count && !mount_count);
    CHECK(sd_card_poll_wait_ms() == 1000);
    puts("No-slot SD boundaries passed");
#endif
    return 0;
}
