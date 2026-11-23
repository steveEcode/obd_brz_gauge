// ota_wifi_server.c — WiFi OTA server (SoftAP + HTTP)
//
// Started from the OTA mode screen: the phone joins the SoftAP, fetches the
// session token from GET /ota/discover and POSTs firmware/bootmedia/theme over
// HTTP. The same server hosts the device web page (GET /) and the token-gated
// /api/v1/* device API (api_v1_handlers.c).
//
// Route policy:
//   /ota/*     frozen App protocol (payloads, status codes, CORS headers)
//   /api/v1/*  X-OTA-Token required, vehicle POST CORS allowlist, {"ok":false,...} errors
//   /          gzipped device page when Accept has text/html, else 204
//
// Coexistence with ESP-NOW (master/slave multi-gauge link):
//   ESP-NOW runs on the STA interface in WIFI_MODE_STA on channel 1. This module
//   NEVER deinitialises WiFi/netif/event-loop that it did not create. When the
//   WiFi stack already exists it switches to APSTA on channel 1, starts the AP,
//   and on stop merely restores the previous mode — leaving the ESP-NOW driver,
//   peers and channel untouched.
#include "ota_wifi_server.h"
#include "ota_idle_tracker.h"
#include "http_u32.h"
#include "vehicle_web_cors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "app_obd_dsp/api_v1_handlers.h"
#include "app_obd_dsp/boot_media_mount.h"
#include "app_obd_dsp/coredump_store.h"
#include "app_obd_dsp/device_identity.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/theme_bundle.h"
#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/sd_io_guard.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/theme_mount.h"
#include "app_obd_dsp/upload_sink.h"
#include "bsp_obd_dsp/ble_hub.h"
#include "bsp_obd_dsp/sd_card.h"
#include "vehicle/vp_manager.h"
#include "media/video_elem.h"
#include "media/boot_obv_validate.h"
#include "media/video_player.h"
#include "theme_engine/theme_interface.h"
#include "freertos/semphr.h"

static const char *TAG = "ota_wifi";
extern SemaphoreHandle_t lvgl_mux;

#define WIFI_SSID_PREFIX    "OBD-Gauge-OTA-"
#define WIFI_PORT           80
#define WIFI_MAX_STA        1
#define WIFI_CHANNEL        1              // match ESP-NOW channel: single-radio AP+STA must share a channel
// The phone joins the SoftAP manually (password is fixed) then fetches the
// per-session token from GET /ota/discover.  Write endpoints require the token.
#define OTA_WIFI_PASSWORD   "88888888"
#define OTA_IDLE_TIMEOUT_US (10 * 60 * 1000000LL)  // auto-stop when no HTTP activity for 10 minutes
// HTTPD task stack size is measured in bytes; flash operations must run from
// an internal-RAM task.
#define OTA_WIFI_STACK_SIZE 8192
#define OTA_SOCK_RCVBUF     (256 * 1024)
#define COREDUMP_PIECE      4096
// Free SD space kept beyond an SD bootmedia image.
#define BOOTMEDIA_SD_MARGIN (64 * 1024)

extern const uint8_t device_web_page_gz_start[] asm("_binary_device_web_page_html_gz_start");
extern const uint8_t device_web_page_gz_end[]   asm("_binary_device_web_page_html_gz_end");

static ota_idle_tracker_t s_idle;
static portMUX_TYPE s_idle_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_active = false;
static portMUX_TYPE s_lifecycle_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_lifecycle_busy = false;
static volatile bool s_maintenance = false;

/* Never wait for another lifecycle caller: the UI may hold lvgl_mux while
 * an HTTP handler is waiting for it during httpd_stop(). */
static bool lifecycle_try_take(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool available = !s_lifecycle_busy;
    if (available) s_lifecycle_busy = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return available;
}

static void lifecycle_give(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_token[17] = {0};
static char s_password[17] = {0};   // SoftAP WPA2 passphrase (OTA_WIFI_PASSWORD)
static char s_ap_ssid[33] = {0};
static bool s_wifi_owned = false;
static wifi_mode_t s_prev_wifi_mode = WIFI_MODE_NULL;

// /ota/firmware, /ota/theme, /ota/bootmedia share one upload slot.
typedef enum {
    OTA_UP_NONE = 0,
    OTA_UP_FIRMWARE,
    OTA_UP_THEME,
    OTA_UP_BOOTMEDIA,
} ota_up_kind_t;

static upload_sink_t s_sink;
static ota_up_kind_t s_up_kind = OTA_UP_NONE;
// Media uploads always stream to SD.
#define SD_THEME_UPLOAD SD_DIR_IMPORT "/.theme-upload.bin"
// Progress reported by /ota/status (kept after the sink is released).
static uint32_t s_stat_received = 0;
static uint32_t s_stat_expected = 0;
// Older browser serial uploaders send lengths only in /prepare and use the
// /upload suffix. Keep that contract scoped to the current OTA session.
static uint32_t s_bootmedia_prepared_size = 0;
static uint32_t s_bootmedia_prepared_manifest_size = 0;

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
                      "(BLE not released?)", internal_free);
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

/* Hand the single 2.4 GHz radio and as much internal DRAM as possible to the
 * SoftAP: BLE off for good (reboot required), video and theme caches freed.
 * Runs in the LVGL task; theme page objects are already deleted by the caller. */
static void release_for_ota(void)
{
    esp_err_t err = ble_hub_shutdown();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ble_hub_shutdown: %s", esp_err_to_name(err));
    }
    video_elem_release_all();
    video_player_release_all();
    theme_release_resources();
    ESP_LOGI(TAG, "released for OTA: internal free=%u, PSRAM largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static esp_err_t wifi_runtime_prepare(wifi_mode_t *current_mode)
{
    // netif / event loop are shared infrastructure: if they already exist
    // (ESP-NOW, other subsystems) we reuse them and never tear them down.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
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
        // keep the memory-lean config.  Needs the DRAM freed by release_for_ota().
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
}

/* ------------------------------------------------------------------ */
/* State (also updates the idle-activity timestamp)                   */
/* ------------------------------------------------------------------ */

static void set_state(ota_wifi_state_t state)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_idle_mux);
    ota_idle_state(&s_idle, state, now);
    portEXIT_CRITICAL(&s_idle_mux);
}

static ota_wifi_state_t get_state(void)
{
    portENTER_CRITICAL(&s_idle_mux);
    ota_wifi_state_t state = s_idle.state;
    portEXIT_CRITICAL(&s_idle_mux);
    return state;
}

static void record_activity(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_idle_mux);
    if (now > s_idle.last_activity_us) s_idle.last_activity_us = now;
    portEXIT_CRITICAL(&s_idle_mux);
}

static void upload_release(void)
{
    upload_sink_abort(&s_sink);
    s_up_kind = OTA_UP_NONE;
}

/* ------------------------------------------------------------------ */
/* Request helpers                                                     */
/* ------------------------------------------------------------------ */

static bool validate_token(httpd_req_t *req)
{
    if (s_maintenance) return false;
    char buf[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    // Constant-time compare to avoid token timing side channels.
    if (s_token[0] == '\0' || strlen(buf) != strlen(s_token)) {
        return false;
    }
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < strlen(s_token); i++) {
        diff |= (uint8_t)(buf[i] ^ s_token[i]);
    }
    if (diff == 0) record_activity();
    return diff == 0;
}

/* Refuse uploads while a library selection is in progress. */
static bool theme_lib_busy(void)
{
    return theme_library_get_state(NULL) == THEME_LIB_WRITING;
}

/* /api/v1 auth callback: authenticated requests count as activity. */
static bool api_auth(httpd_req_t *req)
{
    return s_active && validate_token(req);
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

// One OPTIONS handler; CORS for /ota/ and the vehicle-community POST allowlist.
static esp_err_t options_handler(httpd_req_t *req)
{
    if (strncmp(req->uri, "/ota/", 5) == 0) {
        set_cors_headers(req);
    } else if (vehicle_web_cors_path(req->uri)) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-OTA-Token");
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    }
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
    char storage_detail[BOOT_OBV_DETAIL_SIZE];
    sd_io_trace_t trace;
    if (sd_io_trace_current(&trace) && (!err || !strstr(err, " sd="))) {
        snprintf(storage_detail, sizeof(storage_detail), "%s", err ? err : "SD IO failed");
        size_t used_detail = strlen(storage_detail);
        sd_io_trace_format(&trace, storage_detail + used_detail, sizeof(storage_detail) - used_detail);
        err = storage_detail;
    }

    /* Preserve the complete bounded validator diagnostic. Truncate the message,
     * never the JSON delimiters, and escape strings without a heap allocation. */
    char json[BOOT_OBV_DETAIL_SIZE + 16] = "{\"error\":\"";
    size_t used = strlen(json);
    const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)(err ? err : ""); *p; p++) {
        size_t need = *p < 0x20 ? 6 : (*p == '"' || *p == '\\') ? 2 : 1;
        if (used + need + 3 > sizeof(json)) break; // Reserve closing quote, brace, NUL.
        if (*p < 0x20) {
            memcpy(json + used, "\\u00", 4);
            used += 4;
            json[used++] = hex[*p >> 4];
            json[used++] = hex[*p & 0xf];
        } else {
            if (need == 2) json[used++] = '\\';
            json[used++] = (char)*p;
        }
    }
    memcpy(json + used, "\"}", 3);
    return send_json_response(req, 400, json);
}

static esp_err_t send_204(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static bool hdr_present(httpd_req_t *req, const char *name, char *buf, size_t len)
{
    return httpd_req_get_hdr_value_len(req, name) < len &&
           httpd_req_get_hdr_value_str(req, name, buf, len) == ESP_OK;
}

static bool hdr_u32(httpd_req_t *req, const char *name, uint32_t def, uint32_t *out)
{
    char buf[16] = {0};
    if (httpd_req_get_hdr_value_len(req, name) >= sizeof(buf)) return false;
    esp_err_t err = httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf));
    if (err == ESP_ERR_NOT_FOUND) {
        *out = def;
        return true;
    }
    return err == ESP_OK && http_parse_u32(buf, out);
}

static bool hdr_last(httpd_req_t *req)
{
    char buf[8] = {0};
    return hdr_present(req, "X-Last", buf, sizeof(buf)) && strcmp(buf, "1") == 0;
}

static bool hdr_base64(httpd_req_t *req)
{
    char buf[64] = {0};
    return hdr_present(req, "Content-Type", buf, sizeof(buf)) && strstr(buf, "base64") != NULL;
}

/* Prepare / erase bodies are tiny placeholders; read them so a keep-alive
 * socket cannot carry unread bytes into the next request. */
static bool consume_body(httpd_req_t *req)
{
    char discard[32];
    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, discard,
                                      remaining > (int)sizeof(discard) ? (int)sizeof(discard) : remaining);
        if (received <= 0) {
            return false;
        }
        remaining -= received;
    }
    return true;
}

/* One snapshot avoids an unsigned underflow if free space changes between
 * the capacity check and the value reported to the client. */
static uint64_t sd_available_bytes(void)
{
    if (!sd_card_is_mounted()) return 0;
    uint64_t free_bytes = sd_card_get_free_bytes();
    return free_bytes > BOOTMEDIA_SD_MARGIN ? free_bytes - BOOTMEDIA_SD_MARGIN : 0;
}

static bool sd_can_hold(uint64_t total)
{
    return sd_card_is_mounted() && total <= sd_available_bytes();
}

/* ------------------------------------------------------------------ */
/* HTTP handlers: captive portal + device page                        */
/* ------------------------------------------------------------------ */

// GET / — browsers get the device page; captive-portal probes (Android/iOS
// fetch "/" right after associating, without text/html in Accept) get a
// lightweight 204 so the phone does not hang on an error page.
static esp_err_t root_handler(httpd_req_t *req)
{
    char accept[256] = {0};
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept));
    bool html = (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) && strstr(accept, "text/html");
    if (!html) {
        return send_204(req);
    }
    record_activity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)device_web_page_gz_start,
                           (ssize_t)(device_web_page_gz_end - device_web_page_gz_start));
}

// GET /generate_204, /connectivity-check — captive portal detection
static esp_err_t generate_204_handler(httpd_req_t *req)
{
    return send_204(req);
}

/* ------------------------------------------------------------------ */
/* HTTP handlers: /ota/... (frozen App protocol)                      */
/* ------------------------------------------------------------------ */

// GET /ota/discover
// No auth: the phone must learn the per-session token before it can call any
// authenticated endpoint.  Returning it here (after it joined the AP via the
// fixed bootstrap passphrase) is the bootstrap path.
static esp_err_t discover_handler(httpd_req_t *req)
{
    if (get_state() == OTA_WIFI_STATE_IDLE) {
        return send_err(req, "ota not active");
    }
    record_activity();
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

// GET /ota/info — device identity manifest, no auth (bootstrap endpoint). FROZEN.
static esp_err_t info_handler(httpd_req_t *req)
{
    if (get_state() == OTA_WIFI_STATE_IDLE) {
        return send_err(req, "ota not active");
    }
    record_activity();
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
    switch (get_state()) {
        case OTA_WIFI_STATE_READY: state_str = "ready"; break;
        case OTA_WIFI_STATE_RECEIVING: state_str = "receiving"; break;
        case OTA_WIFI_STATE_DONE: state_str = "done"; break;
        case OTA_WIFI_STATE_ERROR: state_str = "error"; break;
        default: break;
    }
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"received\":%u,\"expected\":%u}",
             state_str, (unsigned)s_stat_received, (unsigned)s_stat_expected);
    return send_json_response(req, 200, json);
}

/* Upload chunk protocol shared by /ota/firmware, /ota/theme, /ota/bootmedia.
 * Each request carries X-OTA-SHA256 / X-OTA-Size / X-Offset / X-Last (the App
 * sends 256 KB chunks; the body is received in 32 KB pieces). The payload is
 * accumulated in PSRAM (or streamed to SD for an oversized bootmedia image);
 * flash is written only after the last chunk verified, and the final reply is
 * sent only after the last flash write succeeded. A chunk that fails to
 * receive is rolled back so the App can retry it. */
static esp_err_t upload_run_impl(httpd_req_t *req, const char *label, const char *done_json,
                                 bool is_last, bool is_base64);
static esp_err_t upload_run(httpd_req_t *req, const char *label, const char *done_json,
                            bool is_last, bool is_base64)
{
    sd_io_trace_t trace;
    sd_io_trace_begin(&trace);
    esp_err_t result = upload_run_impl(req, label, done_json, is_last, is_base64);
    sd_io_trace_end(&trace);
    return result;
}
static esp_err_t upload_run_impl(httpd_req_t *req, const char *label, const char *done_json,
                                 bool is_last, bool is_base64)
{
    int sock_fd = httpd_req_to_sockfd(req);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "%s: invalid socket fd", label);
        upload_release();
        return send_err(req, "invalid socket");
    }
    int rcvbuf_size = OTA_SOCK_RCVBUF;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)) != 0) {
        ESP_LOGW(TAG, "%s: failed to set socket rcvbuf: %d", label, errno);
    }
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    const uint32_t offset = s_sink.received;
    int64_t t0 = esp_timer_get_time();
    set_state(OTA_WIFI_STATE_RECEIVING);
    esp_err_t err = upload_sink_recv(&s_sink, req, is_base64);
    s_stat_received = s_sink.received;
    s_stat_expected = s_sink.expected;
    if (err != ESP_OK) {
        // Network errors can retry at `offset`; an unrecoverable SD error ends the session.
        const char *reason = s_sink.err ? s_sink.err : "receive failed";
        bool fatal = s_sink.failed;
        ESP_LOGE(TAG, "%s chunk at %u failed: %s", label, (unsigned)offset,
                 reason);
        if (fatal) upload_release();
        set_state(OTA_WIFI_STATE_ERROR);
        return send_err(req, fatal ? reason : err == ESP_ERR_NO_MEM ? "no memory" : "receive failed");
    }
    ESP_LOGI(TAG, "%s chunk: offset=%u len=%u next=%u/%u last=%d (%lld ms)",
             label, (unsigned)offset, (unsigned)(s_sink.received - offset),
             (unsigned)s_sink.received, (unsigned)s_sink.expected, is_last,
             (long long)((esp_timer_get_time() - t0) / 1000));

    if (!is_last && s_sink.received < s_sink.expected) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":true,\"nextOffset\":%lu}",
                 (unsigned long)s_sink.received);
        return send_json_response(req, 200, json);
    }

    err = upload_sink_verify(&s_sink);
    if (err != ESP_OK) {
        const char *why = s_sink.err ? s_sink.err : "size mismatch";
        ESP_LOGE(TAG, "%s verify failed: %s", label, why);
        upload_release();
        set_state(OTA_WIFI_STATE_ERROR);
        return send_err(req, why);   // "size mismatch" / "sha mismatch"
    }

    /* Validate the staged OBV before replacing the current boot animation.
     * FatFs publishes the directory size on sync, before the second reader. */
    if (s_up_kind == OTA_UP_BOOTMEDIA && s_sink.manifest_size == 0) {
        const char *reason = "animation sync failed";
        char detail[BOOT_OBV_DETAIL_SIZE] = {0};
        char part[UPLOAD_PATH_MAX + 8];
        snprintf(part, sizeof(part), "%s.part", s_sink.path);
        if (fflush(s_sink.fp) != 0 || fsync(fileno(s_sink.fp)) != 0 ||
            !boot_obv_validate_file_detailed(part, &reason, detail, sizeof(detail))) {
            upload_release();
            set_state(OTA_WIFI_STATE_ERROR);
            return send_err(req, detail[0] ? detail : reason ? reason : "invalid JPEG animation");
        }
    }

    // Commit with the connection still open; the App waits for this reply.
    ESP_LOGI(TAG, "%s verified (%u bytes), installing", label, (unsigned)s_sink.expected);
    set_state(OTA_WIFI_STATE_RECEIVING);
    t0 = esp_timer_get_time();
    err = upload_sink_commit(&s_sink);
    if (err == ESP_OK && s_up_kind == OTA_UP_THEME) {
        char magic[4];
        FILE *theme = fopen(SD_THEME_UPLOAD, "rb");
        if (!theme) {
            err = ESP_FAIL;
        } else {
            size_t count = fread(magic, 1, sizeof(magic), theme);
            fclose(theme);
            if (count == sizeof(magic) && memcmp(magic, "OBDT", sizeof(magic)) == 0) {
                // Bundle errors must never fall through to the legacy importer.
                err = theme_bundle_install(SD_THEME_UPLOAD);
            } else {
                // Keep /ota/theme compatible with existing bare theme.bin clients.
                char key[STORAGE_THEME_KEY_LEN];
                err = theme_library_import_file(SD_THEME_UPLOAD, key, sizeof(key));
                if (err == ESP_OK) err = theme_library_select(key);
            }
        }
    }
    if (err == ESP_OK && s_up_kind == OTA_UP_BOOTMEDIA) {
        /* Only retire previous formats once the new file is durable. */
        const char *old_video = s_sink.manifest_size == 0 ? SD_BOOT_LEGACY : SD_BOOT_OBV;
        esp_err_t video_err = sd_card_remove(old_video);
        esp_err_t jpeg_err = sd_card_remove(SD_BOOT_JPEG);
        /* The old format may legitimately be absent. The new file has already
         * been committed and validated, so cleanup is best-effort. */
        if (video_err != ESP_OK && video_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "old boot media cleanup failed: %s", esp_err_to_name(video_err));
        }
        if (jpeg_err != ESP_OK && jpeg_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "legacy JPEG cleanup failed: %s", esp_err_to_name(jpeg_err));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s install failed: %s (%s)", label, esp_err_to_name(err),
                 s_sink.err ? s_sink.err : "-");
        upload_release();
        set_state(OTA_WIFI_STATE_ERROR);
        return send_err(req, "install failed");
    }
    ESP_LOGI(TAG, "%s installed in %lld ms, rebooting", label,
             (long long)((esp_timer_get_time() - t0) / 1000));
    upload_release();
    set_state(OTA_WIFI_STATE_DONE);

    esp_err_t response_err = send_json_response(req, 200, done_json);
    if (response_err != ESP_OK) {
        ESP_LOGW(TAG, "%s final response failed: %s", label, esp_err_to_name(response_err));
    }
    // Let the TCP response leave the radio before resetting.
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* Common header checks of /ota/firmware and /ota/theme. On success *expected,
 * *offset, *is_last, *is_base64 and sha are filled; otherwise the error reply
 * was sent and false is returned (with *ret set). */
static bool parse_chunk_headers(httpd_req_t *req, uint8_t sha[32], uint32_t *expected,
                                uint32_t *offset, bool *is_last, bool *is_base64, esp_err_t *ret)
{
    char sha_hex[65] = {0};
    char size_str[16] = {0};
    if (!hdr_present(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex)) ||
        !hdr_present(req, "X-OTA-Size", size_str, sizeof(size_str))) {
        *ret = send_err(req, "missing headers");
        return false;
    }
    *is_base64 = hdr_base64(req);
    bool sizes_ok = http_parse_u32(size_str, expected) && hdr_u32(req, "X-Offset", 0, offset);
    *is_last = hdr_last(req);

    if (!sizes_ok || !upload_sha_from_hex(sha_hex, sha) || *expected == 0 || *offset > *expected) {
        *ret = send_err(req, "invalid headers");
        return false;
    }
    if (!*is_base64 && (uint32_t)req->content_len > *expected - *offset) {
        *ret = send_err(req, "invalid chunk size");
        return false;
    }
    return true;
}

static bool chunk_in_sequence(ota_up_kind_t kind, uint32_t expected, uint32_t offset,
                              const uint8_t sha[32], uint32_t manifest_size)
{
    return s_up_kind == kind && upload_sink_is_open(&s_sink) &&
           s_sink.expected == expected && s_sink.received == offset &&
           s_sink.manifest_size == manifest_size &&
           memcmp(s_sink.sha_expected, sha, 32) == 0;
}

static void upload_begin(ota_up_kind_t kind)
{
    upload_release();
    api_v1_reset();          // free any half-done /api/v1 upload
    s_up_kind = kind;
    s_stat_received = 0;
    s_stat_expected = 0;
}

// POST /ota/firmware — chunked; flashed via esp_ota_* after the last chunk.
static esp_err_t firmware_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    uint8_t sha[32];
    uint32_t expected = 0, offset = 0;
    bool is_last = false, is_base64 = false;
    esp_err_t ret = ESP_OK;
    if (!parse_chunk_headers(req, sha, &expected, &offset, &is_last, &is_base64, &ret)) {
        return ret;
    }

    if (offset == 0) {
        upload_begin(OTA_UP_FIRMWARE);
        esp_err_t err = upload_sink_open(&s_sink, UPLOAD_SINK_FIRMWARE, expected, sha, NULL, 0);
        if (err != ESP_OK) {
            upload_release();
            if (err == ESP_ERR_NOT_FOUND) return send_err(req, "no ota partition");
            if (err == ESP_ERR_INVALID_SIZE) return send_err(req, "firmware too large");
            return send_err(req, "no memory for firmware buffer");
        }
    } else if (!chunk_in_sequence(OTA_UP_FIRMWARE, expected, offset, sha, 0)) {
        ESP_LOGE(TAG, "firmware chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)offset, (unsigned long)s_sink.received, (int)s_up_kind);
        return send_err(req, "unexpected chunk offset");
    }

    return upload_run(req, "firmware",
                      "{\"ok\":true,\"message\":\"firmware received, installing\"}",
                      is_last, is_base64);
}

// POST /ota/theme/prepare
// Checks SD space before streaming the upload.
static esp_err_t theme_prepare_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    if (theme_lib_busy()) {
        return send_err(req, "theme library busy");
    }
    char size_str[16] = {0};
    if (!hdr_present(req, "X-OTA-Size", size_str, sizeof(size_str))) {
        return send_err(req, "missing prepare size");
    }

    uint32_t total_size;
    if (!http_parse_u32(size_str, &total_size) || total_size == 0) {
        return send_err(req, "invalid prepare size");
    }

    if (!sd_card_is_mounted()) return send_err(req, "SD card required");
    uint64_t free_bytes = sd_available_bytes();
    if (free_bytes < total_size) return send_err(req, "not enough SD space");
    size_t partition_size = free_bytes > UINT32_MAX ? UINT32_MAX : (size_t)free_bytes;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!consume_body(req)) {
        return send_err(req, "prepare receive failed");
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"storage\":\"sd\",\"partitionSize\":%zu,\"psramFree\":%zu,\"largestBlock\":%zu}",
             partition_size, psram_free, largest);
    return send_json_response(req, 200, json);
}

// POST /ota/theme/erase — delete the selected theme package from SD.
static esp_err_t theme_erase_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    if (theme_lib_busy()) {
        return send_err(req, "theme library busy");
    }
    if (!consume_body(req)) return send_err(req, "receive failed");

    /* The OTA screen deletes all theme page objects before starting this
     * server. Unload the package before removing its directory so no live
     * theme can continue reading SD resources. */
    if (!lvgl_mux || xSemaphoreTake(lvgl_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return send_err(req, "display busy");
    }
    theme_unload();
    theme_info_t info;
    if (theme_get_info(&info) == ESP_OK) {
        xSemaphoreGive(lvgl_mux);
        return send_err(req, "theme still in use");
    }

    bool deleted = false;
    esp_err_t err = theme_library_delete_current(&deleted);
    xSemaphoreGive(lvgl_mux);
    if (err != ESP_OK) return send_err(req, "theme delete failed");

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"deleted\":%s}",
             deleted ? "true" : "false");
    return send_json_response(req, 200, json);
}

// POST /ota/vehicles/clear — delete all installed vehicle/PID profiles and
// leave builtin obd2.generic selected for the next boot.
static esp_err_t vehicle_clear_handler(httpd_req_t *req)
{
    if (!validate_token(req)) return send_err(req, "unauthorized");
    if (!consume_body(req)) return send_err(req, "receive failed");
    if (vp_manager_clear_installed() != ESP_OK) return send_err(req, "vehicle clear failed");
    return send_json_response(req, 200,
                              "{\"ok\":true,\"cleared\":true,\"pending\":\"obd2.generic\"}");
}

// POST /ota/theme — stream, validate, import and select an SD package.
static esp_err_t theme_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    uint8_t sha[32];
    uint32_t expected = 0, offset = 0;
    bool is_last = false, is_base64 = false;
    esp_err_t ret = ESP_OK;
    if (!parse_chunk_headers(req, sha, &expected, &offset, &is_last, &is_base64, &ret)) {
        return ret;
    }

    if (offset == 0) {
        if (theme_lib_busy()) {
            return send_err(req, "theme library busy");
        }
        upload_begin(OTA_UP_THEME);
        if (!sd_card_is_mounted()) {
            upload_release();
            return send_err(req, "SD card required");
        }
        if (!sd_can_hold(expected)) {
            upload_release();
            return send_err(req, "not enough SD space");
        }
        if (upload_sink_open(&s_sink, UPLOAD_SINK_SD_FILE, expected, sha, SD_THEME_UPLOAD, 0) != ESP_OK) {
            upload_release();
            return send_err(req, "SD theme upload open failed");
        }
    } else if (!chunk_in_sequence(OTA_UP_THEME, expected, offset, sha, 0)) {
        ESP_LOGE(TAG, "theme chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)offset, (unsigned long)s_sink.received, (int)s_up_kind);
        return send_err(req, "unexpected chunk offset");
    }

    return upload_run(req, "theme",
                      "{\"ok\":true,\"message\":\"theme received, installing\"}",
                      is_last, is_base64);
}

// GET /ota/coredump — stored core dump image as octet-stream, 204 when none
// (frozen App contract, see docs/APP_PROTOCOL.md).
static esp_err_t coredump_get_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    size_t size = 0;
    if (!coredump_store_present(&size) || size == 0) {
        set_cors_headers(req);
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    uint8_t *buf = heap_caps_malloc(COREDUMP_PIECE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        return send_err(req, "no memory");
    }

    set_cors_headers(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    for (size_t off = 0; off < size; ) {
        size_t n = size - off;
        if (n > COREDUMP_PIECE) n = COREDUMP_PIECE;
        esp_err_t err = coredump_store_read((uint32_t)off, buf, n);
        if (err != ESP_OK) {
            // Headers are already out; end the chunked stream early.
            ESP_LOGE(TAG, "coredump read at %u failed: %s", (unsigned)off, esp_err_to_name(err));
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) {
            ESP_LOGW(TAG, "coredump send failed at %u", (unsigned)off);
            break;
        }
        off += n;
    }
    free(buf);
    // Zero-length final chunk ends the chunked transfer.
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// POST /ota/coredump/erase
static esp_err_t coredump_erase_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    consume_body(req);

    esp_err_t err = coredump_store_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump erase failed: %s", esp_err_to_name(err));
        return send_err(req, "erase failed");
    }
    ESP_LOGI(TAG, "coredump erased");
    return send_json_response(req, 200, "{\"ok\":true}");
}

// POST /ota/bootmedia/prepare
// Checks SD capacity; manifest size zero selects a standalone OBV animation.
static esp_err_t bootmedia_prepare_handler(httpd_req_t *req)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }
    s_bootmedia_prepared_size = 0;
    s_bootmedia_prepared_manifest_size = 0;
    char total_str[16] = {0};
    char manifest_str[16] = {0};
    if (!hdr_present(req, "X-OTA-Size", total_str, sizeof(total_str)) ||
        !hdr_present(req, "X-OTA-Manifest-Size", manifest_str, sizeof(manifest_str))) {
        return send_err(req, "missing prepare sizes");
    }

    uint32_t total_size, manifest_size;
    if (!http_parse_u32(total_str, &total_size) ||
        !http_parse_u32(manifest_str, &manifest_size) || total_size == 0 ||
        (manifest_size != 0 &&
         (manifest_size >= total_size || manifest_size > BOOT_MEDIA_MANIFEST_MAX))) {
        return send_err(req, "invalid prepare sizes");
    }

    if (!sd_card_is_mounted()) return send_err(req, "SD card required");
    uint64_t required = manifest_size == 0 ? total_size :
        (uint64_t)total_size + BOOT_MEDIA_BIN_OFFSET - manifest_size;
    uint64_t sd_free = sd_available_bytes();
    if (sd_free < required)
        return send_err(req, "not enough SD space");
    size_t free_space = sd_free > UINT32_MAX ? UINT32_MAX : (size_t)sd_free;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!consume_body(req)) {
        return send_err(req, "prepare receive failed");
    }

    s_bootmedia_prepared_size = total_size;
    s_bootmedia_prepared_manifest_size = manifest_size;

    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"storage\":\"sd\",\"free\":%zu,\"psramFree\":%zu,\"largestBlock\":%zu}",
             free_space, psram_free, largest);
    return send_json_response(req, 200, json);
}

// POST /ota/bootmedia — OBV or legacy [manifest][bin] stream.
static esp_err_t bootmedia_handle(httpd_req_t *req, bool legacy_path)
{
    if (!validate_token(req)) {
        return send_err(req, "unauthorized");
    }

    char sha_hex[65] = {0};
    char size_str[16] = {0};
    char manifest_str[16] = {0};
    if (!hdr_present(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex))) {
        return send_err(req, "missing headers");
    }

    const bool has_size = hdr_present(req, "X-OTA-Size", size_str, sizeof(size_str));
    const bool has_manifest = hdr_present(req, "X-OTA-Manifest-Size", manifest_str,
                                          sizeof(manifest_str));
    if ((!has_size && httpd_req_get_hdr_value_len(req, "X-OTA-Size") > 0) ||
        (!has_manifest && httpd_req_get_hdr_value_len(req, "X-OTA-Manifest-Size") > 0)) {
        return send_err(req, "invalid sizes");
    }
    if (has_size != has_manifest) {
        return send_err(req, "missing headers");
    }
    if (!has_size && legacy_path) {
        if (s_bootmedia_prepared_size == 0) {
            return send_err(req, "missing headers");
        }
        snprintf(size_str, sizeof(size_str), "%lu", (unsigned long)s_bootmedia_prepared_size);
        snprintf(manifest_str, sizeof(manifest_str), "%lu",
                 (unsigned long)s_bootmedia_prepared_manifest_size);
    } else if (!has_size) {
        return send_err(req, "missing headers");
    }

    bool is_base64 = hdr_base64(req);
    uint32_t total_size = 0, manifest_size = 0, offset = 0;
    bool sizes_ok = http_parse_u32(size_str, &total_size) &&
                    http_parse_u32(manifest_str, &manifest_size) &&
                    hdr_u32(req, "X-Offset", 0, &offset);
    bool is_last = hdr_last(req);

    if (!sizes_ok || total_size == 0 || (manifest_size != 0 &&
        (manifest_size > BOOT_MEDIA_MANIFEST_MAX || manifest_size >= total_size)) || offset > total_size ||
        (!is_base64 && (uint32_t)req->content_len > total_size - offset)) {
        ESP_LOGE(TAG, "invalid sizes/offset: total=%lu manifest=%lu offset=%lu content_len=%d",
                 (unsigned long)total_size, (unsigned long)manifest_size,
                 (unsigned long)offset, (int)req->content_len);
        return send_err(req, "invalid sizes");
    }
    uint8_t sha[32];
    if (!upload_sha_from_hex(sha_hex, sha)) {
        return send_err(req, "invalid sha");
    }

    if (offset == 0) {
        if (theme_lib_busy()) {
            return send_err(req, "theme library busy");
        }
        upload_begin(OTA_UP_BOOTMEDIA);
        if (!sd_card_is_mounted()) {
            upload_release();
            return send_err(req, "SD card required");
        }
        uint64_t required = manifest_size == 0 ? total_size :
            (uint64_t)total_size + BOOT_MEDIA_BIN_OFFSET - manifest_size;
        if (!sd_can_hold(required)) {
            upload_release();
            return send_err(req, "not enough SD space");
        }
        esp_err_t err = upload_sink_open(&s_sink, UPLOAD_SINK_SD_FILE, total_size, sha,
                                        manifest_size == 0 ? SD_BOOT_OBV : SD_BOOT_LEGACY, manifest_size);
        if (err != ESP_OK) {
            upload_release();
            return send_err(req, "SD bootmedia upload open failed");
        }
    } else if (!chunk_in_sequence(OTA_UP_BOOTMEDIA, total_size, offset, sha, manifest_size)) {
        ESP_LOGE(TAG, "bootmedia chunk out of sequence: offset=%lu expected=%lu kind=%d",
                 (unsigned long)offset, (unsigned long)s_sink.received, (int)s_up_kind);
        return send_err(req, "unexpected chunk offset");
    }

    return upload_run(req, "bootmedia",
                      "{\"ok\":true,\"message\":\"bootmedia received, installing\"}",
                      is_last, is_base64);
}

static esp_err_t bootmedia_handler(httpd_req_t *req)
{
    return bootmedia_handle(req, false);
}

static esp_err_t bootmedia_legacy_handler(httpd_req_t *req)
{
    return bootmedia_handle(req, true);
}

/* ------------------------------------------------------------------ */
/* Idle watchdog: auto-stop after OTA_IDLE_TIMEOUT_US without activity */
/* ------------------------------------------------------------------ */

static bool ota_wifi_server_stop_impl(void);

static void ota_timeout_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_idle_mux);
        bool current = ota_idle_current(&s_idle, generation);
        bool expired = ota_idle_expired(&s_idle, generation, now, OTA_IDLE_TIMEOUT_US);
        portEXIT_CRITICAL(&s_idle_mux);
        if (!current) break;
        if (expired && lifecycle_try_take()) {
            /* A stop/start or request may have occurred after the first check.
             * Recheck under lifecycle protection before touching the server. */
            now = esp_timer_get_time();
            portENTER_CRITICAL(&s_idle_mux);
            current = ota_idle_current(&s_idle, generation);
            expired = ota_idle_expired(&s_idle, generation, now, OTA_IDLE_TIMEOUT_US);
            portEXIT_CRITICAL(&s_idle_mux);
            bool stopped = false;
            if (expired) {
                ESP_LOGW(TAG, "WiFi OTA idle timeout (no HTTP activity for %lld s), stopping",
                         (long long)(OTA_IDLE_TIMEOUT_US / 1000000));
                stopped = ota_wifi_server_stop_impl();
            }
            lifecycle_give();
            if (!current || stopped) break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t def = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    esp_err_t err = httpd_register_uri_handler(s_httpd, &def);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
    }
}

static void register_handlers(void)
{
    // Captive portal probes and the device page.
    register_uri("/generate_204", HTTP_GET, generate_204_handler);
    register_uri("/connectivity-check", HTTP_GET, generate_204_handler);
    register_uri("/", HTTP_GET, root_handler);

    // App protocol (frozen).
    register_uri("/ota/discover", HTTP_GET, discover_handler);
    register_uri("/ota/info", HTTP_GET, info_handler);
    register_uri("/ota/status", HTTP_GET, status_handler);
    register_uri("/ota/firmware", HTTP_POST, firmware_handler);
    register_uri("/ota/bootmedia/prepare", HTTP_POST, bootmedia_prepare_handler);
    register_uri("/ota/bootmedia", HTTP_POST, bootmedia_handler);
    register_uri("/ota/bootmedia/upload", HTTP_POST, bootmedia_legacy_handler);
    register_uri("/ota/theme/prepare", HTTP_POST, theme_prepare_handler);
    register_uri("/ota/theme/erase", HTTP_POST, theme_erase_handler);
    register_uri("/ota/theme", HTTP_POST, theme_handler);
    register_uri("/ota/vehicles/clear", HTTP_POST, vehicle_clear_handler);
    register_uri("/ota/coredump", HTTP_GET, coredump_get_handler);
    register_uri("/ota/coredump/erase", HTTP_POST, coredump_erase_handler);

    // Device API (token, narrowly scoped vehicle-community CORS).
    api_v1_register_handlers(s_httpd, api_auth);

    // One wildcard OPTIONS handler covers every route.
    register_uri("/*", HTTP_OPTIONS, options_handler);
}

static bool ota_wifi_server_start_impl(ota_wifi_info_t *info)
{
    if (!info) return false;
    if (get_state() != OTA_WIFI_STATE_IDLE) {
        ESP_LOGW(TAG, "WiFi OTA already running");
        return false;
    }

    s_active = true;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_idle_mux);
    uint32_t generation = ota_idle_begin(&s_idle, now);
    portEXIT_CRITICAL(&s_idle_mux);
    /* Allocate the watchdog before touching BLE/WiFi; otherwise task creation
     * failure would leave a live OTA server without an inactivity timeout. */
    if (xTaskCreate(ota_timeout_task, "ota_timeout", 3072, (void *)(uintptr_t)generation,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot create OTA idle watchdog");
        set_state(OTA_WIFI_STATE_IDLE);
        s_active = false;
        return false;
    }
    release_for_ota();

    // SSID with the SoftAP MAC suffix
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", WIFI_SSID_PREFIX, mac[4], mac[5]);

    // Fixed password; the user joins the SoftAP manually.
    strlcpy(s_password, OTA_WIFI_PASSWORD, sizeof(s_password));

    // Per-session token
    uint8_t token_bytes[8];
    esp_fill_random(token_bytes, sizeof(token_bytes));
    for (int i = 0; i < 8; i++) {
        snprintf(&s_token[i * 2], 3, "%02x", token_bytes[i]);
    }

    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_err_t err = wifi_runtime_prepare(&current_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi runtime prepare failed: %s", esp_err_to_name(err));
        set_state(OTA_WIFI_STATE_ERROR);
        wifi_runtime_cleanup(false);
        return false;
    }

    s_prev_wifi_mode = current_mode;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGE(TAG, "ap netif create failed");
        set_state(OTA_WIFI_STATE_ERROR);
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
            set_state(OTA_WIFI_STATE_ERROR);
            wifi_runtime_cleanup(false);
            return false;
        }
    }

    // SoftAP on channel 1 — same channel as ESP-NOW, so the single radio never
    // has to hop and break the ESP-NOW link.
    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_CHANNEL,
            .max_connection = WIFI_MAX_STA,
            .authmode = WIFI_AUTH_WPA2_PSK,
            // PMF not required — OTA is a private single-client AP; beacon IEs
            // are kept minimal to keep the beacon allocation small.
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)wifi_config.ap.password, s_password, sizeof(wifi_config.ap.password) - 1);

    err = esp_wifi_set_mode(target_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi mode set failed: %s", esp_err_to_name(err));
        set_state(OTA_WIFI_STATE_ERROR);
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi config failed: %s", esp_err_to_name(err));
        set_state(OTA_WIFI_STATE_ERROR);
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        if (!s_wifi_owned && (err == ESP_ERR_WIFI_STATE || err == ESP_ERR_INVALID_STATE)) {
            ESP_LOGD(TAG, "WiFi already started, reusing existing driver");
        } else {
            ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
            set_state(OTA_WIFI_STATE_ERROR);
            wifi_runtime_cleanup(!s_wifi_owned);
            return false;
        }
    }
    // 600 s AP inactivity timeout so phones running network checks stay associated.
    esp_wifi_set_inactive_time(WIFI_IF_AP, 600);

    // Modem sleep costs airtime on a link that is about to carry several MB.
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "disable power save failed: %s", esp_err_to_name(ps_err));
    }
    // Max legal TX power (unit is 0.25 dBm) — fewer PHY errors means fewer
    // retransmits, and every retransmit costs an RTO on this link.
    esp_wifi_set_max_tx_power(80);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = OTA_WIFI_STACK_SIZE;
    // Keep the httpd task stack in internal RAM: flash operations disable the
    // cache, so any task that touches flash must not live in PSRAM.
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.core_id = 1;                              // keep HTTP handler off the WiFi/BLE core
    config.recv_wait_timeout = 2;                    // per recv call; 3 consecutive timeouts fail a chunk
    config.send_wait_timeout = 300;
    config.max_uri_handlers = 24;                    // 14 /ota + captive + 4 /api/v1 + OPTIONS, with headroom
    config.max_resp_headers = 4;                     // CORS (3) + one more
    config.max_open_sockets = 7;                     // max allowed by LWIP_MAX_SOCKETS (10 - 3 internal)
    config.backlog_conn = 5;
    config.lru_purge_enable = true;                  // close idle connections when sockets run out
    config.uri_match_fn = httpd_uri_match_wildcard;  // /api/v1/* and the OPTIONS catch-all
    config.max_req_hdr_len = 1024;                   // OTA custom headers + Accept/Content-Type

    err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s (internal free=%lu)",
                 esp_err_to_name(err),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_httpd = NULL;
        set_state(OTA_WIFI_STATE_ERROR);
        wifi_runtime_cleanup(!s_wifi_owned);
        return false;
    }
    register_handlers();

    strlcpy(info->ssid, s_ap_ssid, sizeof(info->ssid));
    strlcpy(info->password, s_password, sizeof(info->password));
    strlcpy(info->ip, "192.168.4.1", sizeof(info->ip));
    strlcpy(info->token, s_token, sizeof(info->token));
    info->port = WIFI_PORT;

    set_state(OTA_WIFI_STATE_READY);
    ESP_LOGI(TAG, "WiFi OTA started: SSID=%s IP=%s", s_ap_ssid, info->ip);

    return true;
}

bool ota_wifi_server_start(ota_wifi_info_t *info)
{
    if (!lifecycle_try_take()) return false;
    bool started = ota_wifi_server_start_impl(info);
    lifecycle_give();
    return started;
}

static bool ota_wifi_server_stop_impl(void)
{
    if (get_state() == OTA_WIFI_STATE_IDLE) {
        s_active = false;
        return true;
    }

    if (s_httpd) {
        esp_err_t err = httpd_stop(s_httpd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "httpd stop failed: %s", esp_err_to_name(err));
            return false;
        }
        s_httpd = NULL;
    }
    // No handler can run any more: release upload buffers / temp files.
    upload_release();
    api_v1_reset();

    wifi_runtime_cleanup(true);

    memset(s_token, 0, sizeof(s_token));
    memset(s_password, 0, sizeof(s_password));
    memset(s_ap_ssid, 0, sizeof(s_ap_ssid));
    s_stat_received = 0;
    s_stat_expected = 0;
    s_bootmedia_prepared_size = 0;
    s_bootmedia_prepared_manifest_size = 0;
    set_state(OTA_WIFI_STATE_IDLE);
    s_active = false;
    ESP_LOGI(TAG, "WiFi OTA stopped");
    return true;
}

bool ota_wifi_server_stop(void)
{
    if (!lifecycle_try_take()) return false;
    bool stopped = ota_wifi_server_stop_impl();
    lifecycle_give();
    return stopped;
}

typedef struct {
    TaskHandle_t worker;
    bool accepted;
} sd_format_gate_t;

/* Runs on the HTTP task between handlers: no in-flight handler can pass auth
 * before we latch maintenance and then start a new upload after the busy test. */
static void sd_format_gate(void *arg)
{
    sd_format_gate_t *gate = arg;
    ota_wifi_state_t state = get_state();
    gate->accepted = (state == OTA_WIFI_STATE_READY || state == OTA_WIFI_STATE_ERROR) &&
                     !upload_sink_is_open(&s_sink) && !api_v1_upload_busy() && !theme_lib_busy();
    if (gate->accepted) s_maintenance = true;
    xTaskNotifyGive(gate->worker);
}

esp_err_t ota_wifi_server_format_sd(char *detail, size_t capacity, bool *reboot_required)
{
    if (reboot_required) *reboot_required = false;
    if (detail && capacity) detail[0] = '\0';
    if (!reboot_required || !lifecycle_try_take()) return ESP_ERR_INVALID_STATE;
    if (!s_active || !s_httpd || s_maintenance) {
        lifecycle_give();
        return ESP_ERR_INVALID_STATE;
    }
    sd_format_gate_t gate = {.worker = xTaskGetCurrentTaskHandle()};
    /* This worker owns notification slot 0; never use the LVGL task here. */
    ulTaskNotifyTake(pdTRUE, 0);
    esp_err_t err = httpd_queue_work(s_httpd, sd_format_gate, &gate);
    if (err != ESP_OK) { lifecycle_give(); return err; }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!gate.accepted) {
        if (detail && capacity) snprintf(detail, capacity, "Upload/storage busy; finish or cancel the upload first");
        lifecycle_give();
        return ESP_ERR_INVALID_STATE;
    }
    /* From here, caller MUST reboot, including all failures. Keep lifecycle
     * reserved and s_active true so USB/idle watchdog/card jobs cannot take over. */
    *reboot_required = true;
    err = httpd_stop(s_httpd);  /* worker owns no LVGL/storage lock */
    if (err != ESP_OK) {
        if (detail && capacity) snprintf(detail, capacity, "stop HTTP: %s", esp_err_to_name(err));
        return err;
    }
    s_httpd = NULL;
    if (!storage_job_lock(0)) {
        if (detail && capacity) snprintf(detail, capacity, "Storage busy; SD was NOT formatted");
        return ESP_ERR_INVALID_STATE;
    }
    api_v1_reset();
    upload_release();
    err = sd_card_format(detail, capacity);
    storage_job_unlock();
    return err;
}

bool ota_wifi_server_is_active(void)
{
    return s_active;
}

const char *ota_wifi_server_get_ssid(void)
{
    return s_ap_ssid;
}
