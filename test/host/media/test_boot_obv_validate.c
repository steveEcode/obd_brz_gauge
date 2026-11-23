#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "media/boot_obv_validate.h"
#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#include "media/media_jpeg_selftest.inc"

/* Execute production code with deterministic IO/allocation faults. The wrappers
 * affect only the included validator, not fixture creation or the JPEG stubs. */
enum fault_op { FAULT_NONE, FAULT_OPEN, FAULT_SEEK, FAULT_TELL, FAULT_READ, FAULT_ALLOC };
static enum fault_op fault;
static unsigned fault_at, fault_calls;
static bool partial_eof_read;
static bool inject(enum fault_op op)
{
    if (fault != op || ++fault_calls != fault_at) return false;
    errno = op == FAULT_ALLOC ? ENOMEM : EIO;
    return true;
}
static FILE *fault_fopen(const char *path, const char *mode)
{ return inject(FAULT_OPEN) ? NULL : fopen(path, mode); }
static int fault_fseek(FILE *f, long off, int whence)
{ return inject(FAULT_SEEK) ? -1 : fseek(f, off, whence); }
static long fault_ftell(FILE *f) { return inject(FAULT_TELL) ? -1 : ftell(f); }
static size_t fault_fread(void *p, size_t size, size_t n, FILE *f)
{
    if (!inject(FAULT_READ)) return fread(p, size, n, f);
    if (!partial_eof_read) return 0;
    size_t got = fread(p, size, n / 2, f);
    assert(fseek(f, 0, SEEK_END) == 0 && fgetc(f) == EOF);
    errno = 0; // EOF is not necessarily an IO errno.
    return got;
}
static void *fault_malloc(size_t n) { return inject(FAULT_ALLOC) ? NULL : malloc(n); }
#define fopen fault_fopen
#define fseek fault_fseek
#define ftell fault_ftell
#define fread fault_fread
#define malloc fault_malloc
#include "media/boot_obv_validate.c"
#undef fopen
#undef fseek
#undef ftell
#undef fread
#undef malloc

/* Real container/JPEG headers, substituting only the hardware decoder boundary. */
struct media_jpeg { int unused; };
static struct media_jpeg decoder;
static unsigned opens, closes, begins, decodes;
static bool fail_begin, fail_decode;
static esp_err_t open_error;
esp_err_t media_jpeg_open(bool block, media_jpeg_t **out)
{
    assert(!block); *out = NULL;
    if (open_error != ESP_OK) return open_error;
    opens++; *out = &decoder; return ESP_OK;
}
void media_jpeg_close(media_jpeg_t *j) { assert(j == &decoder); closes++; }
esp_err_t media_jpeg_begin(media_jpeg_t *j, const uint8_t *data, size_t len, media_jpeg_info_t *info)
{
    assert(j == &decoder && len > 4 && data[0] == 255);
    begins++;
    *info = (media_jpeg_info_t){.width = 360, .height = 360, .block_len = 368 * 368 * 2, .blocks = 1};
    return fail_begin ? ESP_FAIL : ESP_OK;
}
esp_err_t media_jpeg_decode_frame(media_jpeg_t *j, uint8_t *out, size_t cap)
{
    assert(j == &decoder && out && cap >= 368 * 368 * 2);
    decodes++;
    /* Exercise the padded output capacity required by the 4:2:0 MCU geometry. */
    memset(out, 0, cap);
    return fail_decode ? ESP_FAIL : ESP_OK;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    for (unsigned n = 0; n < 4; n++) p[n] = (uint8_t)(v >> (n * 8));
}
static uint8_t *build(size_t *length, unsigned frames, uint8_t sampling)
{
    const uint8_t *jpeg = sampling == OBV1_SUB_420 ? k_st_jpeg_420 : k_st_jpeg_422;
    size_t size = sampling == OBV1_SUB_420 ? sizeof(k_st_jpeg_420) : sizeof(k_st_jpeg_422);
    size_t data_off = 64 + frames * 16;
    *length = data_off + size * frames;
    uint8_t *blob = calloc(1, *length);
    assert(blob);
    memcpy(blob, "OBV1", 4);
    wr16(blob + 4, 2); wr16(blob + 6, 64); wr16(blob + 8, 360); wr16(blob + 10, 360);
    blob[14] = sampling; blob[15] = sampling == OBV1_SUB_422 ? OBV1_FLAG_BLOCK_SAFE : 0;
    wr32(blob + 16, frames); wr32(blob + 20, 64); wr32(blob + 24, (uint32_t)data_off);
    wr32(blob + 28, (uint32_t)(size * frames)); wr32(blob + 32, (uint32_t)size);
    wr32(blob + 36, frames * 600); wr32(blob + 48, 1000000);
    for (unsigned i = 0; i < frames; i++) {
        uint8_t *entry = blob + 64 + i * 16;
        wr32(entry, (uint32_t)(size * i)); wr32(entry + 4, (uint32_t)size);
        wr32(entry + 8, i * 600000); wr32(entry + 12, 600000);
        memcpy(blob + data_off + size * i, jpeg, size);
    }
    wr32(blob + 40, obv1_crc32(0, blob + 64, frames * 16));
    wr32(blob + 44, obv1_crc32(0, blob + data_off, size * frames));
    wr32(blob + 60, obv1_crc32(0, blob, 60));
    return blob;
}
static bool validate_detailed(const uint8_t *blob, size_t len, const char **reason,
                              char *detail, size_t cap)
{
    char path[] = "/tmp/boot-obv-validate-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *file = fdopen(fd, "wb"); assert(file);
    assert(fwrite(blob, 1, len, file) == len); assert(fclose(file) == 0);
    fault_calls = 0;
    bool ok = detail ? boot_obv_validate_file_detailed(path, reason, detail, cap) :
                       boot_obv_validate_file(path, reason);
    assert(unlink(path) == 0);
    assert(opens == closes);
    return ok;
}
static bool validate(const uint8_t *blob, size_t len, const char **reason)
{
    return validate_detailed(blob, len, reason, NULL, 0);
}
static void check_details(const uint8_t *blob, size_t len)
{
    char detail[BOOT_OBV_DETAIL_SIZE], saved[BOOT_OBV_DETAIL_SIZE], expected_text[BOOT_OBV_DETAIL_SIZE];
    const char *reason = NULL;
    size_t frame_size = sizeof(k_st_jpeg_420);
    fault = FAULT_READ;
    fault_at = 4; // Second frame, zero-based frame=1.
    assert(!validate_detailed(blob, len, &reason, detail, sizeof(detail)));
    snprintf(expected_text, sizeof(expected_text),
             "Cannot read OBV2 frame from SD [frame=1/3 offset=%lu expected=%lu actual=0 bytes=%lu errno=%d eof=0 ferror=0 parser=0 jpeg=0]",
             (unsigned long)(112 + frame_size), (unsigned long)frame_size, (unsigned long)len, EIO);
    assert(strcmp(detail, expected_text) == 0);
    assert(strcmp(reason, "Cannot read OBV2 frame from SD") == 0);
    memcpy(saved, detail, sizeof(saved));
    partial_eof_read = true;
    assert(!validate_detailed(blob, len, NULL, detail, sizeof(detail)));
    snprintf(expected_text, sizeof(expected_text), "actual=%lu", (unsigned long)(frame_size / 2));
    assert(strstr(detail, expected_text));
    assert(strstr(detail, "errno=0 eof=1 ferror=0"));
    partial_eof_read = false;
    fault = FAULT_NONE;
    // Caller-owned diagnostics and the legacy static reason survive another run.
    assert(validate_detailed(blob, len, NULL, detail, sizeof(detail)) && !detail[0]);
    assert(strstr(saved, "frame=1/3"));
    assert(strcmp(reason, "Cannot read OBV2 frame from SD") == 0);
    char tiny[] = {'A', 'B', 'C'};
    assert(!boot_obv_validate_file_detailed(NULL, &reason, tiny, 0));
    assert(memcmp(tiny, "ABC", 3) == 0);
    assert(!boot_obv_validate_file_detailed(NULL, NULL, tiny, 1));
    assert(tiny[0] == 0 && tiny[1] == 'B' && tiny[2] == 'C');
    assert(!boot_obv_validate_file_detailed(NULL, NULL, tiny, 2));
    assert(tiny[0] == 'C' && tiny[1] == 0 && tiny[2] == 'C');
    assert(!boot_obv_validate_file_detailed(NULL, NULL, NULL, 123));
    assert(!boot_obv_validate_file_detailed(NULL, NULL, detail, sizeof(detail)));
    assert(strstr(detail, "frame=-1/0 offset=-1 expected=0 actual=0 bytes=-1"));
}
static size_t marker_at(uint8_t *blob, size_t len, size_t start, uint8_t marker)
{
    for (size_t n = start; n + 1 < len; n++) if (blob[n] == 255 && blob[n + 1] == marker) return n;
    assert(!"JPEG fixture marker missing"); return 0;
}
static void check_fault(const uint8_t *blob, size_t len, enum fault_op op, unsigned at, const char *expected)
{
    const char *reason = NULL;
    fault = op; fault_at = at;
    assert(!validate(blob, len, &reason));
    assert(reason && strcmp(reason, expected) == 0);
    fault = FAULT_NONE;
    /* Each rejection must release its resources and leave the next run usable. */
    assert(validate(blob, len, NULL));
}

int main(void)
{
    size_t len; const char *reason;
    uint8_t *blob = build(&len, 3, OBV1_SUB_420);
    assert(validate(blob, len, &reason)); assert(reason == NULL);
    assert(begins == 3 && decodes == 2); /* all headers, full decode for first second */
    check_fault(blob, len, FAULT_OPEN, 1, "Cannot open staged OBV2 file on SD");
    check_fault(blob, len, FAULT_SEEK, 1, "Cannot seek staged OBV2 file on SD");
    check_fault(blob, len, FAULT_TELL, 1, "Cannot determine staged OBV2 file size");
    check_fault(blob, len, FAULT_SEEK, 2, "Cannot seek OBV2 header on SD");
    check_fault(blob, len, FAULT_READ, 1, "Cannot read OBV2 header from SD");
    check_fault(blob, len, FAULT_ALLOC, 1, "Not enough memory for OBV2 index");
    check_fault(blob, len, FAULT_SEEK, 3, "Cannot seek OBV2 index on SD");
    check_fault(blob, len, FAULT_READ, 2, "Cannot read OBV2 index from SD");
    check_fault(blob, len, FAULT_ALLOC, 2, "Not enough memory for OBV2 frame");
    check_fault(blob, len, FAULT_ALLOC, 3, "Not enough memory for OBV2 pixels");
    for (unsigned n = 0; n < 3; n++) {
        check_fault(blob, len, FAULT_SEEK, n + 4, "Cannot seek OBV2 frame on SD");
        check_fault(blob, len, FAULT_READ, n + 3, "Cannot read OBV2 frame from SD");
    }
    check_details(blob, len);
    open_error = ESP_ERR_NO_MEM;
    assert(!validate(blob, len, &reason));
    assert(strcmp(reason, "Not enough memory for JPEG decoder") == 0);
    open_error = ESP_FAIL;
    assert(!validate(blob, len, &reason));
    assert(strcmp(reason, "Cannot initialize JPEG decoder") == 0);
    open_error = ESP_OK;
    assert(!boot_obv_validate_file(NULL, &reason)); assert(strstr(reason, "open"));

    /* File/header/index corruption must not be mistaken for IO or resource faults. */
    blob[0] ^= 1;
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "magic")); blob[0] ^= 1;
    blob[60] ^= 1;
    assert(!validate(blob, len, &reason)); assert(strcmp(reason, "OBV2 header CRC mismatch") == 0); blob[60] ^= 1;
    blob[64] ^= 1;
    assert(!validate(blob, len, &reason)); assert(strcmp(reason, "OBV2 index CRC mismatch") == 0); blob[64] ^= 1;
    /* A correct index CRC does not excuse a discontinuous timeline. */
    wr32(blob + 64 + 8, 1);
    wr32(blob + 40, obv1_crc32(0, blob + 64, 3 * 16));
    wr32(blob + 60, obv1_crc32(0, blob, 60));
    assert(!validate(blob, len, &reason)); assert(strcmp(reason, "Invalid OBV2 frame or timing index") == 0);
    wr32(blob + 64 + 8, 0);
    wr32(blob + 40, obv1_crc32(0, blob + 64, 3 * 16));
    wr32(blob + 60, obv1_crc32(0, blob, 60));

    assert(!validate(blob, len - 1, &reason) && reason);
    uint8_t *extra = calloc(1, len + 1); assert(extra); memcpy(extra, blob, len);
    assert(!validate(extra, len + 1, &reason)); free(extra);

    /* Pixel data corruption with valid JPEG markers must still fail CRC. */
    blob[len - 4] ^= 1;
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "CRC"));
    blob[len - 4] ^= 1;
    fail_decode = true;
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "JPEG"));
    fail_decode = false; fail_begin = true;
    assert(!validate(blob, len, &reason)); fail_begin = false;
    free(blob);

    blob = build(&len, 1, OBV1_SUB_422);
    assert(validate(blob, len, &reason)); free(blob);

    blob = build(&len, 1, OBV1_SUB_420);
    size_t sof = marker_at(blob, len, 80, 0xc0);
    blob[sof + 1] = 0xc2; /* progressive masquerading as baseline */
    assert(!validate(blob, len, &reason)); assert(strstr(reason, "JPEG"));
    blob[sof + 1] = 0xc0; blob[sof + 11] = 0x21; /* sampling mismatch */
    assert(!validate(blob, len, &reason)); free(blob);

    blob = build(&len, 1, OBV1_SUB_420);
    size_t sos = marker_at(blob, len, 80, 0xda);
    blob[sos + 1] = 0xd9; /* SOF without a supported scan */
    assert(!validate(blob, len, &reason)); free(blob);
    printf("OBV2 install validation: IO/resource faults distinguished from header/index/CRC/JPEG rejection\n");
    return 0;
}
