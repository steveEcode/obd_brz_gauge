#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int serial_test_fflush(FILE *stream);
static int serial_test_fsync(int fd);
#define fflush serial_test_fflush
#define fsync serial_test_fsync
#include "app_obd_dsp/bootmedia_serial.c"
#undef fflush
#undef fsync

static bool locked, busy, ota, fail_open, fail_verify, fail_commit, fail_install, fail_obv, fail_write;
static bool release_busy_on_wait;
static TickType_t last_lock_wait;
static unsigned installs, removes, validations, commits;
static bool fail_flush, fail_sync, file_synced;
static unsigned flushes, syncs;

static int serial_test_fflush(FILE *stream)
{
    assert(locked && stream == s_sink.fp);
    flushes++;
    if (fail_flush) { errno = EIO; return EOF; }
    return fflush(stream);
}
static int serial_test_fsync(int fd)
{
    assert(locked && s_sink.fp && fd == fileno(s_sink.fp) && flushes == 1);
    syncs++;
    if (fail_sync) { errno = EIO; return -1; }
    int result = fsync(fd);
    file_synced = result == 0;
    return result;
}
static const char *const boot_paths[] = {SD_BOOT_OBV, SD_BOOT_JPEG, SD_BOOT_LEGACY};
static bool boot_files[3], committed;
static const char *fail_remove_path;
static int boot_path_index(const char *path)
{
    for (int i = 0; i < 3; i++) if (!strcmp(path, boot_paths[i])) return i;
    return -1;
}
bool boot_obv_validate_file(const char *path, const char **reason)
{ assert(locked); assert(!strcmp(path, SD_BOOT_OBV ".part"));
  /* FatFs readers see the directory size only after writer synchronization. */
  assert(file_synced && flushes == 1 && syncs == 1);
  validations++;
  if (reason) *reason = fail_obv ? "Invalid JPEG test frame" : NULL;
  return !fail_obv; }
static TickType_t ticks;
static int input_mode;
static uint8_t output[32768];
static size_t output_len;
bool storage_job_lock(TickType_t wait)
{
    last_lock_wait = wait;
    if (wait && release_busy_on_wait) busy = false;
    if (busy || locked) return false;
    locked = true;
    return true;
}
void storage_job_unlock(void) { assert(locked); locked = false; }
bool ota_wifi_server_is_active(void) { return ota; }
bool sd_card_is_mounted(void) { return true; }
uint64_t sd_card_get_free_bytes(void) { return 1000000000; }
esp_err_t sd_card_remove(const char *path)
{
    assert(locked); removes++;
    int index = boot_path_index(path);
    if (index >= 0) {
        assert(committed); /* Never clear old formats before validation and commit. */
        assert(strcmp(path, s_sink.path)); /* Never remove the just-installed format. */
        if (fail_remove_path && !strcmp(path, fail_remove_path)) return ESP_FAIL;
        boot_files[index] = false;
    }
    return ESP_OK;
}
bool upload_sink_is_open(const upload_sink_t *s) { return s->kind != UPLOAD_SINK_NONE; }
esp_err_t upload_sink_open(upload_sink_t *s, upload_sink_kind_t kind, uint32_t expected,
                           const uint8_t *sha, const char *path, uint32_t manifest)
{
    (void)sha; (void)manifest;
    assert(kind == UPLOAD_SINK_MEMORY ? !locked : locked); if (fail_open) return ESP_FAIL;
    s->kind = kind; s->expected = expected;
    if (path) { snprintf(s->path, sizeof(s->path), "%s", path); s->fp = tmpfile(); assert(s->fp); }
    if (kind == UPLOAD_SINK_MEMORY) s->buf = calloc(expected + 1, 1);
    return ESP_OK;
}
esp_err_t upload_sink_write(upload_sink_t *s, const void *data, size_t len)
{ assert(s->kind == UPLOAD_SINK_MEMORY ? !locked : locked);
  if (fail_write) { s->failed = true; return ESP_FAIL; }
  if (s->kind == UPLOAD_SINK_MEMORY) memcpy(s->buf + s->received, data, len);
  else { assert(fwrite(data, 1, len, s->fp) == len); file_synced = false; }
  s->received += (uint32_t)len; return ESP_OK; }
esp_err_t upload_sink_verify(upload_sink_t *s)
{ assert(s->kind == UPLOAD_SINK_MEMORY ? !locked : locked); assert(s->received == s->expected); return fail_verify ? ESP_FAIL : ESP_OK; }
esp_err_t upload_sink_commit(upload_sink_t *s)
{
    assert(s->kind == UPLOAD_SINK_MEMORY ? !locked : locked); commits++;
    if (fail_commit) return ESP_FAIL;
    committed = true;
    int index = boot_path_index(s->path);
    if (index >= 0) boot_files[index] = true;
    return ESP_OK;
}
void upload_sink_abort(upload_sink_t *s)
{ if (s->kind == UPLOAD_SINK_SD_FILE && s->received < s->expected) assert(locked);
  if (s->fp) fclose(s->fp);
  free(s->buf); memset(s, 0, sizeof(*s)); }
esp_err_t theme_bundle_install(const char *path)
{ (void)path; assert(!locked); installs++; return fail_install ? ESP_FAIL : ESP_OK; }
TickType_t xTaskGetTickCount(void) { return ticks; }
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
int usb_serial_jtag_read_bytes(void *dst, uint32_t len, TickType_t timeout)
{
    if (input_mode && len == 1) { *(uint8_t *)dst = (uint8_t)"OBDS"[input_mode++ - 1]; return 1; }
    ticks += timeout; return 0;
}
int usb_serial_jtag_write_bytes(const void *src, size_t len, TickType_t timeout)
{ (void)timeout; assert(output_len + len <= sizeof(output));
  memcpy(output + output_len, src, len); output_len += len; return (int)len; }
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t timeout) { (void)timeout; return ESP_OK; }
bool usb_serial_jtag_is_driver_installed(void) { return true; }
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{ (void)config; return ESP_OK; }
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
void storage_test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t delay) { ticks += delay; }
void esp_restart(void) { abort(); }
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                      UBaseType_t prio, TaskHandle_t *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; return pdPASS; }

static unsigned rpc_calls;
char *device_serial_rpc(const char *json, bool *restart)
{
    assert(!locked); assert(!strcmp(json, "12345678")); rpc_calls++;
    *restart = false;
    char *reply = malloc(12); memcpy(reply, "{\"ok\":true}", 12); return reply;
}
static uint8_t begin(bool theme)
{
    uint8_t payload[40] = {0}; put_u32le(payload, 8); put_u32le(payload + 4, theme ? 0 : 1);
    return process_frame(theme ? CMD_BEGIN_THEME : CMD_BEGIN, payload, sizeof(payload));
}
static void data(void)
{
    uint8_t payload[12] = {0};
    assert(process_frame(CMD_DATA, payload, sizeof(payload)) == 0);
    /* A background cleaner must be unable to enter throughout the transfer. */
    assert(!storage_job_lock(0));
}
static void reset(void)
{
    abort_session(); assert(!locked); assert(!s_storage_locked);
    busy = ota = fail_open = fail_verify = fail_commit = fail_install = fail_obv = fail_write = false;
    release_busy_on_wait = false; last_lock_wait = 0;
    installs = removes = validations = commits = 0; ticks = 0; input_mode = 0; output_len = 0;
    committed = false; fail_remove_path = NULL;
    fail_flush = fail_sync = file_synced = false; flushes = syncs = 0;
    for (int i = 0; i < 3; i++) boot_files[i] = true;
}
int main(void)
{
    for (int theme = 0; theme < 2; theme++) {
        reset(); assert(begin(theme) == 0); assert(locked); data();
        assert(process_frame(CMD_END, NULL, 0) == 0); assert(!locked);
        assert(installs == (unsigned)theme); assert(s_sink.expected == 8);
        assert(removes == (theme ? 0u : 2u));
        assert(boot_files[0] == (bool)theme && boot_files[1] == (bool)theme && boot_files[2]);
        reset(); assert(begin(theme) == 0);
        assert(process_frame(CMD_ABORT, NULL, 0) == 0); assert(!locked); assert(removes == (unsigned)theme);
        reset(); fail_open = true; assert(begin(theme) == 3); assert(!locked);
        reset(); assert(begin(theme) == 0); fail_write = true;
        uint8_t failed_chunk[12] = {0};
        assert(process_frame(CMD_DATA, failed_chunk, sizeof(failed_chunk)) == 5);
        assert(!locked && !upload_sink_is_open(&s_sink));
        assert(boot_files[0] && boot_files[1] && boot_files[2]);
        /* Theme reads / boot jobs may finish while BEGIN waits for storage. */
        reset(); busy = release_busy_on_wait = true;
        assert(begin(theme) == 0); assert(locked);
        assert(last_lock_wait == pdMS_TO_TICKS(1000));
        assert(process_frame(CMD_ABORT, NULL, 0) == 0); assert(!locked);
        reset(); busy = true; assert(begin(theme) == 9); assert(!locked);
        assert(last_lock_wait == pdMS_TO_TICKS(1000));
        reset(); ota = true; assert(begin(theme) == 9); assert(!locked);
        for (int commit = 0; commit < 2; commit++) {
            reset(); assert(begin(theme) == 0); data();
            fail_verify = !commit; fail_commit = commit;
            assert(process_frame(CMD_END, NULL, 0) == 6); assert(!locked);
            assert(boot_files[0] && boot_files[1] && boot_files[2]);
            assert(removes == (unsigned)theme);
        }
        reset(); assert(begin(theme) == 0);
        uint8_t header[FRAME_HEADER_SIZE];
        assert(!find_frame_header(header)); assert(!locked); /* idle timeout */
        reset(); assert(begin(theme) == 0); input_mode = 1;
        assert(!find_frame_header(header)); /* truncated header: task cleanup */
        abort_session(); assert(!locked);
    }
    reset(); fail_install = true; assert(begin(true) == 0); data();
    assert(process_frame(CMD_END, NULL, 0) == 6); assert(!locked);
    uint8_t obv[40] = {0}; put_u32le(obv, 8);
    reset(); put_u32le(obv + 4, 1); assert(process_frame(CMD_BEGIN_OBV, obv, 40) == 2);
    assert(!locked); put_u32le(obv + 4, 0);
    for (int invalid = 0; invalid < 2; invalid++) {
        reset(); assert(process_frame(CMD_BEGIN_OBV, obv, 40) == 0); data();
        fail_obv = invalid;
        assert(process_frame(CMD_END, NULL, 0) == (invalid ? 10 : 0));
        assert(validations == 1 && commits == (unsigned)!invalid && removes == (invalid ? 0u : 2u) && !locked);
        assert(boot_files[0] && boot_files[1] == (bool)invalid && boot_files[2] == (bool)invalid);
        if (invalid) {
            send_reply(CMD_END, 12, 10, 0);
            assert(output[16] == 10 && !memcmp(output + 21, "Invalid JPEG test frame", 23));
        }
    }
    /* Both failures stop before validation/commit and preserve old animations. */
    for (unsigned failure = 0; failure < 2; failure++) {
        reset(); assert(process_frame(CMD_BEGIN_OBV, obv, 40) == 0); data();
        fail_flush = failure == 0; fail_sync = failure == 1;
        assert(process_frame(CMD_END, NULL, 0) == 6);
        assert(flushes == 1 && syncs == failure && !file_synced);
        assert(!validations && !commits && !removes && !locked && !s_sink.fp);
        assert(boot_files[0] && boot_files[1] && boot_files[2]);
    }
    /* Cover an upload below the periodic 1 MiB sync threshold and an unsynced tail. */
    const uint32_t sizes[] = {779519, 1024 * 1024 + 17};
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); n++) {
        reset(); put_u32le(obv, sizes[n]);
        assert(process_frame(CMD_BEGIN_OBV, obv, 40) == 0);
        uint8_t payload[FRAME_DATA_MAX] = {0};
        for (uint32_t offset = 0; offset < sizes[n];) {
            uint32_t count = sizes[n] - offset;
            if (count > sizeof(payload) - 4) count = sizeof(payload) - 4;
            put_u32le(payload, offset);
            assert(process_frame(CMD_DATA, payload, count + 4) == 0);
            offset += count;
        }
        assert(!file_synced);
        assert(process_frame(CMD_END, NULL, 0) == 0);
        assert(validations == 1 && commits == 1 && syncs == 1 && !locked);
    }
    put_u32le(obv, 8);
    reset(); assert(process_frame(CMD_BEGIN_OBV, obv, 40) == 0);
    assert(process_frame(CMD_ABORT, NULL, 0) == 0); assert(removes == 0 && !locked);
    assert(boot_files[0] && boot_files[1] && boot_files[2]);
    /* Either upload format replaces any combination of previous formats,
     * including same-format reinstalls and a missing old file. */
    for (unsigned format = 0; format < 2; format++) {
        for (unsigned previous = 0; previous < 8; previous++) {
            reset();
            for (unsigned i = 0; i < 3; i++) boot_files[i] = (previous & (1u << i)) != 0;
            assert((format ? process_frame(CMD_BEGIN_OBV, obv, 40) : begin(false)) == 0);
            data(); assert(removes == 0);
            assert(process_frame(CMD_END, NULL, 0) == 0);
            assert(!locked && removes == 2 && commits == 1);
            assert(boot_files[0] == (bool)format && !boot_files[1] && boot_files[2] == !format);
        }
        /* Verification/commit failures preserve all old boot files. */
        for (unsigned failure = 0; failure < 2; failure++) {
            reset();
            assert((format ? process_frame(CMD_BEGIN_OBV, obv, 40) : begin(false)) == 0);
            data(); fail_verify = failure == 0; fail_commit = failure == 1;
            assert(process_frame(CMD_END, NULL, 0) == 6 && !locked && !removes);
            assert(boot_files[0] && boot_files[1] && boot_files[2]);
        }
        /* Cleanup failures must be reported, with the new file kept and the
         * other obsolete format still attempted. */
        for (unsigned failure = 0; failure < 2; failure++) {
            reset();
            assert((format ? process_frame(CMD_BEGIN_OBV, obv, 40) : begin(false)) == 0);
            data(); fail_remove_path = failure ? SD_BOOT_JPEG : format ? SD_BOOT_LEGACY : SD_BOOT_OBV;
            assert(process_frame(CMD_END, NULL, 0) == 6);
            assert(!locked && removes == 2 && boot_files[format ? 0 : 2]);
            assert(boot_files[1] == (bool)failure && boot_files[format ? 2 : 0] == !failure);
        }
    }
    reset();
    uint8_t request[40] = {0}; put_u32le(request, 8);
    assert(process_frame(CMD_BEGIN_RPC, request, 40) == 0);
    assert(!locked && s_rpc_session);
    uint8_t chunk[12] = {0}; memcpy(chunk + 4, "12345678", 8);
    assert(process_frame(CMD_DATA, chunk, sizeof(chunk)) == 0);
    assert(process_frame(CMD_END, NULL, 0) == 0);
    assert(rpc_calls == 1 && s_rpc_reply && !s_rpc_restart && !locked);
    /* Completed RPC replies survive sink cleanup for END retry caching. */
    send_reply(CMD_END, 42, 0, 8);
    assert(output_len == FRAME_HEADER_SIZE + 5 + strlen(s_rpc_reply));
    assert(get_u16le(output + 6) == 42 && output[5] == (CMD_END | 0x80));
    assert(get_u32le(output + 8) == output_len - FRAME_HEADER_SIZE);
    assert(output[16] == 0 && get_u32le(output + 17) == 8);
    assert(!memcmp(output + 21, s_rpc_reply, strlen(s_rpc_reply)));
    assert(get_u32le(output + 12) == esp_rom_crc32_le(0, output + 16, output_len - 16));
    abort_session(); assert(s_rpc_reply && !s_rpc_restart);
    size_t first_reply = output_len;
    send_reply(CMD_END, 42, 0, 8);
    assert(output_len == first_reply * 2 && !memcmp(output, output + first_reply, first_reply));
    assert(rpc_calls == 1);
    assert(begin(false) == 0); assert(!s_rpc_reply);
    reset(); request[0] = 0; assert(process_frame(CMD_BEGIN_RPC, request, 40) == 1);
    put_u32le(request, RPC_MAX_BYTES + 1); assert(process_frame(CMD_BEGIN_RPC, request, 40) == 1);
    reset(); puts("USB session protects active files; success, errors, abort and timeout release storage");
    return 0;
}
