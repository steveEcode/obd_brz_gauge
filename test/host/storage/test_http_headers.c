#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_obd_dsp/http_u32.h"
#include "media/boot_obv_validate.h"

#include "bsp_obd_dsp/sd_io_guard.h"
static bool sd_trace_failure;
int sd_io_trace_current(sd_io_trace_t *trace) { memset(trace, 0, sizeof(*trace)); return sd_trace_failure; }
void sd_io_trace_format(const sd_io_trace_t *trace, char *out, size_t cap)
{ (void)trace; if (out && cap) snprintf(out, cap, " sd=ESP_ERR_TIMEOUT(0x107) op=R lba=123 sectors=8"); }
#define ESP_ERR_HTTPD_RESULT_TRUNC 2
typedef struct {
    const char *size, *offset, *sha, *last, *type;
    size_t content_len;
} httpd_req_t;
static const char *header(httpd_req_t *r, const char *name)
{
    if (!strcmp(name, "X-OTA-Size")) return r->size;
    if (!strcmp(name, "X-Offset")) return r->offset;
    if (!strcmp(name, "X-OTA-SHA256")) return r->sha;
    if (!strcmp(name, "X-Last")) return r->last;
    if (!strcmp(name, "Content-Type")) return r->type;
    assert(false); return NULL;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *name)
{ const char *s = header(r, name); return s ? strlen(s) : 0; }
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t cap)
{
    const char *s = header(r, name);
    if (!s) return ESP_ERR_NOT_FOUND;
    snprintf(out, cap, "%s", s);
    /* Match IDF 5.5.3, including its len == cap truncation edge. */
    return strlen(s) > cap ? ESP_ERR_HTTPD_RESULT_TRUNC : ESP_OK;
}
static char response[BOOT_OBV_DETAIL_SIZE + 16];
static esp_err_t send_json_response(httpd_req_t *r, int status, const char *text)
{ (void)r; assert(status == 400 && strlen(text) < sizeof(response));
  strcpy(response, text); return -1; }
static esp_err_t send_error(httpd_req_t *r, int status, const char *code, const char *text)
{ (void)r; (void)code; (void)text; assert(status == 400); return -1; }
static bool upload_sha_from_hex(const char *hex, uint8_t out[32])
{ memset(out, 0, 32); return strlen(hex) == 64; }
#include "http_headers.inc"

static bool ota_parse(httpd_req_t *r)
{
    uint8_t sha[32]; uint32_t size = 0, offset = 0;
    bool last = false, base64 = false; esp_err_t ret = 0;
    return parse_chunk_headers(r, sha, &size, &offset, &last, &base64, &ret);
}
static bool api_parse(httpd_req_t *r)
{ chunk_hdr_t h; esp_err_t ret = 0; return parse_chunk(r, &h, &ret); }
int main(void)
{
    httpd_req_t empty = {0};
    char detail[BOOT_OBV_DETAIL_SIZE], expected_json[BOOT_OBV_DETAIL_SIZE + 16];
    memset(detail, 'X', sizeof(detail) - 1); detail[sizeof(detail) - 1] = 0;
    snprintf(expected_json, sizeof(expected_json), "{\"error\":\"%s\"}", detail);
    assert(send_err(&empty, detail) == -1 && !strcmp(response, expected_json));
    assert(send_err(&empty, "quote\" slash\\ newline\n") == -1);
    assert(!strcmp(response, "{\"error\":\"quote\\\" slash\\\\ newline\\u000a\"}"));
    char oversized[1000]; memset(oversized, '\n', sizeof(oversized) - 1); oversized[999] = 0;
    assert(send_err(&empty, oversized) == -1);
    assert(!strncmp(response, "{\"error\":\"", 10));
    assert(!strcmp(response + strlen(response) - 2, "\"}"));
    assert((strlen(response) - 12) % 6 == 0); // No partial escape sequence.
    sd_trace_failure = true;
    assert(send_err(&empty, "write failed") == -1);
    assert(strstr(response, "write failed sd=ESP_ERR_TIMEOUT(0x107) op=R lba=123 sectors=8"));
    assert(send_err(&empty, "read failed sd=ESP_ERR_TIMEOUT(0x107)") == -1);
    assert(!strstr(strstr(response, " sd=") + 1, " sd=")); // No duplicated suffix.
    sd_trace_failure = false;
    uint32_t out = 7;
    assert(http_parse_u32("0", &out) && out == 0);
    assert(http_parse_u32("4294967295", &out) && out == UINT32_MAX);
    const char *bad[] = {"", "-1", "+1", "1x", " 1", "1 ", "0x10", "1.2", "4294967296", "999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        out = 7; assert(!http_parse_u32(bad[i], &out) && out == 7);
    }
    httpd_req_t r = {.size = "100", .sha = "0000000000000000000000000000000000000000000000000000000000000000", .content_len = 10};
    assert(ota_parse(&r) && api_parse(&r));
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        r.offset = bad[i]; assert(!ota_parse(&r) && !api_parse(&r));
        r.offset = NULL; r.size = bad[i]; assert(!ota_parse(&r) && !api_parse(&r)); r.size = "100";
    }
    /* Exactly buffer-size digits must not be silently truncated to offset 0. */
    r.offset = "0000000000000001"; assert(!ota_parse(&r) && !api_parse(&r));
    r.offset = "90"; assert(ota_parse(&r) && api_parse(&r));
    r.offset = "91"; assert(!ota_parse(&r) && !api_parse(&r));
    r.offset = NULL;
    r.sha = ""; assert(!ota_parse(&r) && !api_parse(&r));
    r.sha = "00000000000000000000000000000000000000000000000000000000000000000";
    assert(!ota_parse(&r) && !api_parse(&r));
    r.sha = "000000000000000000000000000000000000000000000000000000000000000000";
    assert(!ota_parse(&r) && !api_parse(&r));
    r.sha = NULL; assert(!ota_parse(&r) && api_parse(&r));
    r.offset = NULL; r.size = NULL; assert(!ota_parse(&r) && api_parse(&r));
    return 0;
}
