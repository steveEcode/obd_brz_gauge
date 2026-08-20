#include "ota_update_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

#include "app_obd_dsp/boot_media_mount.h"
#include "bsp_obd_dsp/rs485_brake_temp.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/espnow_link.h"

#define OTA_TAG "ota_update"

typedef enum {
    OTA_KIND_IDLE = 0,
    OTA_KIND_FIRMWARE,
    OTA_KIND_BOOTMEDIA,
} ota_kind_t;

typedef enum {
    OTA_STAGE_IDLE = 0,
    OTA_STAGE_RECEIVING,
    OTA_STAGE_FINALIZING,
    OTA_STAGE_DONE,
    OTA_STAGE_ERROR,
} ota_stage_t;

typedef struct {
    ota_kind_t kind;
    ota_stage_t stage;
    char filename[32];
    uint8_t expected_sha[32];
    uint32_t expected_size;
    uint32_t received_size;
    bool is_manifest_file;
    bool bootmedia_manifest_ready;
    bool bootmedia_bin_ready;
    bool bootmedia_pending_commit;
    uint8_t bootmedia_manifest_expected_sha[32];
    uint8_t bootmedia_binary_expected_sha[32];
    uint32_t bootmedia_manifest_expected_size;
    uint32_t bootmedia_binary_expected_size;
    bool bootmedia_manifest_expected_set;
    bool bootmedia_binary_expected_set;
    char staged_path[160];
    FILE *stage_file;
    const esp_partition_t *ota_partition;
    esp_ota_handle_t ota_handle;
    bool ota_started;
    mbedtls_sha256_context sha;
    bool sha_started;
    int64_t last_progress_us;
} ota_state_t;

typedef struct {
    bool active;
    bool manifest_ready;
    bool bin_ready;
    uint8_t manifest_expected_sha[32];
    uint8_t binary_expected_sha[32];
    uint32_t manifest_expected_size;
    uint32_t binary_expected_size;
} bootmedia_package_state_t;

enum {
    IDX_OTA_SVC,
    IDX_OTA_CHAR_CONTROL,
    IDX_OTA_CHAR_VAL_CONTROL,
    IDX_OTA_CHAR_DATA,
    IDX_OTA_CHAR_VAL_DATA,
    IDX_OTA_CHAR_STATUS,
    IDX_OTA_CHAR_VAL_STATUS,
    IDX_OTA_CHAR_CFG_STATUS,
    IDX_OTA_NB,
};

static const uint16_t s_attr_uuid_primary_service = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_attr_uuid_char_declare = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_attr_uuid_char_client_cfg = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t s_char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t s_char_prop_write_nr = ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t s_char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static uint16_t s_service_uuid = OBD_OTA_SERVICE_UUID;
static uint16_t s_char_uuid_control = OBD_OTA_CHAR_CONTROL;
static uint16_t s_char_uuid_data = OBD_OTA_CHAR_DATA;
static uint16_t s_char_uuid_status = OBD_OTA_CHAR_STATUS;
static uint16_t s_cccd_init = 0x0000;
static uint8_t s_status_blob[256] = {0};
static uint16_t s_status_len = 0;
static uint16_t s_handle_control = 0;
static uint16_t s_handle_data = 0;
static uint16_t s_handle_status = 0;
static uint16_t s_handle_status_cccd = 0;
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static bool s_connected = false;
static bool s_status_notify_enabled = false;

static ota_state_t s_state = {0};
static bootmedia_package_state_t s_bootmedia = {0};

static const esp_gatts_attr_db_t s_gatt_db_ota[IDX_OTA_NB] = {
    [IDX_OTA_SVC] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_primary_service, ESP_GATT_PERM_READ,
      sizeof(uint16_t), sizeof(s_service_uuid), (uint8_t *)&s_service_uuid}},

    [IDX_OTA_CHAR_CONTROL] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(s_char_prop_write), (uint8_t *)&s_char_prop_write}},

    [IDX_OTA_CHAR_VAL_CONTROL] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_control,
      ESP_GATT_PERM_WRITE,
      256, 0, NULL}},

    [IDX_OTA_CHAR_DATA] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(s_char_prop_write_nr), (uint8_t *)&s_char_prop_write_nr}},

    [IDX_OTA_CHAR_VAL_DATA] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_data,
      ESP_GATT_PERM_WRITE,
      512, 0, NULL}},

    [IDX_OTA_CHAR_STATUS] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_declare, ESP_GATT_PERM_READ,
      sizeof(uint8_t), sizeof(s_char_prop_read_notify), (uint8_t *)&s_char_prop_read_notify}},

    [IDX_OTA_CHAR_VAL_STATUS] =
    {{ESP_GATT_RSP_BY_APP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_char_uuid_status,
      ESP_GATT_PERM_READ,
      sizeof(s_status_blob), 0, s_status_blob}},

    [IDX_OTA_CHAR_CFG_STATUS] =
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t *)&s_attr_uuid_char_client_cfg,
      ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      sizeof(uint16_t), sizeof(s_cccd_init), (uint8_t *)&s_cccd_init}},
};

static void state_reset(void);
static void notify_status(const char *state, const char *target, const char *message, uint32_t received, uint32_t expected);
static void ota_abort_current(void);
static void bootmedia_session_begin(void);
static void bootmedia_session_reset(bool remove_staged_files);
static esp_err_t ota_begin_transfer(const uint8_t *packet, uint16_t len);
static esp_err_t ota_write_payload(const uint8_t *payload, uint16_t len);
static esp_err_t ota_finalize_transfer(void);
static esp_err_t bootmedia_finalize_if_ready(void);
static void start_wifi_ota(void);

static bool path_has_suffix(const char *value, const char *suffix)
{
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    if (value_len < suffix_len) {
        return false;
    }
    return strcmp(value + value_len - suffix_len, suffix) == 0;
}

static void response_json_to_status_blob(const char *json, size_t json_len)
{
    size_t len = json_len;
    if (len >= sizeof(s_status_blob)) {
        len = sizeof(s_status_blob) - 1;
    }
    memcpy(s_status_blob, json, len);
    s_status_blob[len] = '\0';
    s_status_len = (uint16_t)len;
}

static void notify_status(const char *state, const char *target, const char *message, uint32_t received, uint32_t expected)
{
    char buffer[256];
    snprintf(buffer, sizeof(buffer),
             "{\"state\":\"%s\",\"target\":\"%s\",\"message\":\"%s\",\"received\":%u,\"expected\":%u}",
             state, target, message, received, expected);
    response_json_to_status_blob(buffer, strlen(buffer));

    if (s_connected && s_status_notify_enabled && s_gatts_if != ESP_GATT_IF_NONE && s_handle_status != 0) {
        esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handle_status,
                                                    s_status_len, s_status_blob, false);
        if (err != ESP_OK) {
            ESP_LOGW(OTA_TAG, "status notify failed: %s", esp_err_to_name(err));
        }
    }
}

static void update_progress_status(void)
{
    if (s_state.expected_size == 0) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_state.received_size < s_state.expected_size &&
        s_state.received_size != 0 &&
        (s_state.received_size % (64u * 1024u)) != 0 &&
        (now_us - s_state.last_progress_us) < 750000) {
        return;
    }
    s_state.last_progress_us = now_us;

    char message[64];
    snprintf(message, sizeof(message), "%u/%u bytes", s_state.received_size, s_state.expected_size);
    const char *target = (s_state.kind == OTA_KIND_FIRMWARE) ? "firmware" : "bootmedia";
    notify_status("receiving", target, message, s_state.received_size, s_state.expected_size);
}

static void close_stage_file(void)
{
    if (s_state.stage_file) {
        fclose(s_state.stage_file);
        s_state.stage_file = NULL;
    }
}

/* ---------- Async SPIFFS writer (ring buffer + background task) ---------- */
/*
 * BLE GATTS write callbacks must return quickly.  SPIFFS fwrite is a blocking
 * flash operation (10-50 ms per call), so doing it inline blocks the BLE stack
 * and causes the phone app to see "write timeout".  We decouple the two with a
 * single-producer / single-consumer ring buffer: the BLE callback copies data
 * in (fast memcpy) and the background task drains it to SPIFFS in larger chunks.
 */

#define BM_RING_SIZE        (32 * 1024)
#define BM_WRITE_CHUNK      4096
#define BM_DRAIN_POLL_MS    5
#define BM_DRAIN_TIMEOUT_MS 5000

static uint8_t         *s_bm_ring       = NULL;
static volatile size_t  s_bm_ring_head  = 0;   /* producer (BLE callback) advances */
static volatile size_t  s_bm_ring_tail  = 0;   /* consumer (writer task) advances  */
static TaskHandle_t     s_bm_writer_task = NULL;
static SemaphoreHandle_t s_bm_writer_sem = NULL;
static volatile bool    s_bm_writer_error = false;

static size_t ring_used(void)
{
    return s_bm_ring_head - s_bm_ring_tail;
}

static size_t ring_free(void)
{
    return BM_RING_SIZE - ring_used() - 1;
}

static size_t ring_write(const uint8_t *data, size_t len)
{
    size_t avail = ring_free();
    if (len > avail) len = avail;
    for (size_t i = 0; i < len; i++) {
        s_bm_ring[(s_bm_ring_head + i) % BM_RING_SIZE] = data[i];
    }
    s_bm_ring_head += len;
    return len;
}

static size_t ring_read(uint8_t *data, size_t len)
{
    size_t avail = ring_used();
    if (len > avail) len = avail;
    for (size_t i = 0; i < len; i++) {
        data[i] = s_bm_ring[(s_bm_ring_tail + i) % BM_RING_SIZE];
    }
    s_bm_ring_tail += len;
    return len;
}

static void bm_writer_task(void *arg)
{
    (void)arg;
    static uint8_t buf[BM_WRITE_CHUNK];  /* static: avoid stack overflow (fwrite needs stack too) */

    for (;;) {
        xSemaphoreTake(s_bm_writer_sem, portMAX_DELAY);

        while (ring_used() > 0 && s_state.stage_file && !s_bm_writer_error) {
            size_t to_read = ring_used();
            if (to_read > sizeof(buf)) to_read = sizeof(buf);
            size_t got = ring_read(buf, to_read);
            size_t written = fwrite(buf, 1, got, s_state.stage_file);
            if (written != got) {
                ESP_LOGE(OTA_TAG, "SPIFFS async write failed (%u/%u)", (unsigned)written, (unsigned)got);
                s_bm_writer_error = true;
                break;
            }
        }
    }
}

static void bm_writer_ensure_started(void)
{
    if (s_bm_writer_task) return;
    s_bm_writer_sem = xSemaphoreCreateBinary();
    if (!s_bm_writer_sem) return;
    s_bm_ring = heap_caps_malloc(BM_RING_SIZE, MALLOC_CAP_DEFAULT);
    if (!s_bm_ring) {
        vSemaphoreDelete(s_bm_writer_sem);
        s_bm_writer_sem = NULL;
        return;
    }
    s_bm_ring_head = 0;
    s_bm_ring_tail = 0;
    s_bm_writer_error = false;
    xTaskCreatePinnedToCore(bm_writer_task, "bm_writer", 8192, NULL, 3,
                            &s_bm_writer_task, 1);
}

/* Submit payload to the async writer.  Pure non-blocking — never call
   vTaskDelay from a BLE GATTS callback (it blocks btu_task).  With a 32 KB
   ring the buffer should never fill under normal BLE throughput. */
static bool bm_writer_submit(const uint8_t *data, size_t len)
{
    if (!s_bm_writer_task || !s_bm_ring) {
        return false;
    }
    size_t n = ring_write(data, len);
    if (n < len) {
        ESP_LOGE(OTA_TAG, "ring buffer full, dropping %u bytes", (unsigned)(len - n));
        return false;
    }
    xSemaphoreGive(s_bm_writer_sem);
    return true;
}

/* Block until the ring buffer is fully drained (called before finalize). */
static bool bm_writer_drain(void)
{
    int timeout_ticks = pdMS_TO_TICKS(BM_DRAIN_TIMEOUT_MS) / portTICK_PERIOD_MS;
    while (ring_used() > 0 && timeout_ticks > 0) {
        xSemaphoreGive(s_bm_writer_sem);
        vTaskDelay(pdMS_TO_TICKS(BM_DRAIN_POLL_MS));
        timeout_ticks -= BM_DRAIN_POLL_MS;
    }
    return ring_used() == 0 && !s_bm_writer_error;
}

static void bm_writer_reset(void)
{
    s_bm_ring_head = 0;
    s_bm_ring_tail = 0;
    s_bm_writer_error = false;
}

static void sha_init(void)
{
    if (!s_state.sha_started) {
        mbedtls_sha256_init(&s_state.sha);
        mbedtls_sha256_starts(&s_state.sha, 0);
        s_state.sha_started = true;
    }
}

static void sha_update(const uint8_t *payload, size_t len)
{
    if (!s_state.sha_started) {
        return;
    }
    mbedtls_sha256_update(&s_state.sha, payload, len);
}

static bool sha_finish_matches(void)
{
    uint8_t digest[32] = {0};
    if (!s_state.sha_started) {
        return false;
    }
    if (mbedtls_sha256_finish(&s_state.sha, digest) != 0) {
        return false;
    }
    s_state.sha_started = false;
    return memcmp(digest, s_state.expected_sha, sizeof(digest)) == 0;
}

static void state_reset(void)
{
    if (s_bm_writer_task) {
        bm_writer_drain();
        bm_writer_reset();
    }
    close_stage_file();
    if (s_state.ota_started) {
        esp_ota_abort(s_state.ota_handle);
    }
    if (s_state.sha_started) {
        mbedtls_sha256_free(&s_state.sha);
    }
    memset(&s_state, 0, sizeof(s_state));
    s_state.kind = OTA_KIND_IDLE;
    s_state.stage = OTA_STAGE_IDLE;
}

static void ota_abort_current(void)
{
    if (s_state.kind == OTA_KIND_BOOTMEDIA) {
        bootmedia_session_reset(true);
    }
    state_reset();
    rs485_brake_temp_resume();
    notify_status("idle", "none", "ready", 0, 0);
}

static void bootmedia_session_begin(void)
{
    bootmedia_session_reset(true);
    s_bootmedia.active = true;
}

static void bootmedia_session_reset(bool remove_staged_files)
{
    if (remove_staged_files) {
        remove("/bootmedia/boot_block.txt.new");
        remove("/bootmedia/boot_block.bin.new");
    }
    memset(&s_bootmedia, 0, sizeof(s_bootmedia));
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static esp_err_t parse_bootmedia_manifest(uint8_t *binary_expected_sha, size_t sha_len, uint32_t *binary_expected_size)
{
    FILE *fp = fopen("/bootmedia/boot_block.txt.new", "rb");
    if (!fp) {
        return ESP_FAIL;
    }

    char line[256];
    char sha_hex[65] = {0};
    uint32_t binary_size = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *value = strchr(line, '=');
        if (!value) {
            continue;
        }
        *value++ = '\0';
        line[strcspn(line, "\r\n\t ")] = '\0';
        value[strcspn(value, "\r\n")] = '\0';

        if (strcmp(line, "binary_sha256") == 0) {
            strncpy(sha_hex, value, sizeof(sha_hex) - 1);
        } else if (strcmp(line, "binary_size") == 0) {
            binary_size = (uint32_t)strtoul(value, NULL, 10);
        }
    }
    fclose(fp);

    if (strlen(sha_hex) != 64 || binary_size == 0) {
        return ESP_FAIL;
    }

    for (size_t index = 0; index < sha_len; index += 1) {
        char chunk[3] = { sha_hex[index * 2], sha_hex[index * 2 + 1], '\0' };
        binary_expected_sha[index] = (uint8_t)strtoul(chunk, NULL, 16);
    }
    *binary_expected_size = binary_size;
    return ESP_OK;
}

static esp_err_t bootmedia_finalize_if_ready(void)
{
    if (!s_bootmedia.manifest_ready || !s_bootmedia.bin_ready) {
        return ESP_OK;
    }

    uint8_t binary_expected_sha[32] = {0};
    uint32_t binary_expected_size = 0;
    if (parse_bootmedia_manifest(binary_expected_sha, sizeof(binary_expected_sha), &binary_expected_size) != ESP_OK) {
        notify_status("error", "bootmedia", "manifest invalid", s_state.received_size, s_state.expected_size);
        return ESP_FAIL;
    }

    if (memcmp(binary_expected_sha, s_bootmedia.binary_expected_sha, sizeof(binary_expected_sha)) != 0 ||
        binary_expected_size != s_bootmedia.binary_expected_size) {
        notify_status("error", "bootmedia", "manifest mismatch", s_state.received_size, s_state.expected_size);
        return ESP_FAIL;
    }

    notify_status("applying", "bootmedia", "committing update", s_state.received_size, s_state.expected_size);
    if (!boot_media_commit_incoming_update()) {
        notify_status("error", "bootmedia", "commit failed", s_state.received_size, s_state.expected_size);
        return ESP_FAIL;
    }

    notify_status("done", "bootmedia", "boot animation updated, rebooting", s_state.received_size, s_state.expected_size);
    bootmedia_session_reset(false);
    state_reset();
    xTaskCreate(restart_task, "ota_restart_bm", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

static esp_err_t ota_begin_firmware(const uint8_t *sha)
{
    s_state.ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_state.ota_partition) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(s_state.ota_partition, s_state.expected_size, &s_state.ota_handle);
    if (err != ESP_OK) {
        return err;
    }

    s_state.ota_started = true;
    sha_init();
    memcpy(s_state.expected_sha, sha, 32);
    s_state.received_size = 0;
    return ESP_OK;
}

static esp_err_t ota_begin_bootmedia_file(const char *filename, const uint8_t *sha)
{
    if (!boot_media_mount()) {
        return ESP_FAIL;
    }

    bool is_manifest_file = path_has_suffix(filename, ".txt");
    bool is_binary_file = path_has_suffix(filename, ".bin");
    if (!is_manifest_file && !is_binary_file) {
        return ESP_FAIL;
    }

    if (is_manifest_file || !s_bootmedia.active || s_bootmedia.bin_ready) {
        if (!boot_media_remove_active_block()) {
            return ESP_FAIL;
        }
        bootmedia_session_begin();
    }

    const char *suffix = path_has_suffix(filename, ".txt") ? ".txt.new" : ".bin.new";

    snprintf(s_state.staged_path, sizeof(s_state.staged_path), "/bootmedia/boot_block%s", suffix);
    remove(s_state.staged_path);
    s_state.stage_file = fopen(s_state.staged_path, "wb");
    if (!s_state.stage_file) {
        return ESP_FAIL;
    }

    bm_writer_ensure_started();
    bm_writer_reset();

    s_state.sha_started = false;
    sha_init();
    memcpy(s_state.expected_sha, sha, 32);
    s_state.received_size = 0;
    s_state.is_manifest_file = path_has_suffix(filename, ".txt");
    return ESP_OK;
}

static esp_err_t ota_begin_transfer(const uint8_t *packet, uint16_t len)
{
    if (len < 45) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(packet, "OTA1", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t command = packet[4];
    uint8_t target = packet[5];
    uint32_t expected_size = (uint32_t)packet[8] |
                             ((uint32_t)packet[9] << 8) |
                             ((uint32_t)packet[10] << 16) |
                             ((uint32_t)packet[11] << 24);
    const uint8_t *expected_sha = &packet[12];
    uint8_t filename_len = packet[44];
    if (command != 1 || 45u + filename_len > len) {
        return ESP_ERR_INVALID_ARG;
    }

    char filename[32] = {0};
    if (filename_len >= sizeof(filename)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(filename, &packet[45], filename_len);
    filename[filename_len] = '\0';

    ota_abort_current();
    rs485_brake_temp_pause();
    memset(&s_state, 0, sizeof(s_state));
    s_state.expected_size = expected_size;
    memcpy(s_state.expected_sha, expected_sha, sizeof(s_state.expected_sha));
    strncpy(s_state.filename, filename, sizeof(s_state.filename) - 1);
    s_state.kind = (target == 1) ? OTA_KIND_FIRMWARE : (target == 2 ? OTA_KIND_BOOTMEDIA : OTA_KIND_IDLE);
    s_state.stage = OTA_STAGE_RECEIVING;

    if (s_state.kind == OTA_KIND_FIRMWARE) {
        if (ota_begin_firmware(expected_sha) != ESP_OK) {
            ota_abort_current();
            return ESP_FAIL;
        }
        notify_status("receiving", "firmware", "firmware transfer started", 0, s_state.expected_size);
        return ESP_OK;
    }

    if (s_state.kind == OTA_KIND_BOOTMEDIA) {
        if (ota_begin_bootmedia_file(filename, expected_sha) != ESP_OK) {
            ota_abort_current();
            return ESP_FAIL;
        }
        notify_status("receiving", path_has_suffix(filename, ".txt") ? "bootmedia-manifest" : "bootmedia-binary",
                      "bootmedia file transfer started", 0, s_state.expected_size);
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t ota_write_payload(const uint8_t *payload, uint16_t len)
{
    if (s_state.stage != OTA_STAGE_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state.received_size + len > s_state.expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_state.kind == OTA_KIND_FIRMWARE) {
        esp_err_t err = esp_ota_write(s_state.ota_handle, payload, len);
        if (err != ESP_OK) {
            return err;
        }
    } else if (s_state.kind == OTA_KIND_BOOTMEDIA) {
        if (!s_state.stage_file) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!bm_writer_submit(payload, len)) {
            ESP_LOGE(OTA_TAG, "Async writer submit failed (ring full)");
            return ESP_FAIL;
        }
    }

    sha_update(payload, len);
    s_state.received_size += len;
    update_progress_status();
    return ESP_OK;
}

static esp_err_t ota_finalize_transfer(void)
{
    if (s_state.stage != OTA_STAGE_RECEIVING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state.received_size != s_state.expected_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!sha_finish_matches()) {
        return ESP_ERR_INVALID_CRC;
    }

    if (s_state.kind == OTA_KIND_FIRMWARE) {
        esp_err_t err = esp_ota_end(s_state.ota_handle);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_ota_set_boot_partition(s_state.ota_partition);
        if (err != ESP_OK) {
            return err;
        }

        notify_status("done", "firmware", "firmware ready, rebooting", s_state.received_size, s_state.expected_size);
        s_state.stage = OTA_STAGE_DONE;
        s_state.ota_started = false;
        s_state.sha_started = false;
        xTaskCreate(restart_task, "ota_restart", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
        state_reset();
        return ESP_OK;
    }

    if (s_state.kind == OTA_KIND_BOOTMEDIA) {
        if (!bm_writer_drain()) {
            ESP_LOGE(OTA_TAG, "Async writer drain failed (timeout or write error)");
            close_stage_file();
            return ESP_FAIL;
        }
        fflush(s_state.stage_file);
        close_stage_file();
        bm_writer_reset();
        if (s_state.is_manifest_file) {
            s_bootmedia.manifest_ready = true;
            s_bootmedia.manifest_expected_size = s_state.expected_size;
            memcpy(s_bootmedia.manifest_expected_sha, s_state.expected_sha, sizeof(s_bootmedia.manifest_expected_sha));
            notify_status("receiving", "bootmedia", "manifest staged", s_state.received_size, s_state.expected_size);
        } else {
            s_bootmedia.bin_ready = true;
            s_bootmedia.binary_expected_size = s_state.expected_size;
            memcpy(s_bootmedia.binary_expected_sha, s_state.expected_sha, sizeof(s_bootmedia.binary_expected_sha));
            notify_status("receiving", "bootmedia", "binary staged", s_state.received_size, s_state.expected_size);
        }
        esp_err_t err = bootmedia_finalize_if_ready();
        if (err != ESP_OK) {
            return err;
        }
        state_reset();
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

static void parse_control_write(const uint8_t *buf, uint16_t len)
{
    ESP_LOGI(OTA_TAG, "Control write received: len=%u", len);
    if (len < 5 || memcmp(buf, "OTA1", 4) != 0) {
        notify_status("error", "none", "invalid control packet", 0, 0);
        ota_abort_current();
        return;
    }

    uint8_t command = buf[4];
    if (command == 1) {
        if (ota_begin_transfer(buf, len) != ESP_OK) {
            notify_status("error", "none", "begin rejected", 0, 0);
            ota_abort_current();
        }
        return;
    }

    if (command == 2) {
        if (ota_finalize_transfer() != ESP_OK) {
            notify_status("error", "none", "finalize failed", s_state.received_size, s_state.expected_size);
            ota_abort_current();
        }
        return;
    }

    if (command == 3) {
        bootmedia_session_reset(true);
        notify_status("idle", "none", "transfer cancelled", 0, 0);
        ota_abort_current();
    }

    if (command == 4) {
        // 启动 WiFi OTA（BLE 握手，WiFi 传输）
        rs485_brake_temp_pause();
        start_wifi_ota();
    }
}

static void handle_status_cccd_write(const uint8_t *buf, uint16_t len)
{
    if (len < 2) {
        return;
    }
    uint16_t cccd = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    s_status_notify_enabled = ((cccd & 0x0001u) != 0);
    ESP_LOGI(OTA_TAG, "Status notify %s", s_status_notify_enabled ? "enabled" : "disabled");
}

// WiFi OTA 状态回调 — 将 WiFi 传输进度通过 BLE 通知 APP
static void wifi_ota_status_callback(ota_wifi_state_t state, const char *message, uint32_t received, uint32_t expected)
{
    const char *state_str = "idle";
    switch (state) {
        case OTA_WIFI_STATE_STARTING: state_str = "wifi-starting"; break;
        case OTA_WIFI_STATE_READY: state_str = "wifi-ready"; break;
        case OTA_WIFI_STATE_RECEIVING: state_str = "wifi-receiving"; break;
        case OTA_WIFI_STATE_DONE: state_str = "wifi-done"; break;
        case OTA_WIFI_STATE_ERROR: state_str = "wifi-error"; break;
        default: break;
    }
    notify_status(state_str, "wifi-ota", message, received, expected);
}

// Send the final WiFi-ready envelope with credentials included.  The server's
// READY callback fires before ota_wifi_server_start() returns the generated
// credentials, so the first READY status cannot contain the SSID/password.
static void notify_wifi_ready(const ota_wifi_info_t *info)
{
    char buffer[256];
    int len = snprintf(buffer, sizeof(buffer),
                       "{\"state\":\"wifi-ready\",\"target\":\"wifi-ota\","
                       "\"message\":\"wifi ota ready\",\"received\":0,\"expected\":0,"
                       "\"ssid\":\"%s\",\"password\":\"%s\",\"ip\":\"%s\","
                       "\"token\":\"%s\",\"port\":%u}",
                       info->ssid, info->password, info->ip, info->token, info->port);
    if (len < 0) {
        ESP_LOGE(OTA_TAG, "WiFi-ready JSON formatting failed");
        return;
    }
    response_json_to_status_blob(buffer, (size_t)len);

    if (s_connected && s_status_notify_enabled && s_gatts_if != ESP_GATT_IF_NONE && s_handle_status != 0) {
        esp_err_t err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handle_status,
                                                    s_status_len, s_status_blob, false);
        if (err != ESP_OK) {
            ESP_LOGW(OTA_TAG, "WiFi-ready notify failed: %s", esp_err_to_name(err));
        }
    }
}

// command 4: 启动 WiFi OTA
static void start_wifi_ota(void)
{
    // Stop ESP-NOW to free internal RAM for WiFi SoftAP + WPA2 encryption
    espnow_link_stop();

    ota_wifi_info_t info = {0};
    if (!ota_wifi_server_start(&info, wifi_ota_status_callback)) {
        notify_status("error", "none", "wifi ota start failed", 0, 0);
        return;
    }

    // Include credentials in the same envelope as wifi-ready.  Previously the
    // credentials were copied to the read blob and immediately overwritten by
    // notify_status(), so the app received wifi-ready but could not parse them.
    notify_wifi_ready(&info);

    ESP_LOGI(OTA_TAG, "WiFi OTA started: SSID=%s IP=%s", info.ssid, info.ip);
}

void ota_update_ble_start(esp_gatt_if_t gatts_if)
{
    esp_ble_gatts_create_attr_tab(s_gatt_db_ota, gatts_if, IDX_OTA_NB, 3);
}

void ota_update_ble_on_connect(esp_gatt_if_t gatts_if, uint16_t conn_id)
{
    s_gatts_if = gatts_if;
    s_conn_id = conn_id;
    s_connected = true;
    notify_status("idle", "none", "ready", 0, 0);
}

void ota_update_ble_on_disconnect(void)
{
    s_connected = false;
    s_status_notify_enabled = false;
    // Don't abort WiFi OTA when BLE disconnects — the user switches from BLE to
    // WiFi for the actual transfer, and BLE disconnecting is a normal part of that
    // flow. The WiFi OTA server handles its own timeout/error cleanup internally.
    if (ota_wifi_server_get_state() == OTA_WIFI_STATE_IDLE) {
        ota_wifi_server_stop();
    }
    ota_abort_current();
}

bool ota_update_ble_is_connected(void)
{
    return s_connected;
}

bool ota_update_ble_is_active(void)
{
    return s_connected && s_state.ota_started;
}

void ota_update_ble_on_gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status == ESP_GATT_OK &&
            param->add_attr_tab.num_handle == IDX_OTA_NB &&
            param->add_attr_tab.svc_uuid.uuid.uuid16 == OBD_OTA_SERVICE_UUID) {
            uint16_t *handles = param->add_attr_tab.handles;
            s_handle_control = handles[IDX_OTA_CHAR_VAL_CONTROL];
            s_handle_data = handles[IDX_OTA_CHAR_VAL_DATA];
            s_handle_status = handles[IDX_OTA_CHAR_VAL_STATUS];
            s_handle_status_cccd = handles[IDX_OTA_CHAR_CFG_STATUS];
            esp_ble_gatts_start_service(handles[IDX_OTA_SVC]);
            ESP_LOGI(OTA_TAG, "OTA service started");
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        ota_update_ble_on_connect(gatts_if, param->connect.conn_id);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ota_update_ble_on_disconnect();
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle_control) {
            parse_control_write(param->write.value, param->write.len);
        } else if (param->write.handle == s_handle_data) {
            if (ota_write_payload(param->write.value, param->write.len) != ESP_OK) {
                notify_status("error", "none", "data write failed", s_state.received_size, s_state.expected_size);
                ota_abort_current();
            }
        } else if (param->write.handle == s_handle_status_cccd) {
            handle_status_cccd_write(param->write.value, param->write.len);
        }
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_handle_status) {
            esp_gatt_rsp_t rsp = {0};
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.offset = param->read.offset;
            uint16_t available = (s_status_len > param->read.offset) ? (uint16_t)(s_status_len - param->read.offset) : 0;
            uint16_t copy_len = available;
            if (copy_len > sizeof(rsp.attr_value.value)) {
                copy_len = sizeof(rsp.attr_value.value);
            }
            if (copy_len > 0) {
                memcpy(rsp.attr_value.value, s_status_blob + param->read.offset, copy_len);
            }
            rsp.attr_value.len = copy_len;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
        }
        break;

    default:
        break;
    }
}
