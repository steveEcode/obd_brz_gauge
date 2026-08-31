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
#include <errno.h>
#include <sys/socket.h>
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

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

#include "app_obd_dsp/boot_media_mount.h"
#include "app_obd_dsp/theme_mount.h"
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
// Socket receive staging buffer.  Kept in *internal* DRAM: httpd_req_recv()
// copies out of the lwIP pbufs on every call, and a PSRAM destination both
// costs extra bus cycles and contends with the multi-MB PSRAM accumulation
// buffer we are filling at the same time.  16 KB is plenty — recv() returns
// whatever the window holds, rarely more than a few KB at a time.  Falls back
// to PSRAM at OTA_RECV_CHUNK_PSRAM if internal RAM is tight.
#define OTA_RECV_CHUNK          262144     // internal-RAM staging buffer (256KB，进一步减少recv调用次数)
#define OTA_RECV_CHUNK_PSRAM    262144     // PSRAM fallback size
#define OTA_FLASH_WRITE_CHUNK 4096         // internal scratch buffer used for flash-safe writes
#define OTA_PROGRESS_STEP   (64 * 1024)    // report to BLE callback every 64KB
#define OTA_IDLE_TIMEOUT_US (10 * 60 * 1000000LL)  // auto-stop when no HTTP activity for 10 minutes
// HTTPD task stack size is measured in bytes. 4 KB is the default and enough
// for these handlers; flash operations must run from an internal-RAM task.
#define OTA_WIFI_STACK_SIZE 8192

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
    RECV_KIND_BOOTMEDIA,
    RECV_KIND_THEME,
} recv_kind_t;

typedef struct {
    recv_kind_t kind;
    uint32_t expected_size;
    uint32_t received_size;
    uint8_t expected_sha[32];
    mbedtls_sha256_context sha;
    bool sha_started;

    // The complete payload is accumulated in PSRAM.  No flash operation is
    // allowed while the HTTP upload is active; otherwise cache-disabled erase/
    // writes starve WiFi/lwIP and the phone appears to hang.
    uint8_t *upload_buf;

    // firmware
    const esp_partition_t *ota_partition;

    // bootmedia
    uint32_t manifest_size;
} ota_recv_t;

static ota_recv_t s_recv = {0};

/* ------------------------------------------------------------------ */
/* WiFi runtime: reuse the existing stack when present (ESP-NOW case) */
/* ------------------------------------------------------------------ */

/* Raise the WiFi RX buffer pool to throughput levels when internal DRAM
 * allows it.  Returns true if cfg was modified.
 *
 * Sizing: static_rx buffers are DMA-capable internal RAM (~1.6 KB each),
 * dynamic_rx are lwIP-side pbuf slots.  ESP-IDF's own throughput guidance is
 * static_rx=16 / dynamic_rx=32 / rx_ba_win=16, and requires
 * rx_ba_win <= 2*static_rx and rx_ba_win < dynamic_rx. */
static bool bump_wifi_rx_buffers(wifi_init_config_t *cfg)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // ~1.6 KB per static RX buffer + pbuf overhead for the dynamic ones.
    // Keep a 40 KB floor for httpd task stacks, lwIP control blocks and TLS-free
    // request state, otherwise esp_wifi_start()/hostap_attach fails on the
    // 752-byte beacon allocation.
    static const size_t RESERVE = 40 * 1024;
    if (internal_free < RESERVE + 40 * 1024) {
        ESP_LOGW(TAG, "internal heap %zu too low to raise WiFi RX buffers "
                      "(call ota_wifi_server_release_bt() first)", internal_free);
        return false;
    }

    size_t budget = internal_free - RESERVE;
    if (budget >= 100 * 1024) {
        cfg->static_rx_buf_num  = 16;
        cfg->dynamic_rx_buf_num = 32;
        cfg->rx_ba_win          = 16;
    } else if (budget >= 60 * 1024) {
        cfg->static_rx_buf_num  = 10;
        cfg->dynamic_rx_buf_num = 24;
        cfg->rx_ba_win          = 10;
    } else {
        cfg->static_rx_buf_num  = 8;
        cfg->dynamic_rx_buf_num = 16;
        cfg->rx_ba_win          = 8;
    }
    cfg->ampdu_rx_enable = 1;

    return true;
}

/* ------------------------------------------------------------------ */
/* Radio quiesce: hand the single 2.4GHz radio entirely to WiFi         */
/* ------------------------------------------------------------------ */

static bool s_bt_released = false;

void ota_wifi_server_release_bt(void)
{
    if (s_bt_released) {
        return;
    }
    s_bt_released = true;

    // Stop advertising first: tearing down bluedroid with an advert running
    // leaves the controller busy and makes the disable path return an error.
    esp_ble_gap_stop_advertising();
    esp_ble_gap_stop_scanning();
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = esp_bluedroid_disable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bluedroid disable: %s", esp_err_to_name(err));
    }
    err = esp_bluedroid_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bluedroid deinit: %s", esp_err_to_name(err));
    }
    err = esp_bt_controller_disable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bt controller disable: %s", esp_err_to_name(err));
    }
    err = esp_bt_controller_deinit();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bt controller deinit: %s", esp_err_to_name(err));
    }
    // Irreversible until reboot — which is exactly how every OTA flow ends.
    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bt mem release: %s", esp_err_to_name(err));
    }
}

/* Socket staging buffer — internal DRAM preferred, PSRAM as fallback. */
static char *alloc_recv_buf(size_t *out_size)
{
    char *buf = heap_caps_malloc(OTA_RECV_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf) {
        *out_size = OTA_RECV_CHUNK;
        return buf;
    }
    buf = heap_caps_malloc(OTA_RECV_CHUNK_PSRAM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *out_size = buf ? OTA_RECV_CHUNK_PSRAM : 0;
    return buf;
}

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

        // The project-wide Kconfig values (static_rx=4, dynamic_rx=8, rx_ba_win=6)
        // are sized for the *gauge* workload, where WiFi is idle or carrying
        // low-rate ESP-NOW frames and every spare byte of internal DRAM goes to
        // BLE + LVGL.  For a multi-MB upload they are catastrophic: 4 hardware RX
        // buffers cannot absorb a phone's TCP burst, and rx_ba_win(6) > static_rx(4)
        // means the AMPDU reorder window is wider than the buffer pool backing it,
        // so frames get dropped even on a clean link.  Every drop costs a full RTO.
        //
        // We only override when *we* own esp_wifi_init(), so ESP-NOW / normal boot
        // keep the memory-lean config.  Requires ota_wifi_server_release_bt() to
        // have run first, otherwise there is not enough internal DRAM.
        if (bump_wifi_rx_buffers(&cfg)) {
            err = esp_wifi_init(&cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi init with throughput buffers failed (%s), retrying with defaults",
                         esp_err_to_name(err));
                wifi_init_config_t fallback = WIFI_INIT_CONFIG_DEFAULT();
                err = esp_wifi_init(&fallback);
            }
        } else {
            err = esp_wifi_init(&cfg);
        }
        if (err != ESP_OK) {
            return err;
        }

        // Suppress verbose WiFi driver logs during OTA
        esp_log_level_set("wifi", ESP_LOG_WARN);
        esp_log_level_set("wifi_init", ESP_LOG_WARN);

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
    int ret = mbedtls_sha256_finish(&s_recv.sha, digest);
    mbedtls_sha256_free(&s_recv.sha);
    s_recv.sha_started = false;
    return ret == 0 && memcmp(digest, s_recv.expected_sha, 32) == 0;
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

static void recv_reset(void)
{
    if (s_recv.sha_started) {
        mbedtls_sha256_free(&s_recv.sha);
        s_recv.sha_started = false;
    }
    if (s_recv.upload_buf) {
        free(s_recv.upload_buf);
        s_recv.upload_buf = NULL;
    }
    memset(&s_recv, 0, sizeof(s_recv));
}

static bool allocate_upload_buffer(uint32_t size, const char *label)
{
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    size_t free_bytes = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    ESP_LOGI(TAG, "%s upload buffer: need=%lu, PSRAM free=%zu, largest=%zu",
             label, (unsigned long)size, free_bytes, largest);

    if (size == 0 || largest < size) {
        ESP_LOGE(TAG, "%s upload buffer does not fit: need=%lu, largest=%zu",
                 label, (unsigned long)size, largest);
        return false;
    }

    s_recv.upload_buf = heap_caps_malloc(size, caps);
    if (!s_recv.upload_buf) {
        ESP_LOGE(TAG, "%s upload buffer allocation failed: need=%lu, free=%zu, largest=%zu",
                 label, (unsigned long)size, free_bytes, largest);
        return false;
    }
    return true;
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
                      "X-OTA-Token,X-OTA-SHA256,X-OTA-Size,X-OTA-Manifest-Size,X-Offset,X-Last,Content-Type");
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

// GET /generate_204 — captive portal detection for Android/iOS
static esp_err_t generate_204_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// GET /connectivity-check — another captive portal detection path
static esp_err_t connectivity_check_handler(httpd_req_t *req)
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
// POST /ota/firmware — 分块上传
// App 端把固件拆成 256KB 小块逐块 POST，每块带：
//   X-Offset: 该块在整个数据流中的起始偏移
//   X-Last:   是否最后一块（1/0）
// HTTP 阶段只累计到 PSRAM，完整校验后才停止 WiFi 并写 OTA 分区。
static esp_err_t firmware_handler(httpd_req_t *req)
{
    int64_t handler_start_us = esp_timer_get_time();

    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char offset_str[16] = {0};
    char last_str[8] = {0};
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK) {
        return send_err(req, "missing headers");
    }

    bool is_base64 = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_base64 = (strstr(content_type, "base64") != NULL);
    }

    uint32_t expected_size = (uint32_t)strtoul(size_str, NULL, 10);
    uint32_t chunk_offset = 0;
    bool is_last = false;
    if (httpd_req_get_hdr_value_str(req, "X-Offset", offset_str, sizeof(offset_str)) == ESP_OK) {
        chunk_offset = (uint32_t)strtoul(offset_str, NULL, 10);
    }
    if (httpd_req_get_hdr_value_str(req, "X-Last", last_str, sizeof(last_str)) == ESP_OK) {
        is_last = (strcmp(last_str, "1") == 0);
    }

    uint8_t expected_sha[32];
    if (!parse_hex_sha(sha_hex, expected_sha) || expected_size == 0 || chunk_offset > expected_size) {
        return send_err(req, "invalid headers");
    }
    if (!is_base64 && (uint32_t)req->content_len > expected_size - chunk_offset) {
        return send_err(req, "invalid chunk size");
    }

    if (chunk_offset == 0) {
        ota_wifi_server_release_bt();
        recv_reset();
        s_recv.ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_recv.ota_partition) {
            return send_err(req, "no ota partition");
        }
        if (expected_size > s_recv.ota_partition->size) {
            return send_err(req, "firmware too large");
        }
        if (!allocate_upload_buffer(expected_size, "firmware")) {
            recv_reset();
            return send_err(req, "no memory for firmware buffer");
        }

        s_recv.kind = RECV_KIND_FIRMWARE;
        s_recv.expected_size = expected_size;
        s_recv.received_size = 0;
        memcpy(s_recv.expected_sha, expected_sha, sizeof(expected_sha));
        sha_init();
        notify_status(OTA_WIFI_STATE_RECEIVING, "firmware receiving (PSRAM)", 0, expected_size);
    } else if (s_recv.kind != RECV_KIND_FIRMWARE || !s_recv.upload_buf ||
               s_recv.expected_size != expected_size || s_recv.received_size != chunk_offset ||
               memcmp(s_recv.expected_sha, expected_sha, sizeof(expected_sha)) != 0) {
        ESP_LOGE(TAG, "firmware chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)chunk_offset, (unsigned long)s_recv.received_size, s_recv.kind);
        return send_err(req, "unexpected chunk offset");
    }

    int64_t after_init_us = esp_timer_get_time();
    ESP_LOGI(TAG, "firmware chunk begin: offset=%lu content_len=%d last=%d total=%lu (init +%lld ms)",
             (unsigned long)chunk_offset, req->content_len, is_last,
             (unsigned long)expected_size, (after_init_us - handler_start_us) / 1000);

    // 检查TCP连接状态
    int sock_fd = httpd_req_to_sockfd(req);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "firmware: invalid socket fd");
        recv_reset();
        return send_err(req, "invalid socket");
    }

    // 增大socket接收缓冲区，加速数据接收
    int rcvbuf_size = 256 * 1024;  // 256KB
    int ret = setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    if (ret == 0) {
        int actual_size = 0;
        socklen_t optlen = sizeof(actual_size);
        getsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &actual_size, &optlen);
        ESP_LOGI(TAG, "firmware: socket rcvbuf set to %d bytes (requested %d)", actual_size, rcvbuf_size);
    } else {
        ESP_LOGW(TAG, "firmware: failed to set socket rcvbuf: %d", errno);
    }

    // 禁用Nagle算法，减少延迟
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    size_t recv_cap = 0;
    char *recv_buf = alloc_recv_buf(&recv_cap);
    uint8_t *decode_buf = NULL;
    if (!recv_buf) {
        recv_reset();
        return send_err(req, "no memory");
    }
    if (is_base64) {
        decode_buf = heap_caps_malloc(recv_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decode_buf) {
            free(recv_buf);
            recv_reset();
            return send_err(req, "no memory");
        }
    }

    int remaining = req->content_len;
    bool failed = false;
    uint32_t chunk_received = 0;
    unsigned recv_timeouts = 0;
    unsigned consecutive_timeouts = 0;
    unsigned recv_calls = 0;

    int64_t recv_loop_start_us = esp_timer_get_time();
    int64_t last_recv_us = recv_loop_start_us;
    ESP_LOGI(TAG, "firmware: starting recv loop, content_len=%d recv_cap=%zu", req->content_len, recv_cap);

    while (remaining > 0 && !failed) {
        int to_read = remaining > (int)recv_cap ? (int)recv_cap : remaining;
        recv_calls++;

        int64_t before_recv_us = esp_timer_get_time();
        int64_t gap_us = before_recv_us - last_recv_us;
        int received = httpd_req_recv(req, recv_buf, to_read);
        int64_t after_recv_us = esp_timer_get_time();
        int64_t recv_duration_us = after_recv_us - before_recv_us;

        if (recv_calls <= 2 || received > 16384 || recv_duration_us > 100000 || gap_us > 200000) {
            ESP_LOGI(TAG, "firmware recv #%u: to_read=%d received=%d remaining=%d duration=%lld us gap=%lld us",
                     recv_calls, to_read, received, remaining, recv_duration_us, gap_us);
        }

        last_recv_us = after_recv_us;

        if (recv_calls <= 5 || received > 8192) {
            ESP_LOGI(TAG, "firmware recv #%u: to_read=%d received=%d remaining=%d",
                     recv_calls, to_read, received, remaining);
        }

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            recv_timeouts++;
            consecutive_timeouts++;
            ESP_LOGW(TAG, "firmware recv timeout %u (consecutive %u): offset=%lu chunk_received=%lu remaining=%d duration=%lld us",
                     recv_timeouts, consecutive_timeouts, (unsigned long)chunk_offset,
                     (unsigned long)chunk_received, remaining, recv_duration_us);
            // Content-Length模式下，少量超时是正常的（网络抖动）
            // 但如果连续超时说明连接已断开
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "firmware recv timeout limit reached: total=%u consecutive=%u",
                         recv_timeouts, consecutive_timeouts);
                break;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "firmware recv failed: received=%d offset=%lu chunk_received=%lu remaining=%d",
                     received, (unsigned long)chunk_offset,
                     (unsigned long)chunk_received, remaining);
            failed = true;
            break;
        }
        // 成功接收数据，重置连续超时计数器
        consecutive_timeouts = 0;

        const uint8_t *data;
        size_t data_len;
        if (is_base64) {
            size_t decode_len = 0;
            int ret = mbedtls_base64_decode(decode_buf, recv_cap, &decode_len,
                                            (const unsigned char *)recv_buf, received);
            if (ret != 0) {
                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                failed = true;
                break;
            }
            data = decode_buf;
            data_len = decode_len;
        } else {
            data = (const uint8_t *)recv_buf;
            data_len = received;
        }

        if (data_len > expected_size - chunk_offset - chunk_received) {
            ESP_LOGE(TAG, "firmware decoded chunk exceeds payload: offset=%lu got=%zu total=%lu",
                     (unsigned long)(chunk_offset + chunk_received), data_len,
                     (unsigned long)expected_size);
            failed = true;
            break;
        }
        memcpy(s_recv.upload_buf + chunk_offset + chunk_received, data, data_len);
        sha_update(data, data_len);
        chunk_received += data_len;
        remaining -= received;
        s_recv.received_size = chunk_offset + chunk_received;
        // 只在64KB边界报告进度，避免频繁回调阻塞接收循环
        if (s_recv.received_size % OTA_PROGRESS_STEP == 0) {
            report_progress();
        }
    }
    free(recv_buf);
    if (decode_buf) free(decode_buf);

    int64_t recv_loop_end_us = esp_timer_get_time();
    int64_t recv_duration_ms = (recv_loop_end_us - recv_loop_start_us) / 1000;

    if (!failed) {
        ESP_LOGI(TAG, "firmware chunk complete: offset=%lu len=%lu next=%lu last=%d (recv took %lld ms, %u calls)",
                 (unsigned long)chunk_offset, (unsigned long)chunk_received,
                 (unsigned long)s_recv.received_size, is_last, recv_duration_ms, recv_calls);
    }

    if (failed) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware receive failed", received_size, expected_size);
        return send_err(req, "receive failed");
    }

    if (!is_last && s_recv.received_size < s_recv.expected_size) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":true,\"nextOffset\":%lu}",
                 (unsigned long)s_recv.received_size);

        int64_t before_response_us = esp_timer_get_time();
        esp_err_t ret = send_json_response(req, 200, json);
        int64_t after_response_us = esp_timer_get_time();

        ESP_LOGI(TAG, "firmware: intermediate response sent in %lld ms (handler total %lld ms)",
                 (after_response_us - before_response_us) / 1000,
                 (after_response_us - handler_start_us) / 1000);
        return ret;
    }

    if (s_recv.received_size != s_recv.expected_size) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware size mismatch", received_size, expected_size);
        return send_err(req, "size mismatch");
    }
    if (!sha_finish_matches()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "firmware sha mismatch", received_size, expected_size);
        return send_err(req, "sha mismatch");
    }

    // The network phase is complete. Acknowledge it first, then let the TCP
    // response leave the radio before starting any cache-disabled flash work.
    notify_status(OTA_WIFI_STATE_RECEIVING, "firmware installing", expected_size, expected_size);
    esp_err_t response_err = send_json_response(
        req, 200, "{\"ok\":true,\"message\":\"firmware received, installing\"}");
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "firmware final response failed: %s", esp_err_to_name(response_err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(s_recv.ota_partition, expected_size, &ota_handle);
    if (err == ESP_OK) {
        err = flash_safe_ota_write(ota_handle, s_recv.upload_buf, expected_size);
    }
    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
        ota_handle = 0;
    } else if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(s_recv.ota_partition);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "firmware install complete, rebooting");
        notify_status(OTA_WIFI_STATE_DONE, "firmware updated, rebooting", expected_size, expected_size);
    } else {
        ESP_LOGE(TAG, "firmware install failed after upload: %s; rebooting old firmware",
                 esp_err_to_name(err));
        notify_status(OTA_WIFI_STATE_ERROR, "firmware install failed", expected_size, expected_size);
    }
    recv_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// POST /ota/theme/prepare
// App 在上传分块之前调用一次，仅验证分区容量和 PSRAM。
// 这里绝不擦除 Flash，旧主题会保留到新数据完整校验通过。
static esp_err_t theme_prepare_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    if (!theme_mount()) {
        return send_err(req, "mount failed");
    }

    char size_str[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK) {
        return send_err(req, "missing prepare size");
    }

    uint32_t total_size = (uint32_t)strtoul(size_str, NULL, 10);
    if (total_size == 0) {
        return send_err(req, "invalid prepare size");
    }

    size_t partition_size = theme_get_partition_size();
    if (total_size > partition_size) {
        char err_msg[80];
        snprintf(err_msg, sizeof(err_msg), "theme too large: partition=%zu need=%lu",
                 partition_size, (unsigned long)total_size);
        return send_err(req, err_msg);
    }

    // Deliberately do NOT erase here. A multi-megabyte erase in an HTTP handler
    // blocks the ESP32 flash cache long enough to collapse the SoftAP TCP connection.
    // Keeping the old theme also makes an interrupted upload non-destructive.
    // Erase happens only after full SHA verify.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "theme prepare: total=%lu partition=%zu PSRAM free=%zu largest=%zu",
             (unsigned long)total_size, partition_size, psram_free, largest);
    if (largest < total_size) {
        return send_err(req, "no memory for theme buffer");
    }

    // Consume the tiny placeholder body so a keep-alive socket cannot carry
    // unread prepare data into the first upload request.
    char discard[32];
    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, discard, remaining > (int)sizeof(discard) ? (int)sizeof(discard) : remaining);
        if (received <= 0) {
            return send_err(req, "prepare receive failed");
        }
        remaining -= received;
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"partitionSize\":%zu,\"psramFree\":%zu,\"largestBlock\":%zu}",
             partition_size, psram_free, largest);
    return send_json_response(req, 200, json);
}

// POST /ota/theme — 分块上传 theme.bin (一个自包含的 4MB blob，和 firmware 一样
// 没有 manifest/media 分段，写入前整块擦除 theme_0，再一次性写入)。
// 分块协议与 firmware_handler 完全一致：X-OTA-SHA256/X-OTA-Size/X-Offset/X-Last。
static esp_err_t theme_handler(httpd_req_t *req)
{
    int64_t handler_start_us = esp_timer_get_time();

    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char offset_str[16] = {0};
    char last_str[8] = {0};
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK) {
        return send_err(req, "missing headers");
    }

    bool is_base64 = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_base64 = (strstr(content_type, "base64") != NULL);
    }

    uint32_t expected_size = (uint32_t)strtoul(size_str, NULL, 10);
    uint32_t chunk_offset = 0;
    bool is_last = false;
    if (httpd_req_get_hdr_value_str(req, "X-Offset", offset_str, sizeof(offset_str)) == ESP_OK) {
        chunk_offset = (uint32_t)strtoul(offset_str, NULL, 10);
    }
    if (httpd_req_get_hdr_value_str(req, "X-Last", last_str, sizeof(last_str)) == ESP_OK) {
        is_last = (strcmp(last_str, "1") == 0);
    }

    uint8_t expected_sha[32];
    if (!parse_hex_sha(sha_hex, expected_sha) || expected_size == 0 || chunk_offset > expected_size) {
        return send_err(req, "invalid headers");
    }
    if (!is_base64 && (uint32_t)req->content_len > expected_size - chunk_offset) {
        return send_err(req, "invalid chunk size");
    }

    if (chunk_offset == 0) {
        ota_wifi_server_release_bt();
        recv_reset();
        if (!theme_mount()) {
            return send_err(req, "mount failed");
        }
        if (expected_size > theme_get_partition_size()) {
            return send_err(req, "theme too large");
        }
        if (!allocate_upload_buffer(expected_size, "theme")) {
            recv_reset();
            return send_err(req, "no memory for theme buffer");
        }

        s_recv.kind = RECV_KIND_THEME;
        s_recv.expected_size = expected_size;
        s_recv.received_size = 0;
        memcpy(s_recv.expected_sha, expected_sha, sizeof(expected_sha));
        sha_init();
        notify_status(OTA_WIFI_STATE_RECEIVING, "theme receiving (PSRAM)", 0, expected_size);
    } else if (s_recv.kind != RECV_KIND_THEME || !s_recv.upload_buf ||
               s_recv.expected_size != expected_size || s_recv.received_size != chunk_offset ||
               memcmp(s_recv.expected_sha, expected_sha, sizeof(expected_sha)) != 0) {
        ESP_LOGE(TAG, "theme chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)chunk_offset, (unsigned long)s_recv.received_size, s_recv.kind);
        return send_err(req, "unexpected chunk offset");
    }

    int64_t after_init_us = esp_timer_get_time();
    ESP_LOGI(TAG, "theme chunk begin: offset=%lu content_len=%d last=%d total=%lu (init +%lld ms)",
             (unsigned long)chunk_offset, req->content_len, is_last,
             (unsigned long)expected_size, (after_init_us - handler_start_us) / 1000);

    int sock_fd = httpd_req_to_sockfd(req);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "theme: invalid socket fd");
        recv_reset();
        return send_err(req, "invalid socket");
    }

    int rcvbuf_size = 256 * 1024;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    size_t recv_cap = 0;
    char *recv_buf = alloc_recv_buf(&recv_cap);
    uint8_t *decode_buf = NULL;
    if (!recv_buf) {
        recv_reset();
        return send_err(req, "no memory");
    }
    if (is_base64) {
        decode_buf = heap_caps_malloc(recv_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decode_buf) {
            free(recv_buf);
            recv_reset();
            return send_err(req, "no memory");
        }
    }

    int remaining = req->content_len;
    bool failed = false;
    uint32_t chunk_received = 0;
    unsigned consecutive_timeouts = 0;

    while (remaining > 0 && !failed) {
        int to_read = remaining > (int)recv_cap ? (int)recv_cap : remaining;
        int received = httpd_req_recv(req, recv_buf, to_read);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            consecutive_timeouts++;
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "theme recv timeout limit reached");
                break;
            }
            continue;
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "theme recv failed: received=%d offset=%lu chunk_received=%lu remaining=%d",
                     received, (unsigned long)chunk_offset,
                     (unsigned long)chunk_received, remaining);
            failed = true;
            break;
        }
        consecutive_timeouts = 0;

        const uint8_t *data;
        size_t data_len;
        if (is_base64) {
            size_t decode_len = 0;
            int ret = mbedtls_base64_decode(decode_buf, recv_cap, &decode_len,
                                            (const unsigned char *)recv_buf, received);
            if (ret != 0) {
                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                failed = true;
                break;
            }
            data = decode_buf;
            data_len = decode_len;
        } else {
            data = (const uint8_t *)recv_buf;
            data_len = received;
        }

        if (data_len > expected_size - chunk_offset - chunk_received) {
            ESP_LOGE(TAG, "theme decoded chunk exceeds payload");
            failed = true;
            break;
        }
        memcpy(s_recv.upload_buf + chunk_offset + chunk_received, data, data_len);
        sha_update(data, data_len);
        chunk_received += data_len;
        remaining -= received;
        s_recv.received_size = chunk_offset + chunk_received;
        if (s_recv.received_size % OTA_PROGRESS_STEP == 0) {
            report_progress();
        }
    }
    free(recv_buf);
    if (decode_buf) free(decode_buf);

    if (failed) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "theme receive failed", received_size, expected_size);
        return send_err(req, "receive failed");
    }

    if (!is_last && s_recv.received_size < s_recv.expected_size) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":true,\"nextOffset\":%lu}",
                 (unsigned long)s_recv.received_size);
        return send_json_response(req, 200, json);
    }

    if (s_recv.received_size != s_recv.expected_size) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "theme size mismatch", received_size, expected_size);
        return send_err(req, "size mismatch");
    }
    if (!sha_finish_matches()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "theme sha mismatch", received_size, expected_size);
        return send_err(req, "sha mismatch");
    }

    // Network phase complete -- acknowledge before any flash work so the
    // TCP response leaves the radio before SoftAP is torn down.
    notify_status(OTA_WIFI_STATE_RECEIVING, "theme installing", expected_size, expected_size);
    esp_err_t response_err = send_json_response(
        req, 200, "{\"ok\":true,\"message\":\"theme received, installing\"}");
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "theme final response failed: %s", esp_err_to_name(response_err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = theme_erase_all() ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        err = theme_raw_write(0, s_recv.upload_buf, expected_size);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "theme install complete, rebooting");
        notify_status(OTA_WIFI_STATE_DONE, "theme updated, rebooting", expected_size, expected_size);
    } else {
        ESP_LOGE(TAG, "theme install failed after upload: %s", esp_err_to_name(err));
        notify_status(OTA_WIFI_STATE_ERROR, "theme install failed", expected_size, expected_size);
    }
    recv_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// POST /ota/bootmedia/prepare
// App 在上传分块之前调用一次，仅验证分区容量和 PSRAM。
// 这里绝不擦除 Flash，旧动画会保留到新数据完整校验通过。
static esp_err_t bootmedia_prepare_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    if (!boot_media_mount()) {
        return send_err(req, "mount failed");
    }

    char total_str[16] = {0};
    char manifest_str[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Size", total_str, sizeof(total_str)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Manifest-Size", manifest_str, sizeof(manifest_str)) != ESP_OK) {
        return send_err(req, "missing prepare sizes");
    }

    uint32_t total_size = (uint32_t)strtoul(total_str, NULL, 10);
    uint32_t manifest_size = (uint32_t)strtoul(manifest_str, NULL, 10);
    if (total_size == 0 || manifest_size == 0 || manifest_size >= total_size ||
        manifest_size > boot_media_raw_manifest_max()) {
        return send_err(req, "invalid prepare sizes");
    }

    size_t free_space = boot_media_get_free_space();
    const size_t needed_space = (size_t)(total_size - manifest_size);
    if (free_space < needed_space) {
        char err_msg[80];
        snprintf(err_msg, sizeof(err_msg), "bootmedia too large: free=%zu need=%zu",
                 free_space, needed_space);
        return send_err(req, err_msg);
    }

    // Deliberately do NOT invalidate or erase here. A multi-megabyte erase in
    // an HTTP handler blocks the ESP32 flash cache long enough to collapse the
    // SoftAP TCP connection. Keeping the old manifest also makes an interrupted
    // upload non-destructive. Erase/commit happens only after full SHA verify.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "bootmedia prepare: total=%lu manifest=%lu PSRAM free=%zu largest=%zu",
             (unsigned long)total_size, (unsigned long)manifest_size, psram_free, largest);
    if (largest < total_size) {
        return send_err(req, "no memory for bootmedia buffer");
    }

    // Consume the tiny placeholder body so a keep-alive socket cannot carry
    // unread prepare data into the first upload request.
    char discard[32];
    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, discard, remaining > (int)sizeof(discard) ? (int)sizeof(discard) : remaining);
        if (received <= 0) {
            return send_err(req, "prepare receive failed");
        }
        remaining -= received;
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"free\":%zu,\"psramFree\":%zu,\"largestBlock\":%zu}",
             free_space, psram_free, largest);
    return send_json_response(req, 200, json);
}

// POST /ota/bootmedia — 分块上传
// App 端把 [manifest][bin] 整体拆成小块逐块 POST，每块带：
//   X-Offset: 该块在整个数据流中的起始偏移
//   X-Last:   是否最后一块（1/0）
// 收齐全部块后校验 SHA-256 并提交、重启。
static esp_err_t bootmedia_handler(httpd_req_t *req)
{
    int64_t handler_start_us = esp_timer_get_time();

    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char manifest_size_str[16] = {0};
    char offset_str[16] = {0};
    char last_str[8] = {0};
    char content_type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_str, sizeof(size_str)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Manifest-Size", manifest_size_str, sizeof(manifest_size_str)) != ESP_OK) {
        return send_err(req, "missing headers");
    }

    bool is_base64 = false;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        is_base64 = (strstr(content_type, "base64") != NULL);
    }

    uint32_t total_size = (uint32_t)strtoul(size_str, NULL, 10);
    uint32_t manifest_size = (uint32_t)strtoul(manifest_size_str, NULL, 10);
    uint32_t chunk_offset = 0;
    bool is_last = false;
    if (httpd_req_get_hdr_value_str(req, "X-Offset", offset_str, sizeof(offset_str)) == ESP_OK) {
        chunk_offset = (uint32_t)strtoul(offset_str, NULL, 10);
    }
    if (httpd_req_get_hdr_value_str(req, "X-Last", last_str, sizeof(last_str)) == ESP_OK) {
        is_last = (strcmp(last_str, "1") == 0);
    }

    if (total_size == 0 || manifest_size == 0 || manifest_size > boot_media_raw_manifest_max() ||
        manifest_size >= total_size || chunk_offset > total_size ||
        (!is_base64 && (uint32_t)req->content_len > total_size - chunk_offset)) {
        ESP_LOGE(TAG, "invalid sizes/offset: total=%lu manifest=%lu offset=%lu content_len=%d",
                 (unsigned long)total_size, (unsigned long)manifest_size,
                 (unsigned long)chunk_offset, req->content_len);
        return send_err(req, "invalid sizes");
    }
    uint8_t expected_sha[32];
    if (!parse_hex_sha(sha_hex, expected_sha)) {
        return send_err(req, "invalid sha");
    }

    if (chunk_offset == 0) {
        ota_wifi_server_release_bt();
        recv_reset();
        if (!boot_media_mount()) {
            return send_err(req, "mount failed");
        }
        size_t free_space = boot_media_get_free_space();
        size_t needed_space = (size_t)(total_size - manifest_size);
        if (free_space < needed_space) {
            return send_err(req, "bootmedia too large");
        }
        if (!allocate_upload_buffer(total_size, "bootmedia")) {
            recv_reset();
            return send_err(req, "no memory for bootmedia buffer");
        }

        s_recv.kind = RECV_KIND_BOOTMEDIA;
        s_recv.expected_size = total_size;
        s_recv.received_size = 0;
        s_recv.manifest_size = manifest_size;
        memcpy(s_recv.expected_sha, expected_sha, sizeof(expected_sha));
        sha_init();
        notify_status(OTA_WIFI_STATE_RECEIVING, "bootmedia receiving (PSRAM)", 0, total_size);
    } else if (s_recv.kind != RECV_KIND_BOOTMEDIA || !s_recv.upload_buf ||
               s_recv.expected_size != total_size || s_recv.manifest_size != manifest_size ||
               s_recv.received_size != chunk_offset ||
               memcmp(s_recv.expected_sha, expected_sha, sizeof(expected_sha)) != 0) {
        ESP_LOGE(TAG, "bootmedia chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)chunk_offset, (unsigned long)s_recv.received_size, s_recv.kind);
        return send_err(req, "unexpected chunk offset");
    }

    ESP_LOGI(TAG, "bootmedia chunk begin: offset=%lu content_len=%d last=%d total=%lu",
             (unsigned long)chunk_offset, req->content_len, is_last,
             (unsigned long)total_size);

    // 检查TCP连接状态
    int sock_fd = httpd_req_to_sockfd(req);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "bootmedia: invalid socket fd");
        recv_reset();
        return send_err(req, "invalid socket");
    }

    // 增大socket接收缓冲区，加速数据接收
    int rcvbuf_size = 256 * 1024;  // 256KB
    int ret = setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size));
    if (ret == 0) {
        int actual_size = 0;
        socklen_t optlen = sizeof(actual_size);
        getsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &actual_size, &optlen);
        ESP_LOGI(TAG, "bootmedia: socket rcvbuf set to %d bytes (requested %d)", actual_size, rcvbuf_size);
    } else {
        ESP_LOGW(TAG, "bootmedia: failed to set socket rcvbuf: %d", errno);
    }

    // 禁用Nagle算法，减少延迟
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    size_t recv_cap = 0;
    char *recv_buf = alloc_recv_buf(&recv_cap);
    uint8_t *decode_buf = NULL;
    if (!recv_buf) {
        recv_reset();
        return send_err(req, "no memory");
    }
    if (is_base64) {
        decode_buf = heap_caps_malloc(recv_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!decode_buf) {
            free(recv_buf);
            recv_reset();
            return send_err(req, "no memory");
        }
    }

    int remaining = req->content_len;
    bool failed = false;
    uint32_t chunk_received = 0;
    unsigned recv_timeouts = 0;
    unsigned consecutive_timeouts = 0;
    unsigned recv_calls = 0;

    int64_t recv_loop_start_us = esp_timer_get_time();
    int64_t last_recv_us = recv_loop_start_us;
    ESP_LOGI(TAG, "bootmedia: starting recv loop, content_len=%d recv_cap=%zu", req->content_len, recv_cap);

    while (remaining > 0 && !failed) {
        int to_read = remaining > (int)recv_cap ? (int)recv_cap : remaining;
        recv_calls++;

        int64_t before_recv_us = esp_timer_get_time();
        int64_t gap_us = before_recv_us - last_recv_us;
        int received = httpd_req_recv(req, recv_buf, to_read);
        int64_t after_recv_us = esp_timer_get_time();
        int64_t recv_duration_us = after_recv_us - before_recv_us;

        if (recv_calls <= 2 || received > 16384 || recv_duration_us > 100000 || gap_us > 200000) {
            ESP_LOGI(TAG, "bootmedia recv #%u: to_read=%d received=%d remaining=%d duration=%lld us gap=%lld us",
                     recv_calls, to_read, received, remaining, recv_duration_us, gap_us);
        }

        last_recv_us = after_recv_us;

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            recv_timeouts++;
            consecutive_timeouts++;
            ESP_LOGW(TAG, "bootmedia recv timeout %u (consecutive %u): offset=%lu chunk_received=%lu remaining=%d duration=%lld us",
                     recv_timeouts, consecutive_timeouts, (unsigned long)chunk_offset,
                     (unsigned long)chunk_received, remaining, recv_duration_us);
            // Content-Length模式下，少量超时是正常的（网络抖动）
            // 但如果连续超时说明连接已断开
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "bootmedia recv timeout limit reached: total=%u consecutive=%u",
                         recv_timeouts, consecutive_timeouts);
                break;
            }
            continue;
        }

        if (received > 0 && chunk_received % 8192 == 0) {
            ESP_LOGD(TAG, "bootmedia: recv progress: chunk_received=%lu remaining=%d received=%d",
                     (unsigned long)chunk_received, remaining, received);
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "bootmedia recv failed: received=%d offset=%lu chunk_received=%lu remaining=%d",
                     received, (unsigned long)chunk_offset,
                     (unsigned long)chunk_received, remaining);
            failed = true;
            break;
        }
        // 成功接收数据，重置连续超时计数器
        consecutive_timeouts = 0;

        const uint8_t *data;
        size_t data_len;
        if (is_base64) {
            size_t decode_len = 0;
            int ret = mbedtls_base64_decode(decode_buf, recv_cap, &decode_len,
                                            (const unsigned char *)recv_buf, received);
            if (ret != 0) {
                ESP_LOGE(TAG, "base64 decode failed: %d", ret);
                failed = true;
                break;
            }
            data = decode_buf;
            data_len = decode_len;
        } else {
            data = (const uint8_t *)recv_buf;
            data_len = received;
        }

        if (data_len > total_size - chunk_offset - chunk_received) {
            ESP_LOGE(TAG, "bootmedia decoded chunk exceeds payload");
            failed = true;
            break;
        }
        memcpy(s_recv.upload_buf + chunk_offset + chunk_received, data, data_len);
        sha_update(data, data_len);
        chunk_received += data_len;
        remaining -= received;
        s_recv.received_size = chunk_offset + chunk_received;
        // 只在64KB边界报告进度，避免频繁回调阻塞接收循环
        if (s_recv.received_size % OTA_PROGRESS_STEP == 0) {
            report_progress();
        }
    }
    free(recv_buf);
    if (decode_buf) free(decode_buf);

    int64_t recv_loop_end_us = esp_timer_get_time();
    int64_t recv_duration_ms = (recv_loop_end_us - recv_loop_start_us) / 1000;

    if (!failed) {
        ESP_LOGI(TAG, "bootmedia chunk complete: offset=%lu len=%lu next=%lu last=%d (recv took %lld ms, %u calls)",
                 (unsigned long)chunk_offset, (unsigned long)chunk_received,
                 (unsigned long)s_recv.received_size, is_last, recv_duration_ms, recv_calls);
    }

    if (failed) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia receive failed", received_size, total_size);
        return send_err(req, "receive failed");
    }

    if (!is_last && s_recv.received_size < total_size) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":true,\"nextOffset\":%lu}",
                 (unsigned long)s_recv.received_size);

        int64_t before_response_us = esp_timer_get_time();
        esp_err_t ret = send_json_response(req, 200, json);
        int64_t after_response_us = esp_timer_get_time();

        ESP_LOGI(TAG, "bootmedia: intermediate response sent in %lld ms (handler total %lld ms), ret=%d",
                 (after_response_us - before_response_us) / 1000,
                 (after_response_us - handler_start_us) / 1000, ret);
        return ret;
    }
    if (s_recv.received_size != total_size) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia size mismatch", received_size, total_size);
        return send_err(req, "size mismatch");
    }
    if (!sha_finish_matches()) {
        uint32_t received_size = s_recv.received_size;
        recv_reset();
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia sha mismatch", received_size, total_size);
        return send_err(req, "sha mismatch");
    }

    notify_status(OTA_WIFI_STATE_RECEIVING, "bootmedia installing", total_size, total_size);
    esp_err_t response_err = send_json_response(
        req, 200, "{\"ok\":true,\"message\":\"bootmedia received, installing\"}");
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "bootmedia final response failed: %s", esp_err_to_name(response_err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t write_err = ESP_OK;
    if (!boot_media_invalidate_manifest()) {
        write_err = ESP_FAIL;
    }
    if (write_err == ESP_OK && !boot_media_erase_data_region(total_size, manifest_size)) {
        write_err = ESP_FAIL;
    }
    // Commit the manifest last. If power is lost while the binary is being
    // written, the erased manifest keeps the incomplete animation invisible.
    if (write_err == ESP_OK) {
        write_err = boot_media_raw_write(manifest_size, s_recv.upload_buf + manifest_size,
                                         total_size - manifest_size, manifest_size);
    }
    if (write_err == ESP_OK) {
        write_err = boot_media_raw_write(0, s_recv.upload_buf, manifest_size, manifest_size);
    }

    if (write_err == ESP_OK) {
        ESP_LOGI(TAG, "bootmedia install complete, rebooting");
        notify_status(OTA_WIFI_STATE_DONE, "bootmedia updated, rebooting", total_size, total_size);
    } else {
        ESP_LOGE(TAG, "bootmedia install failed after upload: %s", esp_err_to_name(write_err));
        notify_status(OTA_WIFI_STATE_ERROR, "bootmedia install failed", total_size, total_size);
    }
    recv_reset();
    vTaskDelay(pdMS_TO_TICKS(300));
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
    // 设置 AP 空闲超时时间为 600 秒，防止手机因网络检测而断开连接
    esp_wifi_set_inactive_time(WIFI_IF_AP, 600);

    // Modem sleep costs airtime on a link that is about to carry several MB.
    // The default is WIFI_PS_MIN_MODEM; only espnow_link_start() ever disabled
    // it, so the OTA path was running with power save on.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "disable power save failed: %s", esp_err_to_name(ps_err));
    }
    // Max legal TX power (unit is 0.25 dBm) — fewer PHY errors means fewer
    // retransmits, and every retransmit costs an RTO on this link.
    esp_wifi_set_max_tx_power(80);

    // 启动 HTTP 服务器
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = OTA_WIFI_STACK_SIZE;
    // Keep the httpd task stack in internal RAM: flash/SPIFFS operations
    // disable cache, so any task that touches flash must not live in PSRAM.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.core_id = 1;                              // keep HTTP handler off the WiFi/BLE core
    config.recv_wait_timeout = 2;                  // 2s per recv call, Content-Length模式下不应该超时
    config.send_wait_timeout = 300;
    // Reduce httpd memory usage
    config.max_uri_handlers = 16;                    // 13 handlers (incl. OPTIONS preflight for POST routes)
    config.max_resp_headers = 4;                     // minimal headers
    config.max_open_sockets = 7;                     // max allowed by LWIP_MAX_SOCKETS (10 - 3 internal)
    config.backlog_conn = 5;                         // accept队列长度
    config.lru_purge_enable = true;                  // 启用LRU清理，自动关闭空闲连接

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
    httpd_uri_t generate_204_uri = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = generate_204_handler,
    };
    httpd_register_uri_handler(s_httpd, &generate_204_uri);

    httpd_uri_t connectivity_check_uri = {
        .uri = "/connectivity-check",
        .method = HTTP_GET,
        .handler = connectivity_check_handler,
    };
    httpd_register_uri_handler(s_httpd, &connectivity_check_uri);

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

    // CORS preflight: browsers send OPTIONS before POST when custom headers are used.
    // Without these, the WebView fetch preflight fails and the upload is blocked.
    httpd_uri_t firmware_opt_uri = {
        .uri = "/ota/firmware",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_httpd, &firmware_opt_uri);

    httpd_uri_t bootmedia_prepare_uri = {
        .uri = "/ota/bootmedia/prepare",
        .method = HTTP_POST,
        .handler = bootmedia_prepare_handler,
    };
    httpd_register_uri_handler(s_httpd, &bootmedia_prepare_uri);

    httpd_uri_t bootmedia_prepare_opt_uri = {
        .uri = "/ota/bootmedia/prepare",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_httpd, &bootmedia_prepare_opt_uri);

    httpd_uri_t bootmedia_uri = {
        .uri = "/ota/bootmedia",
        .method = HTTP_POST,
        .handler = bootmedia_handler,
    };
    httpd_register_uri_handler(s_httpd, &bootmedia_uri);

    httpd_uri_t bootmedia_opt_uri = {
        .uri = "/ota/bootmedia",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_httpd, &bootmedia_opt_uri);

    httpd_uri_t theme_prepare_uri = {
        .uri = "/ota/theme/prepare",
        .method = HTTP_POST,
        .handler = theme_prepare_handler,
    };
    httpd_register_uri_handler(s_httpd, &theme_prepare_uri);

    httpd_uri_t theme_prepare_opt_uri = {
        .uri = "/ota/theme/prepare",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_httpd, &theme_prepare_opt_uri);

    httpd_uri_t theme_uri = {
        .uri = "/ota/theme",
        .method = HTTP_POST,
        .handler = theme_handler,
    };
    httpd_register_uri_handler(s_httpd, &theme_uri);

    httpd_uri_t theme_opt_uri = {
        .uri = "/ota/theme",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
    };
    httpd_register_uri_handler(s_httpd, &theme_opt_uri);

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
