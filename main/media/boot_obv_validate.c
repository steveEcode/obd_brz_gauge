#include "boot_obv_validate.h"

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#ifdef ESP_PLATFORM
#include "bsp_obd_dsp/sd_card.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_obd_dsp/sd_io_guard.h"
#define read_bytes sd_card_read
static void *buffer_alloc(size_t n) { return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
#else
static size_t read_bytes(FILE *f, void *p, size_t n) { return fread(p, 1, n, f); }
static void *buffer_alloc(size_t n) { return malloc(n); }
#endif

static bool jpeg_sampling(const uint8_t *p, size_t n, uint8_t sampling, uint16_t *w, uint16_t *h)
{
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8 || p[n - 2] != 0xff || p[n - 1] != 0xd9) return false;
    size_t at = 2;
    bool sof = false;
    while (at + 4 <= n) {
        if (p[at++] != 0xff) return false;
        while (at < n && p[at] == 0xff) at++;
        if (at >= n) return false;
        uint8_t marker = p[at++];
        if (at + 2 > n) return false;
        size_t len = ((size_t)p[at] << 8) | p[at + 1];
        if (len < 2 || len > n - at) return false;
        if (marker == 0xda) return sof && len == 12 && p[at + 2] == 3;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (marker != 0xc0 || sof || len != 17 || p[at + 2] != 8 || p[at + 7] != 3 ||
                p[at + 9] != (sampling == OBV1_SUB_422 ? 0x21 : 0x22) ||
                p[at + 12] != 0x11 || p[at + 15] != 0x11) return false;
            *h = ((uint16_t)p[at + 3] << 8) | p[at + 4];
            *w = ((uint16_t)p[at + 5] << 8) | p[at + 6];
            sof = true;
        }
        at += len;
    }
    return false;
}

/* These strings are returned over USB/HTTP; keep them static, not a shared
 * mutable buffer: callers retain the reason after validation has returned. */
static const char *header_failure(int err)
{
    switch (err) {
    case OBV1_ERR_MAGIC: return "Invalid OBV2 magic (expected OBV1)";
    case OBV1_ERR_VERSION: return "Unsupported OBV2 version or header size";
    case OBV1_ERR_CRC: return "OBV2 header CRC mismatch";
    case OBV1_ERR_GEOMETRY: return "Invalid OBV2 geometry or header fields";
    case OBV1_ERR_FRAME_SIZE: return "OBV2 frame exceeds supported size";
    case OBV1_ERR_TRUNCATED: return "OBV2 file length does not match header";
    default: return "Invalid OBV2 data size";
    }
}

bool boot_obv_validate_file(const char *path, const char **reason)
{
    return boot_obv_validate_file_detailed(path, reason, NULL, 0);
}

bool boot_obv_validate_file_detailed(const char *path, const char **reason,
                                    char *detail, size_t detail_size)
{
    const char *failure = "Cannot open staged OBV2 file on SD";
    bool ok = false;
    uint8_t header[OBV1_HEADER_SIZE];
    uint8_t *index = NULL;
    uint8_t *frame = NULL;
    uint8_t *pixels = NULL;
    media_jpeg_t *decoder = NULL;
    obv1_info_t info = {0};
    long length = -1;
    long long offset = -1;
    size_t expected = 0, actual = 0;
    if (detail && detail_size) detail[0] = '\0';
    int parser_err = OBV1_OK;
    esp_err_t jpeg_err = ESP_OK;
    uint32_t frame_no = UINT32_MAX;
#ifdef ESP_PLATFORM
    sd_io_trace_t sd_trace;
    sd_io_trace_begin(&sd_trace);
#endif
    errno = 0;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) goto done;

    failure = "Cannot seek staged OBV2 file on SD";
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    failure = "Cannot determine staged OBV2 file size";
    length = ftell(f);
    if (length < 0) goto done;
    failure = "Invalid OBV2 file size (64 bytes to 256 MiB required)";
    if (length < (long)sizeof(header) || (uint64_t)length > 256u * 1024u * 1024u) goto done;
    failure = "Cannot seek OBV2 header on SD";
    offset = 0;
    errno = 0;
    if (fseek(f, 0, SEEK_SET) != 0) goto done;
    failure = "Cannot read OBV2 header from SD";
    expected = sizeof(header);
    errno = 0;
    actual = read_bytes(f, header, expected);
    if (actual != expected) goto done;
    parser_err = obv1_parse_header(header, sizeof(header), (uint64_t)length, OBV1_FRAME_MAX, &info);
    if (parser_err != OBV1_OK) { failure = header_failure(parser_err); goto done; }
    failure = "Boot animation requires OBV2 (version 2)";
    if (info.version != OBV2_VERSION) goto done;
    failure = "OBV2 file length does not match header";
    if ((uint64_t)info.data_offset + info.data_size != (uint64_t)length) goto done;

    size_t index_size = (size_t)info.frame_count * OBV2_INDEX_ENTRY;
    failure = "Not enough memory for OBV2 index";
    errno = 0;
    index = buffer_alloc(index_size);
    if (!index) goto done;
    failure = "Cannot seek OBV2 index on SD";
    offset = info.index_offset;
    expected = actual = 0;
    errno = 0;
    if (fseek(f, (long)info.index_offset, SEEK_SET) != 0) goto done;
    failure = "Cannot read OBV2 index from SD";
    expected = index_size;
    errno = 0;
    actual = read_bytes(f, index, expected);
    if (actual != expected) goto done;
    parser_err = obv1_check_index(&info, index, index_size);
    if (parser_err != OBV1_OK) {
        failure = parser_err == OBV1_ERR_CRC ? "OBV2 index CRC mismatch" : "Invalid OBV2 frame or timing index";
        goto done;
    }
    failure = "Not enough memory for OBV2 frame";
    errno = 0;
    frame = buffer_alloc(info.max_frame_size);
    if (!frame) goto done;
    size_t pixel_cap = (size_t)((info.width + 15u) & ~15u) * ((info.height + 15u) & ~15u) * 2u;
    failure = "Not enough memory for OBV2 pixels";
    errno = 0;
    pixels = buffer_alloc(pixel_cap);
    if (!pixels) goto done;
    errno = 0;
    jpeg_err = media_jpeg_open(false, &decoder);
    if (jpeg_err != ESP_OK) {
        failure = jpeg_err == ESP_ERR_NO_MEM ? "Not enough memory for JPEG decoder" : "Cannot initialize JPEG decoder";
        goto done;
    }
    uint32_t crc = 0;
    for (uint32_t i = 0; i < info.frame_count; i++) {
        frame_no = i;
        uint32_t off, size;
        obv_index_frame(&info, index, i, &off, &size);
        uint64_t pos = (uint64_t)info.data_offset + off;
        failure = "Cannot seek OBV2 frame on SD";
        offset = (long long)pos;
        expected = actual = 0;
        errno = 0;
        if (pos > LONG_MAX || fseek(f, (long)pos, SEEK_SET) != 0) goto done;
        failure = "Cannot read OBV2 frame from SD";
        expected = size;
        errno = 0;
        actual = read_bytes(f, frame, expected);
        if (actual != expected) goto done;
        crc = obv1_crc32(crc, frame, size);
        uint16_t w = 0, h = 0;
        media_jpeg_info_t decoded;
        failure = "Frame is not supported baseline JPEG";
        if (!jpeg_sampling(frame, size, info.subsampling, &w, &h) || w != info.width || h != info.height) goto done;
        errno = 0;
        jpeg_err = media_jpeg_begin(decoder, frame, size, &decoded);
        if (jpeg_err == ESP_OK && (decoded.width != info.width || decoded.height != info.height)) goto done;
        if (jpeg_err == ESP_OK && obv_frame_time(index, i) < OBV2_TIMESCALE)
            jpeg_err = media_jpeg_decode_frame(decoder, pixels, pixel_cap);
        if (jpeg_err != ESP_OK) {
            if (jpeg_err == ESP_ERR_NO_MEM) failure = "Not enough memory to decode JPEG frame";
            goto done;
        }
#ifdef ESP_PLATFORM
        if (!(i % 8)) vTaskDelay(1);
#endif
    }
    failure = "OBV2 data CRC mismatch";
    if (crc != info.data_crc32) goto done;
    ok = true;
done:;  // Capture before formatting/logging/cleanup can change errno.
    int saved_errno = errno;
    int at_eof = f ? !!feof(f) : 0;
    int stream_error = f ? !!ferror(f) : 0;
    if (!ok && detail && detail_size) {
        snprintf(detail, detail_size,
                 "%s [frame=%ld/%lu offset=%lld expected=%lu actual=%lu bytes=%ld errno=%d eof=%d ferror=%d parser=%d jpeg=%d]",
                 failure, frame_no == UINT32_MAX ? -1L : (long)frame_no,
                 (unsigned long)info.frame_count, offset,
                 (unsigned long)expected, (unsigned long)actual, length,
                 saved_errno, at_eof, stream_error, parser_err, (int)jpeg_err);
    }
#ifdef ESP_PLATFORM
    sd_io_trace_end(&sd_trace);
    if (!ok && detail && detail_size) {
        size_t used = strlen(detail);
        sd_io_trace_format(&sd_trace, detail + used, detail_size - used);
    }
#endif
    if (reason) *reason = ok ? NULL : failure;
#ifdef ESP_PLATFORM
    if (!ok) {
        /* Log before cleanup changes errno. Frame UINT32_MAX means no frame
         * was reached. Heap diagnostics distinguish resource pressure from a
         * malformed container without weakening installation validation. */
        ESP_LOGE("boot_obv", "%s: path=%s bytes=%ld frame=%u parser=%s jpeg=%s errno=%d",
                 failure, path ? path : "(null)", length, (unsigned)frame_no,
                 obv1_strerror(parser_err), esp_err_to_name(jpeg_err), saved_errno);
        ESP_LOGE("boot_obv", "largest free blocks: internal=%u PSRAM=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
#else
    (void)frame_no;
#endif
    if (decoder) media_jpeg_close(decoder);
    free(index);
    free(frame);
    free(pixels);
    if (f) fclose(f);
    return ok;
}
