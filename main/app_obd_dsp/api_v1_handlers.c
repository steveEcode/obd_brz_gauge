// api_v1_handlers.c — /api/v1/* device API (see api_v1_handlers.h)
//
// One wildcard dispatcher per HTTP method. All handlers run in the single
// httpd task, so the upload slot below needs no locking.
#include "api_v1_handlers.h"
#include "http_u32.h"
#include "vehicle_web_cors.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_obd_dsp/boot_media_mount.h"
#include "app_obd_dsp/coredump_store.h"
#include "app_obd_dsp/device_identity.h"
#include "app_obd_dsp/first_boot_migration.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "app_obd_dsp/theme_library.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/upload_sink.h"
#include "bsp_obd_dsp/config_part.h"
#include "bsp_obd_dsp/sd_card.h"
#include "theme_engine/theme_interface.h"
#include "vehicle/vp_manager.h"

#ifndef OBD_GAUGE_BUILD_TAG
#define OBD_GAUGE_BUILD_TAG "unknown"
#endif
#ifndef OBD_GAUGE_GIT_COUNT
#define OBD_GAUGE_GIT_COUNT 0
#endif
#ifndef OBD_GAUGE_GIT_BRANCH
#define OBD_GAUGE_GIT_BRANCH "unknown"
#endif
#ifndef OBD_GAUGE_GIT_HASH
#define OBD_GAUGE_GIT_HASH "unknown"
#endif

static const char *TAG = "api_v1";

#define API_PREFIX           "/api/v1/"
#define API_PATH_MAX         96
#define API_QUERY_MAX        512
#define API_LIST_MAX         200            // fs/list entry cap
#define API_VEH_LIST_MAX     32
#define API_VEH_ISSUES       16
#define API_REPORT_LEN       512
#define API_SIDECAR_MEM_MAX  (64 * 1024)    // larger sidecars stream to SD
#define API_SIDECAR_MAX      (32 * 1024 * 1024)
#define API_SD_MARGIN        (64 * 1024)
#define API_LOG_DEFAULT      (8 * 1024)
#define API_LOG_MAX          (32 * 1024)
#define API_SIDECAR_TMP      SD_DIR_IMPORT "/websidecar.tmp"
#define API_EPOCH_MIN        1600000000LL   // 2020-09-13; anything older is a client bug

static api_v1_auth_fn s_auth = NULL;

// One upload slot for theme/sidecar and fs/put (chunked like /ota/*).
typedef enum {
    API_UP_NONE = 0,
    API_UP_SIDECAR,
    API_UP_FS,
} api_up_kind_t;

static upload_sink_t s_up;
static api_up_kind_t s_up_kind = API_UP_NONE;

/* ------------------------------------------------------------------ */
/* JSON output                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} jbuf_t;

static void jb_init(jbuf_t *j, size_t cap)
{
    j->buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    j->len = 0;
    j->cap = j->buf ? cap : 0;
    j->oom = (j->buf == NULL);
    if (j->buf) j->buf[0] = '\0';
}

static void jb_free(jbuf_t *j)
{
    free(j->buf);
    memset(j, 0, sizeof(*j));
}

static bool jb_reserve(jbuf_t *j, size_t extra)
{
    if (j->oom) return false;
    if (j->len + extra + 1 <= j->cap) return true;
    size_t cap = j->cap * 2;
    while (cap < j->len + extra + 1) cap *= 2;
    char *p = heap_caps_realloc(j->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        j->oom = true;
        return false;
    }
    j->buf = p;
    j->cap = cap;
    return true;
}

static void jb_putc(jbuf_t *j, char c)
{
    if (!jb_reserve(j, 1)) return;
    j->buf[j->len++] = c;
    j->buf[j->len] = '\0';
}

static void jb_raw(jbuf_t *j, const char *s)
{
    size_t n = strlen(s);
    if (!jb_reserve(j, n)) return;
    memcpy(j->buf + j->len, s, n + 1);
    j->len += n;
}

static void jb_printf(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_printf(jbuf_t *j, const char *fmt, ...)
{
    if (j->oom) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        j->buf[j->len] = '\0';
        return;
    }
    if (j->len + (size_t)n + 1 > j->cap) {
        j->buf[j->len] = '\0';
        if (!jb_reserve(j, (size_t)n)) return;
        va_start(ap, fmt);
        vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
        va_end(ap);
    }
    j->len += (size_t)n;
}

/* Quoted, escaped JSON string (NULL -> ""). */
static void jb_str(jbuf_t *j, const char *s)
{
    jb_putc(j, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') {
            jb_putc(j, '\\');
            jb_putc(j, (char)*p);
        } else if (*p < 0x20) {
            jb_printf(j, "\\u%04x", *p);
        } else {
            jb_putc(j, (char)*p);
        }
    }
    jb_putc(j, '"');
}

/* Comma before the next member unless one was just opened. */
static void jb_sep(jbuf_t *j)
{
    if (j->oom || j->len == 0) return;
    char last = j->buf[j->len - 1];
    if (last != '{' && last != '[' && last != ':') jb_putc(j, ',');
}

static void jb_key(jbuf_t *j, const char *key)
{
    jb_sep(j);
    jb_str(j, key);
    jb_putc(j, ':');
}

static void jb_open(jbuf_t *j, const char *key, char bracket)
{
    if (key) jb_key(j, key);
    else jb_sep(j);
    jb_putc(j, bracket);
}

static void jb_kstr(jbuf_t *j, const char *key, const char *val)  { jb_key(j, key); jb_str(j, val); }
static void jb_kbool(jbuf_t *j, const char *key, bool val)        { jb_key(j, key); jb_raw(j, val ? "true" : "false"); }
static void jb_ku64(jbuf_t *j, const char *key, uint64_t val)     { jb_key(j, key); jb_printf(j, "%llu", (unsigned long long)val); }

/* ------------------------------------------------------------------ */
/* Responses                                                           */
/* ------------------------------------------------------------------ */

static const char *status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 422: return "422 Unprocessable Entity";
    case 507: return "507 Insufficient Storage";
    default:  return "500 Internal Server Error";
    }
}

static esp_err_t send_jb(httpd_req_t *req, int status, jbuf_t *j);

/* {"ok":false,"error":code,"detail":detail} */
static esp_err_t send_error(httpd_req_t *req, int status, const char *code, const char *detail)
{
    jbuf_t j;
    jb_init(&j, 256);
    jb_raw(&j, "{\"ok\":false");
    jb_kstr(&j, "error", code);
    jb_kstr(&j, "detail", detail ? detail : "");
    jb_putc(&j, '}');
    if (j.oom) {
        jb_free(&j);
        httpd_resp_set_status(req, status_line(status));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no_memory\",\"detail\":\"\"}");
    }
    return send_jb(req, status, &j);
}

/* Sends and frees j (500 no_memory when it ran out of memory). */
static esp_err_t send_jb(httpd_req_t *req, int status, jbuf_t *j)
{
    if (j->oom) {
        jb_free(j);
        return send_error(req, 500, "no_memory", "response too large");
    }
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, j->buf, (ssize_t)j->len);
    jb_free(j);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* esp_err_t -> HTTP status + error code. */
static esp_err_t send_esp_err(httpd_req_t *req, esp_err_t err, const char *detail)
{
    int status = 500;
    const char *code = "failed";
    switch (err) {
    case ESP_ERR_NOT_FOUND:     status = 404; code = "not_found"; break;
    case ESP_ERR_INVALID_STATE: status = 409; code = "invalid_state"; break;
    case ESP_ERR_INVALID_ARG:   status = 400; code = "invalid_arg"; break;
    case ESP_ERR_INVALID_SIZE:  status = 413; code = "too_large"; break;
    case ESP_ERR_NO_MEM:        status = 500; code = "no_memory"; break;
    case ESP_ERR_NOT_SUPPORTED: status = 409; code = "not_supported"; break;
    default: break;
    }
    return send_error(req, status, code, (detail && detail[0]) ? detail : esp_err_to_name(err));
}

static esp_err_t send_vpm_err(httpd_req_t *req, esp_err_t err)
{
    return send_esp_err(req, err, vp_manager_last_error());
}

/* ------------------------------------------------------------------ */
/* Request helpers                                                     */
/* ------------------------------------------------------------------ */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place URL decode (%XX, '+'). false on a malformed escape or %00. */
static bool url_decode(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '%') {
            int hi = hex_val(p[1]);
            int lo = hi < 0 ? -1 : hex_val(p[2]);
            if (lo < 0 || (hi == 0 && lo == 0)) return false;
            *o++ = (char)((hi << 4) | lo);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
    return true;
}

/* Decoded query parameter. false when absent, too long or malformed. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen >= API_QUERY_MAX) return false;
    char *query = malloc(qlen + 1);
    if (!query) return false;
    bool ok = httpd_req_get_url_query_str(req, query, qlen + 1) == ESP_OK &&
              httpd_query_key_value(query, key, out, out_len) == ESP_OK &&
              url_decode(out);
    free(query);
    return ok;
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
    return httpd_req_get_hdr_value_str(req, "X-Last", buf, sizeof(buf)) == ESP_OK &&
           strcmp(buf, "1") == 0;
}

/* Whole request body into a fresh NUL-terminated PSRAM buffer. */
static esp_err_t read_body(httpd_req_t *req, size_t max, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if ((size_t)req->content_len > max) return ESP_ERR_INVALID_SIZE;

    upload_sink_t s = {0};
    esp_err_t err = upload_sink_open(&s, UPLOAD_SINK_MEMORY, req->content_len, NULL, NULL, 0);
    if (err == ESP_OK) err = upload_sink_recv(&s, req, false);
    if (err == ESP_OK) err = upload_sink_verify(&s);
    if (err != ESP_OK) {
        upload_sink_abort(&s);
        return err == ESP_ERR_NO_MEM ? err : ESP_FAIL;
    }
    *out = (char *)s.buf;
    *out_len = s.received;
    s.buf = NULL;
    upload_sink_abort(&s);
    return ESP_OK;
}

static esp_err_t send_body_err(httpd_req_t *req, esp_err_t err, size_t max)
{
    char detail[48];
    switch (err) {
    case ESP_ERR_INVALID_SIZE:
        snprintf(detail, sizeof(detail), "body larger than %u bytes", (unsigned)max);
        return send_error(req, 413, "too_large", detail);
    case ESP_ERR_NO_MEM:
        return send_error(req, 500, "no_memory", "request body");
    default:
        return send_error(req, 400, "receive_failed", "request body");
    }
}

/* Body {"<key>":"<string>"} -> out. Sends the error reply itself. */
static bool body_string(httpd_req_t *req, const char *key, char *out, size_t out_len, esp_err_t *ret)
{
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, 1024, &body, &len);
    if (err != ESP_OK) {
        *ret = send_body_err(req, err, 1024);
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *item = root ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    bool ok = cJSON_IsString(item) && item->valuestring && strlen(item->valuestring) < out_len;
    if (ok) strlcpy(out, item->valuestring, out_len);
    cJSON_Delete(root);
    if (!ok) {
        char detail[48];
        snprintf(detail, sizeof(detail), "expected {\"%s\":\"...\"}", key);
        *ret = send_error(req, 400, root ? "bad_request" : "bad_json", detail);
    }
    return ok;
}

static bool valid_vehicle_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= VPM_ID_LEN) return false;
    for (const char *p = id; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return false;
    }
    return true;
}

static const char *veh_state_name(veh_state_t st)
{
    switch (st) {
    case VEH_STATE_INSTALLED: return "installed";
    case VEH_STATE_PENDING:   return "pending";
    case VEH_STATE_ACTIVE:    return "active";
    case VEH_STATE_FAILED:    return "failed";
    default:                  return "none";
    }
}

static const char *sd_state_name(sd_state_t st)
{
    switch (st) {
    case SD_STATE_ABSENT:  return "absent";
    case SD_STATE_MOUNTED: return "mounted";
    case SD_STATE_ERROR:   return "error";
    case SD_STATE_EJECTED: return "ejected";
    default:               return "disabled";
    }
}

/* ------------------------------------------------------------------ */
/* Paths: SD /OBD tree and the config partition only                  */
/* ------------------------------------------------------------------ */

typedef enum {
    API_FS_NONE = 0,
    API_FS_SD,
    API_FS_CONFIG,
} api_fs_t;

static bool has_prefix_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Resolve the ?path= parameter. Relative paths are under SD_OBD_ROOT.
 * dir_ok: the root directories themselves are accepted (fs/list).
 * Sends the error reply itself and returns API_FS_NONE on failure. */
static api_fs_t resolve_path(httpd_req_t *req, const char *def, bool dir_ok,
                             char out[UPLOAD_PATH_MAX], esp_err_t *ret)
{
    char raw[UPLOAD_PATH_MAX];
    if (!query_param(req, "path", raw, sizeof(raw))) {
        if (!def) {
            *ret = send_error(req, 400, "bad_path", "missing or malformed ?path=");
            return API_FS_NONE;
        }
        strlcpy(raw, def, sizeof(raw));
    }

    int n = raw[0] == '/' ? snprintf(out, UPLOAD_PATH_MAX, "%s", raw)
                          : snprintf(out, UPLOAD_PATH_MAX, "%s/%s", SD_OBD_ROOT, raw);
    if (n < 0 || n >= UPLOAD_PATH_MAX) {
        *ret = send_error(req, 400, "bad_path", "path too long");
        return API_FS_NONE;
    }
    size_t len = (size_t)n;
    if (dir_ok) {
        while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    }

    bool bad = len == 0 || strstr(out, "..") || strstr(out, "//") || strchr(out, '\\') ||
               (!dir_ok && out[len - 1] == '/');
    for (const char *p = out; !bad && *p; p++) {
        if ((unsigned char)*p < 0x20) bad = true;
    }
    if (bad) {
        *ret = send_error(req, 400, "bad_path", out);
        return API_FS_NONE;
    }

    api_fs_t fs = API_FS_NONE;
    if (has_prefix_dir(out, SD_OBD_ROOT)) {
        fs = API_FS_SD;
    } else if (has_prefix_dir(out, CONFIG_MOUNT_POINT)) {
        fs = API_FS_CONFIG;
    }
    bool is_root = strcmp(out, SD_OBD_ROOT) == 0 || strcmp(out, CONFIG_MOUNT_POINT) == 0;
    if (fs == API_FS_NONE || (is_root && !dir_ok)) {
        *ret = send_error(req, 400, "bad_path", "allowed: " SD_OBD_ROOT "/... or " CONFIG_MOUNT_POINT "/...");
        return API_FS_NONE;
    }
    if (fs == API_FS_SD && !sd_card_is_mounted()) {
        *ret = send_error(req, 409, "sd_unavailable", sd_state_name(sd_card_get_state()));
        return API_FS_NONE;
    }
    if (fs == API_FS_CONFIG && !config_part_is_mounted()) {
        *ret = send_error(req, 409, "config_unavailable", "config partition not mounted");
        return API_FS_NONE;
    }
    return fs;
}

/* ------------------------------------------------------------------ */
/* Chunked uploads (theme/sidecar, fs/put)                             */
/* ------------------------------------------------------------------ */

static void up_release(void)
{
    upload_sink_abort(&s_up);
    s_up_kind = API_UP_NONE;
}

static esp_err_t send_sink_err(httpd_req_t *req, esp_err_t err)
{
    const char *why = s_up.err ? s_up.err : esp_err_to_name(err);
    if (err == ESP_ERR_NO_MEM) return send_error(req, 500, "no_memory", why);
    if (err == ESP_ERR_INVALID_SIZE) return send_error(req, 413, "too_large", why);
    if (err == ESP_ERR_INVALID_CRC) return send_error(req, 400, "sha_mismatch", why);
    if (err == ESP_ERR_INVALID_ARG) return send_error(req, 400, "bad_request", why);
    if (s_up.err && strncmp(s_up.err, "sd ", 3) == 0) return send_error(req, 500, "sd_error", why);
    if (s_up.err && strncmp(s_up.err, "config", 6) == 0) return send_error(req, 500, "config_error", why);
    return send_error(req, 400, "receive_failed", why);
}

/* Chunk headers: X-OTA-Size (total, default content_len), X-Offset,
 * X-Last, optional X-OTA-SHA256. */
typedef struct {
    uint32_t total;
    uint32_t offset;
    bool     last;
    bool     has_sha;
    uint8_t  sha[32];
} chunk_hdr_t;

static bool parse_chunk(httpd_req_t *req, chunk_hdr_t *h, esp_err_t *ret)
{
    memset(h, 0, sizeof(*h));
    if (!hdr_u32(req, "X-OTA-Size", (uint32_t)req->content_len, &h->total) ||
        !hdr_u32(req, "X-Offset", 0, &h->offset)) {
        *ret = send_error(req, 400, "bad_request", "invalid size or offset");
        return false;
    }
    h->last = hdr_last(req);
    char sha_hex[65] = {0};
    esp_err_t sha_err = httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", sha_hex, sizeof(sha_hex));
    if (sha_err != ESP_ERR_NOT_FOUND) {
        if (sha_err != ESP_OK || httpd_req_get_hdr_value_len(req, "X-OTA-SHA256") != 64 ||
            !upload_sha_from_hex(sha_hex, h->sha)) {
            *ret = send_error(req, 400, "bad_request", "X-OTA-SHA256 must be 64 hex chars");
            return false;
        }
        h->has_sha = true;
    }
    if (h->offset > h->total || (uint32_t)req->content_len > h->total - h->offset) {
        *ret = send_error(req, 400, "bad_request", "X-Offset / body exceed X-OTA-Size");
        return false;
    }
    return true;
}

static bool chunk_continues(api_up_kind_t kind, const chunk_hdr_t *h, const char *path)
{
    return s_up_kind == kind && upload_sink_is_open(&s_up) &&
           s_up.expected == h->total && s_up.received == h->offset &&
           s_up.has_sha == h->has_sha &&
           (!h->has_sha || memcmp(s_up.sha_expected, h->sha, 32) == 0) &&
           (!path || strcmp(s_up.path, path) == 0);
}

static esp_err_t send_bad_offset(httpd_req_t *req, api_up_kind_t kind)
{
    char detail[48];
    if (s_up_kind == kind && upload_sink_is_open(&s_up)) {
        snprintf(detail, sizeof(detail), "expected offset %u", (unsigned)s_up.received);
    } else {
        strlcpy(detail, "no upload in progress; start at offset 0", sizeof(detail));
    }
    return send_error(req, 409, "bad_offset", detail);
}

/* Receive this request's body. Returns true when the upload is complete and
 * verified; otherwise the reply (nextOffset or error) has been sent. */
static bool chunk_receive(httpd_req_t *req, const chunk_hdr_t *h, esp_err_t *ret)
{
    esp_err_t err = upload_sink_recv(&s_up, req, false);
    if (err != ESP_OK) {
        // Network errors can retry at h->offset; a damaged file cannot.
        *ret = send_sink_err(req, err);
        if (s_up.failed) up_release();
        return false;
    }
    if (!h->last && s_up.received < s_up.expected) {
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":true,\"nextOffset\":%u}", (unsigned)s_up.received);
        httpd_resp_set_type(req, "application/json");
        *ret = httpd_resp_sendstr(req, json);
        return false;
    }
    err = upload_sink_verify(&s_up);
    if (err != ESP_OK) {
        *ret = err == ESP_ERR_INVALID_CRC
            ? send_error(req, 400, "sha_mismatch", "payload digest differs from X-OTA-SHA256")
            : send_error(req, 400, "size_mismatch", "upload ended before X-OTA-Size bytes");
        up_release();
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* GET info                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t h_info(httpd_req_t *req, const char *arg)
{
    (void)arg;
    jbuf_t j;
    jb_init(&j, 2048);
    jb_raw(&j, "{\"ok\":true");

    const esp_app_desc_t *app = esp_app_get_description();
    jb_open(&j, "firmware", '{');
    jb_kstr(&j, "version", app->version);
    jb_kstr(&j, "build", OBD_GAUGE_BUILD_TAG);
    jb_ku64(&j, "count", (uint64_t)OBD_GAUGE_GIT_COUNT);
    jb_kstr(&j, "branch", OBD_GAUGE_GIT_BRANCH);
    jb_kstr(&j, "hash", OBD_GAUGE_GIT_HASH);
    jb_kstr(&j, "idf", app->idf_ver);
    jb_kstr(&j, "date", app->date);
    jb_kstr(&j, "time", app->time);
    jb_putc(&j, '}');

    const device_identity_t *id = device_identity_get();
    jb_open(&j, "board", '{');
    jb_kstr(&j, "name", id->board_name);
    jb_kstr(&j, "variant", id->board_variant);
    jb_kstr(&j, "lcd", id->lcd_name);
    jb_ku64(&j, "width", id->screen_width);
    jb_ku64(&j, "height", id->screen_height);
    jb_ku64(&j, "flash_mb", id->flash_mb);
    jb_ku64(&j, "psram_mb", id->psram_mb);
    jb_putc(&j, '}');

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    jb_open(&j, "partitions", '{');
    jb_kstr(&j, "running", running ? running->label : "");
    jb_kstr(&j, "next", next ? next->label : "");
    jb_kstr(&j, "resource_storage", "sd");
    jb_ku64(&j, "theme_size", 0);  // retired writable Flash cache
    jb_ku64(&j, "bootmedia_size", 0);
    jb_ku64(&j, "bootmedia_free", 0);
    jb_kbool(&j, "bootmedia_video", sd_card_exists(SD_BOOT_OBV) ||
             sd_card_exists(SD_BOOT_JPEG) || sd_card_exists(SD_BOOT_LEGACY));
    jb_putc(&j, '}');

    bool sd = sd_card_is_mounted();
    jb_open(&j, "sd", '{');
    jb_kstr(&j, "state", sd_state_name(sd_card_get_state()));
    jb_ku64(&j, "total", sd ? sd_card_get_total_bytes() : 0);
    jb_ku64(&j, "free", sd ? sd_card_get_free_bytes() : 0);
    jb_putc(&j, '}');

    size_t used = 0, total = 0;
    bool cfg = config_part_is_mounted() && config_part_get_usage(&used, &total) == ESP_OK;
    jb_open(&j, "config", '{');
    jb_kbool(&j, "mounted", cfg);
    jb_ku64(&j, "used", used);
    jb_ku64(&j, "total", total);
    jb_putc(&j, '}');

    jb_open(&j, "vehicle", '{');
    jb_kstr(&j, "active", vp_manager_active_id());
    jb_kstr(&j, "state", veh_state_name(vp_manager_active_state()));
    jb_kbool(&j, "safe_mode", vp_manager_safe_mode());
    jb_kstr(&j, "last_error", vp_manager_last_error());
    jb_putc(&j, '}');

    theme_info_t ti;
    memset(&ti, 0, sizeof(ti));
    bool loaded = theme_get_info(&ti) == ESP_OK;
    jb_open(&j, "theme", '{');
    jb_kbool(&j, "loaded", loaded);
    jb_kstr(&j, "id", ti.id);
    jb_kstr(&j, "name", ti.name);
    jb_kstr(&j, "version", ti.version);
    jb_kstr(&j, "author", ti.author);
    jb_kbool(&j, "sidecar", loaded && theme_sidecar_present());
    jb_putc(&j, '}');

    size_t cd_size = 0;
    bool cd = coredump_store_present(&cd_size) && cd_size > 0;
    jb_open(&j, "coredump", '{');
    jb_kbool(&j, "present", cd);
    jb_ku64(&j, "size", cd ? cd_size : 0);
    jb_putc(&j, '}');

    jb_ku64(&j, "uptime_s", (uint64_t)(esp_timer_get_time() / 1000000));
    jb_open(&j, "heap", '{');
    jb_ku64(&j, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    jb_ku64(&j, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    jb_ku64(&j, "psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    jb_putc(&j, '}');
    jb_kstr(&j, "ssid", ota_wifi_server_get_ssid());
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

/* ------------------------------------------------------------------ */
/* Vehicles                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t h_vehicles_list(httpd_req_t *req, const char *arg)
{
    (void)arg;
    vpm_entry_t *list = heap_caps_calloc(API_VEH_LIST_MAX, sizeof(*list),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!list) return send_error(req, 500, "no_memory", "vehicle list");
    int n = vp_manager_list(list, API_VEH_LIST_MAX);

    jbuf_t j;
    jb_init(&j, 1024);
    jb_raw(&j, "{\"ok\":true");
    jb_kstr(&j, "active", vp_manager_active_id());
    jb_kstr(&j, "state", veh_state_name(vp_manager_active_state()));
    jb_kbool(&j, "safe_mode", vp_manager_safe_mode());
    jb_kstr(&j, "last_error", vp_manager_last_error());
    jb_open(&j, "vehicles", '[');
    for (int i = 0; i < n; i++) {
        const vpm_entry_t *e = &list[i];
        jb_open(&j, NULL, '{');
        jb_kstr(&j, "id", e->id);
        jb_kstr(&j, "name", e->name);
        jb_ku64(&j, "rev", e->rev);
        jb_kbool(&j, "builtin", e->builtin);
        jb_kbool(&j, "active", e->active);
        jb_kbool(&j, "pending", e->pending);
        jb_kstr(&j, "state", veh_state_name(e->state));
        jb_putc(&j, '}');
    }
    jb_raw(&j, "]}");
    free(list);
    return send_jb(req, 200, &j);
}

static void jb_issues(jbuf_t *j, const vp_issues_t *issues)
{
    jb_ku64(j, "errors", issues->n_errors);
    jb_ku64(j, "warnings", issues->n_warnings);
    jb_open(j, "issues", '[');
    for (size_t i = 0; i < issues->count; i++) {
        const vp_issue_t *it = &issues->items[i];
        jb_open(j, NULL, '{');
        jb_kstr(j, "code", it->code);
        jb_kstr(j, "severity", it->severity == VP_SEV_ERROR ? "error" : "warning");
        jb_kstr(j, "path", it->path);
        jb_kstr(j, "msg", it->msg);
        jb_putc(j, '}');
    }
    jb_putc(j, ']');
}

// POST vehicles — body: vehicle profile JSON. Validates, compiles, stores; not activated.
static esp_err_t h_vehicles_install(httpd_req_t *req, const char *arg)
{
    (void)arg;
    if (req->content_len == 0) return send_error(req, 400, "empty_body", "profile JSON expected");

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, VP_MAX_JSON_BYTES, &body, &len);
    if (err != ESP_OK) return send_body_err(req, err, VP_MAX_JSON_BYTES);

    vp_issue_t *items = heap_caps_calloc(API_VEH_ISSUES, sizeof(*items),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) {
        free(body);
        return send_error(req, 500, "no_memory", "issue list");
    }
    vp_issues_t issues;
    vp_issues_init(&issues, items, API_VEH_ISSUES);

    char id[VPM_ID_LEN] = {0};
    err = vp_manager_install_json(body, len, &issues, id, sizeof(id));
    free(body);

    jbuf_t j;
    jb_init(&j, 1024);
    int status = 200;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "vehicle %s installed (%u warnings)", id, (unsigned)issues.n_warnings);
        jb_raw(&j, "{\"ok\":true");
        jb_kstr(&j, "id", id);
    } else if (err == ESP_ERR_INVALID_ARG) {
        status = 422;
        jb_raw(&j, "{\"ok\":false");
        jb_kstr(&j, "error", "invalid_profile");
        jb_kstr(&j, "detail", vp_manager_last_error());
    } else {
        jb_free(&j);
        free(items);
        return send_vpm_err(req, err);
    }
    jb_issues(&j, &issues);
    jb_putc(&j, '}');
    free(items);
    return send_jb(req, status, &j);
}

static esp_err_t send_pending(httpd_req_t *req, const char *id)
{
    jbuf_t j;
    jb_init(&j, 128);
    jb_raw(&j, "{\"ok\":true");
    jb_kstr(&j, "pending", id);
    jb_kbool(&j, "reboot_required", true);
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

// POST vehicles/activate {"id":"..."} — takes effect on the next boot.
static esp_err_t h_vehicles_activate(httpd_req_t *req, const char *arg)
{
    (void)arg;
    char id[VPM_ID_LEN];
    esp_err_t ret = ESP_OK;
    if (!body_string(req, "id", id, sizeof(id), &ret)) return ret;
    if (!valid_vehicle_id(id)) return send_error(req, 400, "bad_id", id);
    esp_err_t err = vp_manager_activate(id);
    return err == ESP_OK ? send_pending(req, id) : send_vpm_err(req, err);
}

static esp_err_t h_vehicles_reset(httpd_req_t *req, const char *arg)
{
    (void)arg;
    esp_err_t err = vp_manager_reset_to_generic();
    if (err != ESP_OK) return send_vpm_err(req, err);
    first_boot_migration_drop_vehicle();   /* same as RESET.TXT: cancel a pending migrated vehicle */
    return send_pending(req, VPM_GENERIC_ID);
}

static esp_err_t h_vehicles_rollback(httpd_req_t *req, const char *arg)
{
    (void)arg;
    esp_err_t err = vp_manager_rollback();
    if (err != ESP_OK) return send_vpm_err(req, err);

    char pending[VPM_ID_LEN] = {0};
    vpm_entry_t *list = heap_caps_calloc(API_VEH_LIST_MAX, sizeof(*list),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (list) {
        int n = vp_manager_list(list, API_VEH_LIST_MAX);
        for (int i = 0; i < n; i++) {
            if (list[i].pending) {
                strlcpy(pending, list[i].id, sizeof(pending));
                break;
            }
        }
        free(list);
    }
    return send_pending(req, pending);
}

// GET vehicles/<id> — stored source JSON.
static esp_err_t h_vehicle_get(httpd_req_t *req, const char *id)
{
    if (!valid_vehicle_id(id)) return send_error(req, 400, "bad_id", id);
    char *json = NULL;
    size_t len = 0;
    esp_err_t err = vp_manager_read_json(id, &json, &len);
    if (err != ESP_OK) return send_esp_err(req, err, id);
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, json, (ssize_t)len);
    free(json);
    return err;
}

/* ------------------------------------------------------------------ */
/* Theme sidecar                                                       */
/* ------------------------------------------------------------------ */

// POST theme/sidecar — chunked upload of a sidecar or a full .gaugeproj.json.
// Up to 64 KB is kept in PSRAM; larger files stream to an SD temp file.
static esp_err_t h_sidecar_put(httpd_req_t *req, const char *arg)
{
    (void)arg;
    if (!sd_card_is_mounted()) {
        return send_error(req, 409, "sd_unavailable", "an SD card is required to store theme sidecars");
    }
    chunk_hdr_t h;
    esp_err_t ret = ESP_OK;
    if (!parse_chunk(req, &h, &ret)) return ret;

    if (h.offset == 0) {
        up_release();
        if (h.total == 0) return send_error(req, 400, "empty_body", "sidecar JSON expected");
        if (h.total > API_SIDECAR_MAX) return send_error(req, 413, "too_large", "sidecar larger than 32 MB");
        esp_err_t err;
        if (h.total <= API_SIDECAR_MEM_MAX) {
            err = upload_sink_open(&s_up, UPLOAD_SINK_MEMORY, h.total, h.has_sha ? h.sha : NULL, NULL, 0);
        } else {
            if (sd_card_get_free_bytes() < (uint64_t)h.total + API_SD_MARGIN) {
                return send_error(req, 507, "sd_full", "not enough SD space");
            }
            err = upload_sink_open(&s_up, UPLOAD_SINK_SD_FILE, h.total, h.has_sha ? h.sha : NULL,
                                   API_SIDECAR_TMP, 0);
        }
        if (err != ESP_OK) {
            ret = send_sink_err(req, err);
            up_release();
            return ret;
        }
        s_up_kind = API_UP_SIDECAR;
    } else if (!chunk_continues(API_UP_SIDECAR, &h, NULL)) {
        return send_bad_offset(req, API_UP_SIDECAR);
    }

    if (!chunk_receive(req, &h, &ret)) return ret;

    char *report = heap_caps_calloc(1, API_REPORT_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!report) {
        up_release();
        return send_error(req, 500, "no_memory", "report buffer");
    }
    esp_err_t err;
    if (s_up.kind == UPLOAD_SINK_MEMORY) {
        err = theme_sidecar_install((const char *)s_up.buf, s_up.received, report, API_REPORT_LEN);
    } else {
        err = upload_sink_commit(&s_up);   // .part -> API_SIDECAR_TMP
        if (err == ESP_OK) {
            err = theme_sidecar_install_file(API_SIDECAR_TMP, report, API_REPORT_LEN);
            sd_card_remove(API_SIDECAR_TMP);
        } else {
            ret = send_sink_err(req, err);
            up_release();
            free(report);
            return ret;
        }
    }
    uint32_t size = s_up.received;
    up_release();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sidecar install failed: %s (%s)", esp_err_to_name(err), report);
        ret = err == ESP_ERR_INVALID_ARG
            ? send_error(req, 422, "invalid_sidecar", report[0] ? report : esp_err_to_name(err))
            : send_esp_err(req, err, report);
        free(report);
        return ret;
    }
    ESP_LOGI(TAG, "sidecar installed (%u bytes)", (unsigned)size);
    jbuf_t j;
    jb_init(&j, API_REPORT_LEN + 64);
    jb_raw(&j, "{\"ok\":true");
    jb_ku64(&j, "size", size);
    jb_kstr(&j, "report", report);
    jb_putc(&j, '}');
    free(report);
    return send_jb(req, 200, &j);
}

// DELETE theme/sidecar[?theme=<id>] — current theme when omitted.
static esp_err_t h_sidecar_delete(httpd_req_t *req, const char *arg)
{
    (void)arg;
    char theme[THEME_ID_LEN + 1];
    bool named = query_param(req, "theme", theme, sizeof(theme)) && theme[0];
    esp_err_t err = theme_sidecar_remove(named ? theme : NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, 409, "sd_unavailable", "an SD card is required to remove theme sidecars");
    }
    if (err != ESP_OK) return send_esp_err(req, err, named ? theme : "current theme");
    return send_ok(req);
}

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

// PUT/POST fs/put?path= — chunked raw body into /sdcard/OBD/... or /config/...
static esp_err_t h_fs_put(httpd_req_t *req, const char *arg)
{
    (void)arg;
    char path[UPLOAD_PATH_MAX];
    esp_err_t ret = ESP_OK;
    api_fs_t fs = resolve_path(req, NULL, false, path, &ret);
    if (fs == API_FS_NONE) return ret;

    chunk_hdr_t h;
    if (!parse_chunk(req, &h, &ret)) return ret;

    if (h.offset == 0) {
        up_release();
        if (fs == API_FS_SD && sd_card_get_free_bytes() < (uint64_t)h.total + API_SD_MARGIN) {
            return send_error(req, 507, "sd_full", "not enough SD space");
        }
        esp_err_t err = upload_sink_open(&s_up, fs == API_FS_SD ? UPLOAD_SINK_SD_FILE : UPLOAD_SINK_CONFIG_FILE,
                                         h.total, h.has_sha ? h.sha : NULL, path, 0);
        if (err != ESP_OK) {
            if (fs == API_FS_CONFIG && err == ESP_ERR_INVALID_SIZE) {
                ret = send_error(req, 507, "config_full", s_up.err);
            } else {
                ret = send_sink_err(req, err);
            }
            up_release();
            return ret;
        }
        s_up_kind = API_UP_FS;
    } else if (!chunk_continues(API_UP_FS, &h, path)) {
        return send_bad_offset(req, API_UP_FS);
    }

    if (!chunk_receive(req, &h, &ret)) return ret;

    esp_err_t err = upload_sink_commit(&s_up);
    if (err != ESP_OK) {
        ret = send_sink_err(req, err);
        up_release();
        return ret;
    }
    uint32_t size = s_up.received;
    up_release();
    ESP_LOGI(TAG, "file written: %s (%u bytes)", path, (unsigned)size);

    jbuf_t j;
    jb_init(&j, 256);
    jb_raw(&j, "{\"ok\":true");
    jb_kstr(&j, "path", path);
    jb_ku64(&j, "size", size);
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

// GET fs/list[?path=] — directory listing (default /sdcard/OBD).
static esp_err_t h_fs_list(httpd_req_t *req, const char *arg)
{
    (void)arg;
    char path[UPLOAD_PATH_MAX];
    esp_err_t ret = ESP_OK;
    if (resolve_path(req, SD_OBD_ROOT, true, path, &ret) == API_FS_NONE) return ret;

    DIR *dir = opendir(path);
    if (!dir) return send_error(req, 404, "not_found", path);

    char *full = malloc(UPLOAD_PATH_MAX + 256);
    jbuf_t j;
    jb_init(&j, 2048);
    if (!full) j.oom = true;
    jb_raw(&j, "{\"ok\":true");
    jb_kstr(&j, "path", path);
    jb_open(&j, "entries", '[');
    int count = 0;
    bool truncated = false;
    struct dirent *de;
    while (!j.oom && (de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (count >= API_LIST_MAX) {
            truncated = true;
            break;
        }
        snprintf(full, UPLOAD_PATH_MAX + 256, "%s/%s", path, de->d_name);
        struct stat st;
        bool have_stat = stat(full, &st) == 0;
        bool is_dir = have_stat ? S_ISDIR(st.st_mode) : (de->d_type == DT_DIR);
        jb_open(&j, NULL, '{');
        jb_kstr(&j, "name", de->d_name);
        jb_kbool(&j, "dir", is_dir);
        jb_ku64(&j, "size", (have_stat && !is_dir) ? (uint64_t)st.st_size : 0);
        jb_putc(&j, '}');
        count++;
    }
    closedir(dir);
    free(full);
    jb_putc(&j, ']');
    jb_kbool(&j, "truncated", truncated);
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

// GET logs[?bytes=N] — tail of the SD import log as text/plain.
static esp_err_t h_logs(httpd_req_t *req, const char *arg)
{
    (void)arg;
    if (!sd_card_is_mounted()) {
        return send_error(req, 409, "sd_unavailable", sd_state_name(sd_card_get_state()));
    }
    size_t want = API_LOG_DEFAULT;
    char num[12];
    if (query_param(req, "bytes", num, sizeof(num))) {
        long v = strtol(num, NULL, 10);
        if (v > 0) want = (size_t)v < API_LOG_MAX ? (size_t)v : API_LOG_MAX;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    FILE *f = fopen(SD_IMPORT_LOG, "rb");
    if (!f) return httpd_resp_send(req, "", 0);

    long size = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    if (size <= 0) {
        fclose(f);
        return httpd_resp_send(req, "", 0);
    }
    long start = size > (long)want ? size - (long)want : 0;
    size_t len = (size_t)(size - start);
    char *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return send_error(req, 500, "no_memory", "log buffer");
    }
    size_t got = (fseek(f, start, SEEK_SET) == 0) ? sd_card_read(f, buf, len) : 0;
    fclose(f);
    buf[got] = '\0';

    // Start at a line boundary when the head was cut.
    const char *text = buf;
    if (start > 0) {
        const char *nl = memchr(buf, '\n', got);
        if (nl) text = nl + 1;
    }
    esp_err_t err = httpd_resp_send(req, text, (ssize_t)(got - (size_t)(text - buf)));
    free(buf);
    return err;
}

static esp_err_t h_sd(httpd_req_t *req, const char *arg)
{
    (void)arg;
    bool mounted = sd_card_is_mounted();
    jbuf_t j;
    jb_init(&j, 192);
    jb_raw(&j, "{\"ok\":true");
    jb_kstr(&j, "state", sd_state_name(sd_card_get_state()));
    jb_ku64(&j, "total", mounted ? sd_card_get_total_bytes() : 0);
    jb_ku64(&j, "free", mounted ? sd_card_get_free_bytes() : 0);
    jb_ku64(&j, "freq_khz", mounted ? sd_card_get_freq_khz() : 0);
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

/* ------------------------------------------------------------------ */
/* System                                                              */
/* ------------------------------------------------------------------ */

// POST time {"epoch":<unix seconds>} — the gauge has no network time source.
static esp_err_t h_time(httpd_req_t *req, const char *arg)
{
    (void)arg;
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, 256, &body, &len);
    if (err != ESP_OK) return send_body_err(req, err, 256);
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *epoch = root ? cJSON_GetObjectItemCaseSensitive(root, "epoch") : NULL;
    double v = cJSON_IsNumber(epoch) ? epoch->valuedouble : 0;
    cJSON_Delete(root);
    if (v < (double)API_EPOCH_MIN || v > 4102444800.0) {
        return send_error(req, 400, "bad_request", "expected {\"epoch\":<unix seconds>}");
    }

    struct timeval tv = { .tv_sec = (time_t)v, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return send_error(req, 500, "failed", "settimeofday");
    }
    ESP_LOGI(TAG, "time set: %lld", (long long)tv.tv_sec);
    jbuf_t j;
    jb_init(&j, 64);
    jb_raw(&j, "{\"ok\":true");
    jb_ku64(&j, "epoch", (uint64_t)tv.tv_sec);
    jb_putc(&j, '}');
    return send_jb(req, 200, &j);
}

static esp_err_t h_reboot(httpd_req_t *req, const char *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "reboot requested over /api/v1");
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the reply leave the radio
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

typedef esp_err_t (*api_route_fn)(httpd_req_t *req, const char *arg);

typedef struct {
    const char  *path;     // relative to /api/v1/
    bool         prefix;   // path is a prefix; the rest is passed as arg
    int          method;
    api_route_fn fn;
} api_route_t;

static const api_route_t s_routes[] = {
    { "info",              false, HTTP_GET,    h_info },
    { "vehicles",          false, HTTP_GET,    h_vehicles_list },
    { "vehicles",          false, HTTP_POST,   h_vehicles_install },
    { "vehicles/activate", false, HTTP_POST,   h_vehicles_activate },
    { "vehicles/reset",    false, HTTP_POST,   h_vehicles_reset },
    { "vehicles/rollback", false, HTTP_POST,   h_vehicles_rollback },
    { "vehicles/",         true,  HTTP_GET,    h_vehicle_get },
    { "theme/sidecar",     false, HTTP_POST,   h_sidecar_put },
    { "theme/sidecar",     false, HTTP_DELETE, h_sidecar_delete },
    { "fs/put",            false, HTTP_PUT,    h_fs_put },
    { "fs/put",            false, HTTP_POST,   h_fs_put },
    { "fs/list",           false, HTTP_GET,    h_fs_list },
    { "logs",              false, HTTP_GET,    h_logs },
    { "sd",                false, HTTP_GET,    h_sd },
    { "time",              false, HTTP_POST,   h_time },
    { "reboot",            false, HTTP_POST,   h_reboot },
};

#define API_ROUTE_COUNT (sizeof(s_routes) / sizeof(s_routes[0]))

static esp_err_t api_dispatch(httpd_req_t *req)
{
    // Include CORS on auth/validation failures as well, so the browser can
    // report the real device error. Other methods and endpoints stay closed.
    if (req->method == HTTP_POST && vehicle_web_cors_path(req->uri)) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }
    if (!s_auth || !s_auth(req)) {
        return send_error(req, 401, "unauthorized", "missing or invalid X-OTA-Token");
    }

    const char *rel = req->uri + strlen(API_PREFIX);
    size_t n = strcspn(rel, "?#");
    if (n == 0 || n >= API_PATH_MAX) return send_error(req, 404, "not_found", "unknown endpoint");
    char path[API_PATH_MAX];
    memcpy(path, rel, n);
    path[n] = '\0';

    // Exact paths first (a wrong method there is 405), then prefix routes.
    bool path_known = false;
    for (size_t i = 0; i < API_ROUTE_COUNT; i++) {
        const api_route_t *r = &s_routes[i];
        if (r->prefix || strcmp(path, r->path) != 0) continue;
        path_known = true;
        if ((int)req->method == r->method) return r->fn(req, NULL);
    }
    for (size_t i = 0; !path_known && i < API_ROUTE_COUNT; i++) {
        const api_route_t *r = &s_routes[i];
        size_t plen = strlen(r->path);
        if (!r->prefix || strncmp(path, r->path, plen) != 0 || path[plen] == '\0') continue;
        path_known = true;
        if ((int)req->method == r->method) return r->fn(req, path + plen);
    }
    if (path_known) return send_error(req, 405, "method_not_allowed", path);
    return send_error(req, 404, "not_found", path);
}

esp_err_t api_v1_register_handlers(httpd_handle_t server, api_v1_auth_fn auth)
{
    if (!server || !auth) return ESP_ERR_INVALID_ARG;
    s_auth = auth;

    static const httpd_method_t methods[] = { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE };
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        httpd_uri_t def = {
            .uri = API_PREFIX "*",
            .method = methods[i],
            .handler = api_dispatch,
        };
        esp_err_t err = httpd_register_uri_handler(server, &def);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register " API_PREFIX "* (method %d) failed: %s",
                     (int)methods[i], esp_err_to_name(err));
            if (first_err == ESP_OK) first_err = err;
        }
    }
    return first_err;
}

bool api_v1_upload_busy(void)
{
    return upload_sink_is_open(&s_up);
}

void api_v1_reset(void)
{
    up_release();
}
