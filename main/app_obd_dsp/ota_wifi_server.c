// ota_wifi_server.c — WiFi OTA server (SoftAP + HTTP)
//
// Handles the WiFi side of the BLE-handshake/WiFi-transfer OTA architecture:
//   phone connects via BLE, gets the SoftAP credentials + session token via BLE
//   notify, joins the AP and POSTs firmware/bootmedia over HTTP.
//
// Coexistence with ESP-NOW (master/slave multi-gauge link):
//   ESP-NOW runs on the STA interface in WIFI_MODE_STA on channel 1. This module
//   NEVER deinitialises WiFi/netif/event-loop that it did not create. When the
//   WiFi stack already exists it switches to APSTA on channel 1, starts the AP,
//   and on stop merely restores the previous mode — leaving the ESP-NOW driver,
//   peers and channel untouched.
#include "ota_wifi_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

#include "app_obd_dsp/boot_media_mount.h"
#include "app_obd_dsp/device_identity.h"
#include "bsp_obd_dsp/rs485_brake_temp.h"

static const char *TAG = "ota_wifi";

#define WIFI_SSID_PREFIX    "OBD-Gauge-OTA-"
#define WIFI_PORT           80
#define WIFI_MAX_STA        1
#define WIFI_CHANNEL        1              // match ESP-NOW channel: single-radio AP+STA must share a channel
#define WIFI_PASS_LEN       8              // WPA2 PSK minimum
// The companion app no longer uses BLE handshake.  The phone joins the SoftAP
// manually (password is fixed) then fetches the per-session token from
// GET /ota/discover.  Write endpoints still require the token.
#define OTA_WIFI_PASSWORD   "88888888"
#define OTA_RECV_CHUNK      16384          // receive/decode buffer; flash writes use a separate internal scratch buffer
#define OTA_FLASH_WRITE_CHUNK 4096         // internal scratch buffer used for flash-safe writes
#define OTA_PROGRESS_STEP   (64 * 1024)    // report to BLE callback every 64KB
#define OTA_IDLE_TIMEOUT_US (10 * 60 * 1000000LL)  // auto-stop when no HTTP activity for 10 minutes
// HTTPD task stack size is measured in bytes. 4 KB is the default and enough
// for these handlers; flash operations must run from an internal-RAM task.
#define OTA_WIFI_STACK_SIZE 4096

static ota_wifi_state_t s_state = OTA_WIFI_STATE_IDLE;
static ota_wifi_status_cb_t s_status_cb = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_token[17] = {0};
static char s_password[17] = {0};   // random per-session WPA2 password, delivered via BLE notify
static char s_ap_ssid[33] = {0};
static bool s_netif_owned = false;
static bool s_event_loop_owned = false;
static bool s_wifi_owned = false;
static wifi_mode_t s_prev_wifi_mode = WIFI_MODE_NULL;
static volatile int64_t s_last_activity_us = 0;
static uint8_t s_flash_write_buf[OTA_FLASH_WRITE_CHUNK];

// OTA 接收状态
typedef enum {
    RECV_KIND_NONE = 0,
    RECV_KIND_FIRMWARE,
    RECV_KIND_BOOTMEDIA_MANIFEST,
    RECV_KIND_BOOTMEDIA_BIN,
} recv_kind_t;

typedef struct {
    recv_kind_t kind;
    uint32_t expected_size;
    uint32_t received_size;
    uint8_t expected_sha[32];
    char expected_sha_hex[65];
    mbedtls_sha256_context sha;
    bool sha_started;

    // firmware
    const esp_partition_t *ota_partition;
    esp_ota_handle_t ota_handle;
    bool ota_started;

    // bootmedia
    FILE *stage_file;
    char staged_path[128];
} ota_recv_t;

static ota_recv_t s_recv = {0};

/* ------------------------------------------------------------------ */
/* WiFi runtime: reuse the existing stack when present (ESP-NOW case) */
/* ------------------------------------------------------------------ */

static esp_err_t wifi_runtime_prepare(wifi_mode_t *current_mode)
{
    // netif / event loop are shared infrastructure: if they already exist
    // (ESP-NOW, other subsystems) we reuse them and must NOT tear them down.
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        s_netif_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        s_event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_wifi_get_mode(current_mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_owned = true;
        *current_mode = WIFI_MODE_NULL;
        return ESP_OK;
    }

    return err;
}

static void wifi_runtime_cleanup(bool restore_mode)
{
    if (restore_mode && !s_wifi_owned && s_prev_wifi_mode != WIFI_MODE_NULL) {
        esp_err_t err = esp_wifi_set_mode(s_prev_wifi_mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore WiFi mode failed: %s", esp_err_to_name(err));
        }
    }

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    // Only tear down WiFi when WE created it. ESP-NOW keeps running otherwise.
    if (s_wifi_owned) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "wifi stop failed during cleanup: %s", esp_err_to_name(err));
        }
        err = esp_wifi_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "wifi deinit failed during cleanup: %s", esp_err_to_name(err));
        }
    }

    // netif and event loop are shared across subsystems — never delete them here.
    s_wifi_owned = false;
    s_prev_wifi_mode = WIFI_MODE_NULL;
    s_netif_owned = false;
    s_event_loop_owned = false;
}

/* ------------------------------------------------------------------ */
/* Status notifications (also updates the idle-activity timestamp)    */
/* ------------------------------------------------------------------ */

static void notify_status(ota_wifi_state_t state, const char *message, uint32_t received, uint32_t expected)
{
    s_last_activity_us = esp_timer_get_time();   // any state change counts as activity
    s_state = state;
    if (s_status_cb) {
        s_status_cb(state, message, received, expected);
    }
}

/* ------------------------------------------------------------------ */
/* SHA-256 helper                                                      */
/* ------------------------------------------------------------------ */

static void sha_init(void)
{
    if (!s_recv.sha_started) {
        mbedtls_sha256_init(&s_recv.sha);
        mbedtls_sha256_starts(&s_recv.sha, 0);
        s_recv.sha_started = true;
    }
}

static void sha_update(const uint8_t *data, size_t len)
{
    if (s_recv.sha_started) {
        mbedtls_sha256_update(&s_recv.sha, data, len);
    }
}

static bool sha_finish_matches(void)
{
    if (!s_recv.sha_started) return false;
    uint8_t digest[32] = {0};
    if (mbedtls_sha256_finish(&s_recv.sha, digest) != 0) return false;
    s_recv.sha_started = false;
    return memcmp(digest, s_recv.expected_sha, 32) == 0;
}

static esp_err_t flash_safe_ota_write(esp_ota_handle_t handle, const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > sizeof(s_flash_write_buf)) {
            chunk = sizeof(s_flash_write_buf);
        }
        memcpy(s_flash_write_buf, data + offset, chunk);
        esp_err_t err = esp_ota_write(handle, s_flash_write_buf, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            return err;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t flash_safe_file_write(FILE *file, const uint8_t *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > sizeof(s_flash_write_buf)) {
            chunk = sizeof(s_flash_write_buf);
        }
        memcpy(s_flash_write_buf, data + offset, chunk);
        size_t written = fwrite(s_flash_write_buf, 1, chunk, file);
        if (written != chunk) {
            ESP_LOGE(TAG, "SPIFFS write failed (%u/%u)", (unsigned)written, (unsigned)chunk);
            return ESP_FAIL;
        }
        offset += chunk;
    }
    return ESP_OK;
}

static void recv_reset(void)
{
    if (s_recv.ota_started) {
        esp_ota_abort(s_recv.ota_handle);
        s_recv.ota_started = false;
    }
    if (s_recv.stage_file) {
        fclose(s_recv.stage_file);
        s_recv.stage_file = NULL;
    }
    if (s_recv.sha_started) {
        mbedtls_sha256_free(&s_recv.sha);
        s_recv.sha_started = false;
    }
    memset(&s_recv, 0, sizeof(s_recv));
}

static bool parse_hex_sha(const char *hex, uint8_t *out)
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        char chunk[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtoul(chunk, NULL, 16);
    }
    return true;
}

static bool validate_token(httpd_req_t *req)
{
    char buf[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    // Constant-time compare to avoid token timing side channels.
    if (strlen(buf) != strlen(s_token)) {
        return false;
    }
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < strlen(s_token); i++) {
        diff |= (uint8_t)(buf[i] ^ s_token[i]);
    }
    return diff == 0;
}

static void set_cors_headers(httpd_req_t *req)
{
    // Capacitor WebView runs on a different origin than 192.168.4.1.
    // Include CORS headers for both the discovery GET and custom-header POSTs.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                      "X-OTA-Token,X-OTA-SHA256,X-OTA-Size,X-OTA-Manifest-Size,Content-Type");
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t send_json_response(httpd_req_t *req, int status, const char *json)
{
    set_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200 ? "200 OK" : "400 Bad Request");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t send_err(httpd_req_t *req, const char *err)
{
    // Truncate the error message to keep the JSON response small and well-formed.
    char json[80];
    snprintf(json, sizeof(json), "{\"error\":\"%s\"}", err);
    return send_json_response(req, 400, json);
}

/* Periodic status report while receiving (throttled by OTA_PROGRESS_STEP). */
static void report_progress(void)
{
    if (s_recv.received_size % OTA_PROGRESS_STEP == 0) {
        notify_status(OTA_WIFI_STATE_RECEIVING, "receiving",
                      s_recv.received_size, s_recv.expected_size);
    }
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

// GET / — captive portal detection.  Android/iOS probe this immediately after
// associating.  Return a lightweight 204 so the phone doesn't hang waiting
// for a 404 error page that consumes heap and triggers the WDT.
static esp_err_t root_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// GET /ota/discover
// No auth: the phone must learn the per-session token before it can call any
// authenticated endpoint.  Returning it here (after it joined the AP via the
// fixed bootstrap passphrase) is the BLE-independent bootstrap path.
static esp_err_t discover_handler(httpd_req_t *req)
{
    if (s_state == OTA_WIFI_STATE_IDLE) {
        return send_err(req, "ota not active");
    }
    char json[192];
    int written = snprintf(json, sizeof(json),
                           "{\"ssid\":\"%s\",\"password\":\"%s\",\"ip\":\"192.168.4.1\","
                           "\"token\":\"%s\",\"port\":%d}",
                           s_ap_ssid, s_password, s_token, WIFI_PORT);
    if (written < 0 || written >= (int)sizeof(json)) {
        ESP_LOGE(TAG, "discover JSON truncated (len=%d, cap=%u)", written, (unsigned)sizeof(json));
        return send_err(req, "discover response too large");
    }
    return send_json_response(req, 200, json);
}

// GET /ota/info — device identity manifest, no auth (bootstrap endpoint)
static esp_err_t info_handler(httpd_req_t *req)
{
    if (s_state == OTA_WIFI_STATE_IDLE) {
        return send_err(req, "ota not active");
    }
    const char *manifest = device_identity_manifest_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, manifest, strlen(manifest));
    return ESP_OK;
}

// GET /ota/status
static esp_err_t status_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char json[128];
    const char *state_str = "idle";
    switch (s_state) {
        case OTA_WIFI_STATE_READY: state_str = "ready"; break;
        case OTA_WIFI_STATE_RECEIVING: state_str = "receiving"; break;
        case OTA_WIFI_STATE_DONE: state_str = "done"; break;
        case OTA_WIFI_STATE_ERROR: state_str = "error"; break;
        default: break;
    }
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"received\":%u,\"expected\":%u}",
             state_str, s_recv.received_size, s_recv.expected_size);
    return send_json_response(req, 200, json);
}

// POST /ota/firmware
// 支持两种格式：
// 1. Content-Type: application/octet-stream - 原始二进制
// 2. Content-Type: application/base64 - Base64 编码
static esp_err_t firmware_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK) {
        return send_err(req, "missing headers");
    }

    // 检查是否是 Base64 编码
    bool is_base64 = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_base64 = (strstr(content_type, "base64") != NULL);
    }

    uint32_t expected_size = (uint32_t)strtoul(size_str, NULL, 10);
    s_recv.ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_recv.ota_partition) {
        return send_err(req, "no ota partition");
    }
    if (expected_size == 0 || expected_size > s_recv.ota_partition->size ||
        !parse_hex_sha(sha_hex, s_recv.expected_sha)) {
        return send_err(req, "invalid headers");
    }

    esp_err_t err = esp_ota_begin(s_recv.ota_partition, expected_size, &s_recv.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return send_err(req, "ota begin failed");
    }
    s_recv.ota_started = true;
    s_recv.kind = RECV_KIND_FIRMWARE;
    s_recv.expected_size = expected_size;
    s_recv.received_size = 0;
    sha_init();

    notify_status(OTA_WIFI_STATE_RECEIVING, "firmware receiving", 0, expected_size);

    // 接收缓冲区 - 显式从 PSRAM 分配，保留内部 RAM 给 WiFi/SPIFFS DMA
    char *recv_buf = heap_caps_malloc(OTA_RECV_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *decode_buf = NULL;
    if (!recv_buf) {
        recv_reset();
        return send_err(req, "no memory");
    }
    if (is_base64) {
        // Base64 解码后数据会更小，预留足够空间
        decode_buf = heap_caps_malloc(OTA_RECV_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decode_buf) {
            free(recv_buf);
            recv_reset();
            return send_err(req, "no memory");
        }
    }

    int remaining = req->content_len;
    bool failed = false;
    const uint32_t total_expected_size = expected_size;
    while (remaining > 0 && !failed) {
        int to_read = remaining > OTA_RECV_CHUNK ? OTA_RECV_CHUNK : remaining;
        int received = httpd_req_recv(req, recv_buf, to_read);
        if (received <= 0) {
            failed = true;
            break;
        }

        const uint8_t *data_to_write;
        size_t data_len;

        if (is_base64) {
            // Base64 解码
            size_t decode_len = 0;
            int ret = mbedtls_base64_decode(decode_buf, OTA_RECV_CHUNK, &decode_len, (const unsigned char *)recv_buf, received);
            if (ret != 0) {
                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                failed = true;
                break;
            }
            data_to_write = decode_buf;
            data_len = decode_len;
        } else {
            data_to_write = (const uint8_t *)recv_buf;
            data_len = received;
        }

        err = flash_safe_ota_write(s_recv.ota_handle, data_to_write, data_len);
        if (err != ESP_OK) {
            failed = true;
            break;
        }

        sha_update(data_to_write, data_len);
        s_recv.received_size += data_len;
        remaining -= received;
        report_progress();
    }
    free(recv_buf);
    if (decode_buf) free(decode_buf);

    if (failed || s_recv.received_size != s_recv.expected_size) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware receive failed", received_size, total_expected_size);
        return send_err(req, "receive failed");
    }

    if (!sha_finish_matches()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware sha mismatch", received_size, total_expected_size);
        return send_err(req, "sha mismatch");
    }

    err = esp_ota_end(s_recv.ota_handle);
    if (err != ESP_OK) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware ota end failed", received_size, total_expected_size);
        return send_err(req, "ota end failed");
    }

    err = esp_ota_set_boot_partition(s_recv.ota_partition);
    if (err != ESP_OK) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware set boot failed", received_size, total_expected_size);
        return send_err(req, "set boot failed");
    }

    s_recv.ota_started = false;
    notify_status(OTA_WIFI_STATE_DONE, "firmware updated, rebooting", s_recv.received_size, s_recv.expected_size);

    // 先回成功响应给 App，再重启。否则 App 等不到 HTTP 200，会把一次成功的
    // 刷写误报为失败。500ms 的延迟留给响应真正发出去。
    send_json_response(req, 200, "{\"ok\":true,\"message\":\"firmware updated, rebooting\"}");

    // 延迟重启
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

// POST /ota/bootmedia
// 接收 boot_block.txt 和 boot_block.bin 打包上传
// 支持两种格式：
// 1. Content-Type: application/octet-stream - 原始二进制
// 2. Content-Type: application/base64 - Base64 编码
// 格式: [manifest][bin]
static esp_err_t bootmedia_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char manifest_size_str[16] = {0};
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Manifest-Size", manifest_size_str, sizeof(manifest_size_str)) != ESP_OK) {
        return send_err(req, "missing headers");
    }

    // 检查是否是 Base64 编码
    bool is_base64 = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_base64 = (strstr(content_type, "base64") != NULL);
    }

    uint32_t total_size = (uint32_t)strtoul(size_str, NULL, 10);
    uint32_t manifest_size = (uint32_t)strtoul(manifest_size_str, NULL, 10);
    esp_err_t err = ESP_OK;
    if (total_size == 0 || manifest_size == 0 || manifest_size >= total_size) {
        return send_err(req, "invalid sizes");
    }
    if (!parse_hex_sha(sha_hex, s_recv.expected_sha)) {
        return send_err(req, "invalid sha");
    }

    if (!boot_media_mount()) {
        return send_err(req, "mount failed");
    }

    // Always clear the previous boot animation before staging a fresh one.
    if (!boot_media_remove_active_block()) {
        return send_err(req, "cleanup failed");
    }

    // Check SPIFFS space after clearing the old animation slot.
    size_t free_space = boot_media_get_free_space();

    const size_t needed_space = (size_t)total_size + 8192;

    if (free_space < needed_space) {
        ESP_LOGE(TAG, "SPIFFS space insufficient: free=%zu, need=%lu (total=%lu)",
                 free_space, (unsigned long)needed_space, (unsigned long)total_size);
        char err_msg[80];
        snprintf(err_msg, sizeof(err_msg), "bootmedia too large: free=%zu need=%lu", free_space, (unsigned long)needed_space);
        return send_err(req, err_msg);
    }

    // 先接收 manifest
    snprintf(s_recv.staged_path, sizeof(s_recv.staged_path), "/bootmedia/boot_block.txt.new");
    s_recv.stage_file = fopen(s_recv.staged_path, "wb");
    if (!s_recv.stage_file) {
        return send_err(req, "create manifest failed");
    }

    s_recv.kind = RECV_KIND_BOOTMEDIA_MANIFEST;
    s_recv.expected_size = manifest_size;
    s_recv.received_size = 0;
    sha_init();

    notify_status(OTA_WIFI_STATE_RECEIVING, "bootmedia manifest receiving", 0, manifest_size);

    char *recv_buf = heap_caps_malloc(OTA_RECV_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *decode_buf = NULL;
    if (!recv_buf) {
        recv_reset();
        return send_err(req, "no memory");
    }
    if (is_base64) {
        decode_buf = heap_caps_malloc(OTA_RECV_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decode_buf) {
            free(recv_buf);
            recv_reset();
            return send_err(req, "no memory");
        }
    }

    int remaining = req->content_len;
    bool failed = false;
    uint32_t bytes_written_to_manifest = 0;
    uint32_t bytes_written_to_bin = 0;
    uint32_t bin_size = total_size - manifest_size;

    while (remaining > 0 && !failed) {
        int to_read = remaining > OTA_RECV_CHUNK ? OTA_RECV_CHUNK : remaining;
        int received = httpd_req_recv(req, recv_buf, to_read);
        if (received <= 0) {
            failed = true;
            break;
        }

        const uint8_t *data_to_write;
        size_t data_len;

        if (is_base64) {
            // Base64 解码
            size_t decode_len = 0;
            int ret = mbedtls_base64_decode(decode_buf, OTA_RECV_CHUNK, &decode_len, (const unsigned char *)recv_buf, received);
            if (ret != 0) {
                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                failed = true;
                break;
            }
            data_to_write = decode_buf;
            data_len = decode_len;
        } else {
            data_to_write = (const uint8_t *)recv_buf;
            data_len = received;
        }

        // 写入 manifest 或 bin
        size_t offset = 0;
        while (offset < data_len && !failed) {
            if (bytes_written_to_manifest < manifest_size) {
                // 还在写 manifest
                size_t to_write = data_len - offset;
                if (bytes_written_to_manifest + to_write > manifest_size) {
                    to_write = manifest_size - bytes_written_to_manifest;
                }
                err = flash_safe_file_write(s_recv.stage_file, data_to_write + offset, to_write);
                if (err != ESP_OK) {
                    failed = true;
                    break;
                }
                sha_update(data_to_write + offset, to_write);
                bytes_written_to_manifest += to_write;
                offset += to_write;
                s_recv.received_size += to_write;

                // manifest 写完，切换到 bin
                if (bytes_written_to_manifest == manifest_size) {
                    fclose(s_recv.stage_file);
                    s_recv.stage_file = NULL;

                    // 开始接收 bin
                    snprintf(s_recv.staged_path, sizeof(s_recv.staged_path), "/bootmedia/boot_block.bin.new");
                    s_recv.stage_file = fopen(s_recv.staged_path, "wb");
                    if (!s_recv.stage_file) {
                        failed = true;
                        break;
                    }
                    s_recv.kind = RECV_KIND_BOOTMEDIA_BIN;
                    notify_status(OTA_WIFI_STATE_RECEIVING, "bootmedia bin receiving", bytes_written_to_manifest, total_size);
                }
            } else {
                // 写 bin
                size_t to_write = data_len - offset;
                err = flash_safe_file_write(s_recv.stage_file, data_to_write + offset, to_write);
                if (err != ESP_OK) {
                    failed = true;
                    break;
                }
                sha_update(data_to_write + offset, to_write);
                bytes_written_to_bin += to_write;
                offset += to_write;
                s_recv.received_size += to_write;
            }
        }

        remaining -= received;
        report_progress();
    }

    if (s_recv.stage_file) {
        fclose(s_recv.stage_file);
        s_recv.stage_file = NULL;
    }
    free(recv_buf);
    if (decode_buf) free(decode_buf);

    if (failed || bytes_written_to_manifest != manifest_size || bytes_written_to_bin != bin_size) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia receive failed", received_size, total_size);
        return send_err(req, "receive failed");
    }

    if (!sha_finish_matches()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia sha mismatch", received_size, total_size);
        return send_err(req, "sha mismatch");
    }

    // 提交
    if (!boot_media_commit_incoming_update()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia commit failed", received_size, total_size);
        return send_err(req, "commit failed");
    }

    recv_reset();
    notify_status(OTA_WIFI_STATE_DONE, "bootmedia updated, rebooting", total_size, total_size);

    // 同 firmware_handler：先回成功响应再重启，避免 App 误报失败。
    send_json_response(req, 200, "{\"ok\":true,\"message\":\"bootmedia updated, rebooting\"}");

    // 延迟重启
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Idle watchdog: auto-stop after OTA_IDLE_TIMEOUT_US without activity */
/* ------------------------------------------------------------------ */

static void ota_timeout_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_state == OTA_WIFI_STATE_IDLE) {
            break;   // stopped externally
        }
        if (esp_timer_get_time() - s_last_activity_us > OTA_IDLE_TIMEOUT_US) {
            ESP_LOGW(TAG, "WiFi OTA idle timeout (no HTTP activity for %lld s), stopping",
                     (long long)(OTA_IDLE_TIMEOUT_US / 1000000));
            ota_wifi_server_stop();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

static void generate_wifi_password(void)
{
    // Unambiguous alphabet (no 0/O/1/l/I): 8 chars, still WPA2-PSK compliant.
    static const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    uint8_t rnd[8];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < WIFI_PASS_LEN; i++) {
        s_password[i] = charset[rnd[i] % (sizeof(charset) - 1)];
    }
    s_password[WIFI_PASS_LEN] = '\0';
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool ota_wifi_server_start(ota_wifi_info_t *info, ota_wifi_status_cb_t callback)
{
    if (s_state != OTA_WIFI_STATE_IDLE) {
        ESP_LOGW(TAG, "WiFi OTA already running");
        return false;
    }

    s_status_cb = callback;
    notify_status(OTA_WIFI_STATE_STARTING, "starting", 0, 0);
    rs485_brake_temp_pause();

    // 生成 SSID (带 MAC 后缀)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", WIFI_SSID_PREFIX, mac[4], mac[5]);

    // 固定密码，App 无需 BLE 即可加入 SoftAP。
    strncpy(s_password, OTA_WIFI_PASSWORD, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';

    // 生成 token
    uint8_t token_bytes[8];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    for (int i = 0; i < 8; i++) {
        snprintf(&s_token[i * 2], 3, "%02x", token_bytes[i]);
    }

    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_err_t err = wifi_runtime_prepare(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi runtime prepare failed: %s", esp_err_to_name(err));
        notify_status(OTA_WIFI_STATE_ERROR, "wifi runtime prepare failed", 0, 0);
        rs485_brake_temp_resume();
        wifi_runtime_cleanup(false);
        return false;
    }

    s_prev_wifi_mode = current_mode;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "ap netif create failed");
        notify_status(OTA_WIFI_STATE_ERROR, "ap netif create failed", 0, 0);
        rs485_brake_temp_resume();
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    wifi_mode_t target_mode = WIFI_MODE_AP;
    if (!s_wifi_owned) {
        if (current_mode == WIFI_MODE_STA) {
            target_mode = WIFI_MODE_APSTA;   // ESP-NOW active → add AP to the existing STA
        } else if (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
            target_mode = current_mode;
        }
    }

    if (s_wifi_owned) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi storage config failed: %s", esp_err_to_name(err));
            notify_status(OTA_WIFI_STATE_ERROR, "wifi storage config failed", 0, 0);
            rs485_brake_temp_resume();
            wifi_runtime_cleanup(false);
            return false;
        }
    }

    // 配置 SoftAP — channel 固定为 1，与 ESP-NOW 同信道，避免单射频切换信道打断 ESP-NOW
    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_CHANNEL,
            .max_connection = WIFI_MAX_STA,
            .authmode = WIFI_AUTH_WPA2_PSK,
            // PMF not required — OTA is a private single-client AP, beacon IEs are
        // kept minimal to reduce the beacon size and avoid internal-RAM allocation
        // failure under BLE+WiFi coexistence.
        .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)wifi_config.ap.password, s_password, sizeof(wifi_config.ap.password) - 1);

    err = esp_wifi_set_mode(target_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi mode set failed: %s", esp_err_to_name(err));
        notify_status(OTA_WIFI_STATE_ERROR, "wifi mode set failed", 0, 0);
        rs485_brake_temp_resume();
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi config failed: %s", esp_err_to_name(err));
        notify_status(OTA_WIFI_STATE_ERROR, "wifi config failed", 0, 0);
        rs485_brake_temp_resume();
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        if (!s_wifi_owned && (err == ESP_ERR_WIFI_STATE || err == ESP_ERR_INVALID_STATE)) {
            ESP_LOGD(TAG, "WiFi already started, reusing existing driver");
        } else {
            ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
            notify_status(OTA_WIFI_STATE_ERROR, "wifi start failed", 0, 0);
            rs485_brake_temp_resume();
            wifi_runtime_cleanup(!s_wifi_owned);
            return false;
        }
    }

    // 启动 HTTP 服务器
    ESP_LOGI(TAG, "heap before httpd: internal=%lu / total=%lu, stack=%u caps=INTERNAL",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)OTA_WIFI_STACK_SIZE);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = OTA_WIFI_STACK_SIZE;
    // Keep the httpd task stack in internal RAM: flash/SPIFFS operations
    // disable cache, so any task that touches flash must not live in PSRAM.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.core_id = 1;                              // keep HTTP handler off the WiFi/BLE core
    config.recv_wait_timeout = 30;                   // allow slow phone WiFi association without premature close
    config.send_wait_timeout = 30;
    // Reduce httpd memory usage
    config.max_uri_handlers = 8;                     // we only need 6 handlers
    config.max_resp_headers = 4;                     // minimal headers

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s (internal free=%lu)",
                 esp_err_to_name(err),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        notify_status(OTA_WIFI_STATE_ERROR, "httpd start failed", 0, 0);
        rs485_brake_temp_resume();
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    // 注册 handlers
    // Captive portal: phone OS probes "/" immediately after associating.
    // Must be registered first so httpd matches it before the catch-all 404.
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(s_httpd, &root_uri);

    httpd_uri_t discover_uri = {
        .uri = "/ota/discover",
        .method = HTTP_GET,
        .handler = discover_handler,
    };
    httpd_register_uri_handler(s_httpd, &discover_uri);

    httpd_uri_t info_uri = {
        .uri = "/ota/info",
        .method = HTTP_GET,
        .handler = info_handler,
    };
    httpd_register_uri_handler(s_httpd, &info_uri);

    httpd_uri_t status_uri = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    httpd_register_uri_handler(s_httpd, &status_uri);

    httpd_uri_t firmware_uri = {
        .uri = "/ota/firmware",
        .method = HTTP_POST,
        .handler = firmware_handler,
    };
    httpd_register_uri_handler(s_httpd, &firmware_uri);

    httpd_uri_t bootmedia_uri = {
        .uri = "/ota/bootmedia",
        .method = HTTP_POST,
        .handler = bootmedia_handler,
    };
    httpd_register_uri_handler(s_httpd, &bootmedia_uri);

    // 填充 info
    strncpy(info->ssid, s_ap_ssid, sizeof(info->ssid) - 1);
    strncpy(info->password, s_password, sizeof(info->password) - 1);
    strncpy(info->ip, "192.168.4.1", sizeof(info->ip) - 1);
    strncpy(info->token, s_token, sizeof(info->token) - 1);
    info->port = WIFI_PORT;

    // The caller sends the single final wifi-ready BLE notification after
    // receiving these generated credentials.  Do not emit an intermediate
    // READY callback here; it cannot contain the SSID/password and wastes a
    // BLE controller notification buffer.
    s_state = OTA_WIFI_STATE_READY;
    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WiFi OTA started: SSID=%s IP=%s", s_ap_ssid, info->ip);

    // 启动空闲超时 watchdog
    s_last_activity_us = esp_timer_get_time();
    xTaskCreate(ota_timeout_task, "ota_timeout", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    return true;
}

void ota_wifi_server_stop(void)
{
    if (s_state == OTA_WIFI_STATE_IDLE) return;

    recv_reset();

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    if (!s_wifi_owned && s_prev_wifi_mode != WIFI_MODE_NULL) {
        esp_err_t err = esp_wifi_set_mode(s_prev_wifi_mode);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore WiFi mode failed: %s", esp_err_to_name(err));
        }
    }

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    if (s_wifi_owned) {
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "wifi stop failed: %s", esp_err_to_name(err));
        }

        err = esp_wifi_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "wifi deinit failed: %s", esp_err_to_name(err));
        }
    }

    rs485_brake_temp_resume();

    // netif / event loop belong to other subsystems too (ESP-NOW) — do not delete.

    memset(s_token, 0, sizeof(s_token));
    memset(s_password, 0, sizeof(s_password));
    memset(s_ap_ssid, 0, sizeof(s_ap_ssid));
    s_status_cb = NULL;
    s_wifi_owned = false;
    s_prev_wifi_mode = WIFI_MODE_NULL;
    s_state = OTA_WIFI_STATE_IDLE;
    ESP_LOGI(TAG, "WiFi OTA stopped");
}

ota_wifi_state_t ota_wifi_server_get_state(void)
{
    return s_state;
}

bool ota_wifi_server_is_busy(void)
{
    return s_state == OTA_WIFI_STATE_RECEIVING;
}
