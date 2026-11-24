/*
 * video_player.c — OBV1 MJPEG playback engine (see video_player.h).
 *
 * One session at a time. Two tasks share a PSRAM ring:
 *   reader  (core 0, priority 3): header + index, then frame records from SD
 *   decoder (core 1, mode-dependent priority): paces, decodes and presents
 *
 * FULLSCREEN: LVGL's refresh timer is paused and the decoder writes the
 *   panel itself. esp_new_jpeg block mode (8/16-line MCU rows) decodes into
 *   two internal DMA strips (LVGL's own draw buffers when they qualify, else
 *   allocated for the session) that ping-pong through
 *   esp_lcd_panel_draw_bitmap. Exactly one strip is in flight at a time:
 *   draw_buf->flushing is set before each draw and cleared by the
 *   flush-done ISR (lv_disp_flush_ready), so it doubles as the transfer-idle
 *   flag and LVGL finds it clear when it resumes. PSRAM never reaches the
 *   panel: the whole-frame fallback copies lines into the strips.
 * COMPOSITE: whole frames into two PSRAM buffers; publication wakes the LVGL
 *   task to hand the newest one to video_view before its normal timer handler.
 *
 * Ring records: [u32 frame idx][u32 size][jpeg, padded to 4]. A record never
 * wraps: idx 0xFFFFFFFF marks "continue at 0", and fewer than 8 bytes before
 * the end wrap implicitly. Skipped tail bytes count as used until consumed.
 *
 * Pacing: frame i is due at t0 + i / fps (t0 rebased after buffering, pause
 * and catch-up). Native cadence is preserved unless target_fps is specified.
 * Only frames more than one source period late are dropped; decode cost does
 * not impose an additional coarse frame-rate tier.
 *
 * Lifetime: the session is refcounted (API, reader, decoder); the last
 * reference frees it, so stop() never waits for the reader's SD I/O. In
 * FULLSCREEN stop() does not resume LVGL until the decoder releases the
 * borrowed draw buffers and any in-flight panel transfer has completed.
 * All public calls are made from the LVGL task (release_all takes lvgl_mux
 * when the caller does not hold it).
 */

#include "app_obd_dsp/perf_stats.h"
#include "app_obd_dsp/lvgl_runtime.h"
#include "media/video_player.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "media/media_jpeg.h"
#include "media/obv1_format.h"
#include "media/video_player_internal.h"
#include "media/video_ring_peek.h"
#include "media/video_view.h"

static const char *TAG = "video_player";

/* Periodic playback statistics are useful during video tuning but are noisy
 * during normal OBD operation. Enable with -DVP_DIAG_LOG=1 when needed. */
#ifndef VP_DIAG_LOG
#define VP_DIAG_LOG 0
#endif

extern SemaphoreHandle_t lvgl_mux;   /* app_main.c */

#define VP_LCD_W             EXAMPLE_LCD_WIDTH
#define VP_LCD_H             EXAMPLE_LCD_HEIGHT
#define VP_STRIP_MAX_BYTES   (EXAMPLE_LCD_WIDTH * 40 * 2)  /* one LVGL band = one SPI transfer */

#define VP_RING_MIN          (256u * 1024u)
#define VP_RING_MAX          (1280u * 1024u)
#define VP_THEME_RING_MAX    (2u * 1024u * 1024u)
#define VP_THEME_RESERVE     (512u * 1024u)
/* A sector-aligned 4 KiB DMA window leaves internal RAM for BLE queues while
 * compressed frames and read-ahead remain in the PSRAM ring. */
#define VP_SD_BUFFER_BYTES   (4u * 1024u)
#define VP_SD_BUFFER_MIN     512u
#define VP_REC_HDR           8u
#define VP_REC_WRAP          0xFFFFFFFFu

#define VP_WATERMARK_MS      400u
#define VP_STATS_US          5000000
#define VP_MAX_DEC_ERRORS    10u

#define VP_FLUSH_WAIT_MS     200u
#define VP_STOP_WAIT_MS      1000u
#define VP_READER_RETRY_MS   20u

#define VP_READER_STACK      4096
#define VP_DECODER_STACK     6144
#define VP_BOOT_WORKER_PRIO  5u
#define VP_THEME_WORKER_PRIO 3u
#define VP_SD_READER_PRIO    3u

#define EV_WAKE_READER       BIT0
#define EV_WAKE_DECODER      BIT1
#define EV_DECODER_DONE      BIT2

typedef struct {
    uint64_t bytes;
    uint64_t total_us;
    uint64_t max_us;
    uint32_t frames;
} vp_io_stats_t;

typedef struct {
    /* config, fixed after start */
    video_mode_t mode;
    uint32_t     id;             /* LVGL-owned identity, never a borrowed pointer */
    bool         loop;
    uint8_t      target_fps;
    lv_obj_t    *view;
    char         path[VIDEO_PLAYER_PATH_MAX];
    int64_t      t_start;
    bool         value_driven;
    double       position;      /* guarded by lock; latest requested position */
    uint32_t     position_revision; /* changes even when A -> B -> A outruns reader */

    portMUX_TYPE       lock;
    int                refs;
    EventGroupHandle_t ev;

    volatile bool    quit;
    volatile bool    paused;
    volatile bool    started;      /* first frame presented */
    volatile uint8_t state;        /* video_state_t */

    /* reader -> decoder, published under lock with hdr_ready */
    obv1_info_t   info;
    uint8_t      *timed_index; /* immutable until last worker releases session */
    volatile bool hdr_ready;
    volatile bool eof;
    volatile bool reader_err;

    /* ring (PSRAM); positions and counters under lock */
    uint8_t      *ring;
    uint32_t      ring_cap;
    uint32_t      wpos, rpos, used;
    uint32_t      frames_buffered;
    volatile bool ring_full;       /* reader waits for space */

    /* FULLSCREEN */
    lv_disp_t          *disp;
    lv_disp_draw_buf_t *draw_buf;
    uint8_t            *strip[2];  /* internal DMA, 16-aligned */
    uint32_t            strip_cap;
    bool                strip_owned;
    bool                refr_paused;
    lv_timer_t         *refr_timer;
    lv_timer_cb_t       refr_cb;

    /* COMPOSITE (fb[0] doubles as the FULLSCREEN whole-frame buffer) */
    uint8_t    *fb[2];             /* PSRAM, 16-aligned */
    uint32_t    fb_cap;
    uint8_t    *alpha_tmp;         /* one decoded alpha plane for OBV3 */
    bool        pending_alpha;
    int8_t      front;             /* buffer shown by the view, -1 none (lock) */
    int8_t      pending;           /* decoded, not yet handed over, -1 none (lock) */

    /* Reader updates / decoder snapshots under lock; excludes ring-space waits. */
    vp_io_stats_t io;
    int64_t ui_last_us;
    uint64_t ui_gap_us;
    uint64_t ui_gap_max_us;
    uint32_t ui_intervals;

    /* Decoder-task-only counters, emitted in the periodic debug log. */
    uint32_t frames_dropped;
    uint32_t underruns;
} vp_session_t;

/* Decoder output state (decoder task only). */
typedef struct {
    media_jpeg_t *j;
    bool          block;        /* esp_new_jpeg block (strip) mode */
    bool          prepared;     /* strips / frame buffer sized for this video */
    uint16_t      x0, y0;       /* video origin on the panel */
    uint16_t      strip_lines;  /* lines per panel draw */
    uint8_t       next;         /* strip that is not in flight */
    uint32_t      pace_wait_us; /* deadline sleep, excluded from decode diagnostics */
} vp_out_t;

/* Decoder pacing state (decoder task only). */
typedef struct {
    int64_t  t0;                /* wall clock of stream time 0 */
    uint64_t pos;               /* records consumed */
    uint64_t last_slot;         /* last presented target_fps time bucket */
    int64_t  fps_t;
    uint32_t fps_n;
    uint32_t ema;
    uint32_t samples;
    uint32_t ui_ema;
    uint32_t late_run;
    bool     shown;
    bool     buffering;
    bool     rebase;
} vp_pace_t;

static vp_session_t *s_cur;
static uint32_t s_next_session_id;
/* A stopped session can still be inside SD I/O. Do not let its successor
 * allocate another internal DMA buffer until that reader has released it. */
static portMUX_TYPE s_reader_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_reader_gate_owned;

/* ------------------------------------------------------------- session */

static void session_free(vp_session_t *s)
{
    if (s->strip_owned) {
        heap_caps_free(s->strip[0]);
        heap_caps_free(s->strip[1]);
    }
    heap_caps_free(s->fb[0]);
    heap_caps_free(s->fb[1]);
    heap_caps_free(s->alpha_tmp);
    heap_caps_free(s->ring);
    heap_caps_free(s->timed_index);
    if (s->ev) vEventGroupDelete(s->ev);
    heap_caps_free(s);
}

static void session_ref(vp_session_t *s)
{
    taskENTER_CRITICAL(&s->lock);
    s->refs++;
    taskEXIT_CRITICAL(&s->lock);
}

static void session_unref(vp_session_t *s)
{
    taskENTER_CRITICAL(&s->lock);
    int left = --s->refs;
    taskEXIT_CRITICAL(&s->lock);
    if (left == 0) session_free(s);
}

/* One consumer per wake bit; events latch across the predicate/wait gap.
 * Each worker retains the session/event group until it exits. Header/data,
 * ring release, UI handoff and pause/resume/stop all notify their waiter. */
static void wait_ev(vp_session_t *s, EventBits_t bit, TickType_t ticks)
{
    xEventGroupWaitBits(s->ev, bit, pdTRUE, pdFALSE, ticks);
}

static bool reader_gate_acquire(vp_session_t *s)
{
    while (!s->quit) {
        taskENTER_CRITICAL(&s_reader_gate_lock);
        bool acquired = !s_reader_gate_owned;
        if (acquired) s_reader_gate_owned = true;
        taskEXIT_CRITICAL(&s_reader_gate_lock);
        if (acquired) return true;
        /* A finite wait avoids blocking the LVGL task on SD I/O and lets a
         * superseded reader respond promptly to the normal stop notification. */
        wait_ev(s, EV_WAKE_READER, pdMS_TO_TICKS(VP_READER_RETRY_MS));
    }
    return false;
}

static void reader_gate_release(void)
{
    taskENTER_CRITICAL(&s_reader_gate_lock);
    s_reader_gate_owned = false;
    taskEXIT_CRITICAL(&s_reader_gate_lock);
}


static uint8_t *align16(void *p)
{
    return (uint8_t *)(((uintptr_t)p + 15u) & ~(uintptr_t)15u);
}

/* ---------------------------------------------------------------- ring */

/* Reserve need bytes (header + padded data); writes the wrap marker when the
 * tail is skipped. False when quitting. */
static bool ring_reserve(vp_session_t *s, uint32_t need, uint32_t *at, uint32_t *waste)
{
    for (;;) {
        if (s->quit) return false;
        taskENTER_CRITICAL(&s->lock);
        uint32_t pos = s->wpos;
        uint32_t tail = s->ring_cap - pos;
        uint32_t w = tail < need ? tail : 0;
        bool ok = s->ring_cap - s->used >= w + need;
        s->ring_full = !ok;
        taskEXIT_CRITICAL(&s->lock);
        if (ok) {
            if (w >= VP_REC_HDR) {
                uint32_t *h = (uint32_t *)(s->ring + pos);
                h[0] = VP_REC_WRAP;
                h[1] = 0;
            }
            *at = w ? 0 : pos;
            *waste = w;
            return true;
        }
        xEventGroupSetBits(s->ev, EV_WAKE_DECODER);   /* a full ring satisfies the watermark */
        wait_ev(s, EV_WAKE_READER, portMAX_DELAY);
    }
}

static void ring_commit(vp_session_t *s, uint32_t at, uint32_t need, uint32_t waste)
{
    taskENTER_CRITICAL(&s->lock);
    s->used += waste + need;
    s->wpos = at + need;
    if (s->wpos >= s->ring_cap) s->wpos = 0;
    s->frames_buffered++;
    taskEXIT_CRITICAL(&s->lock);
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
}

/* Oldest record, skipping wrap padding; false when the ring is empty. */
static bool ring_peek(vp_session_t *s, uint32_t *size, const uint8_t **data, uint32_t *rec_len)
{
    return vp_ring_peek_committed(s->ring, s->ring_cap, &s->rpos, &s->used,
                                 &s->lock, size, data, rec_len);
}

static void ring_release(vp_session_t *s, uint32_t rec_len)
{
    taskENTER_CRITICAL(&s->lock);
    s->used -= rec_len;
    s->rpos += rec_len;
    if (s->rpos >= s->ring_cap) s->rpos = 0;
    if (s->frames_buffered) s->frames_buffered--;
    s->ring_full = false;   /* re-evaluated by the reader's next reserve */
    taskEXIT_CRITICAL(&s->lock);
    xEventGroupSetBits(s->ev, EV_WAKE_READER);
}

/* -------------------------------------------------------------- reader */

static uint32_t ring_size_for(const obv1_info_t *in, bool theme_loop)
{
    uint64_t rec_max = (uint64_t)in->max_frame_size + VP_REC_HDR + 4u;
    uint64_t avg = in->data_size / in->frame_count;
    /* Theme loops prefetch ~2.5 s; legacy fullscreen retains ~1.25 s. */
    uint64_t want = avg * in->fps * 5u / (theme_loop ? 2u : 4u) + 2u * rec_max;
    uint32_t ceiling = theme_loop ? VP_THEME_RING_MAX : VP_RING_MAX;
    if (want < VP_RING_MIN) want = VP_RING_MIN;
    if (want > ceiling) want = ceiling;
    /* never more than the whole stream (short boot clips) */
    uint64_t whole = (uint64_t)in->data_size + (uint64_t)in->frame_count * (VP_REC_HDR + 4u) + rec_max;
    if (want > whole) want = whole;
    if (want < 2u * rec_max) want = 2u * rec_max;
    return (uint32_t)((want + 3u) & ~(uint64_t)3u);
}

/* The decoder allocates after hdr_ready. Leave room for both pixel buffers,
 * the OBV3 alpha plane and other theme/UI work before growing the SD ring. */
static uint32_t theme_ring_budget(const obv1_info_t *info, uint32_t wanted)
{
    size_t pixels = (size_t)info->width * info->height;
    bool alpha = info->version == OBV3_VERSION;
    size_t fb = (pixels * (alpha ? 3u : 2u) + 15u) & ~(size_t)15u;
    size_t reserve = 2u * fb + (alpha ? pixels : 0u) + VP_THEME_RESERVE;
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t available = free_psram > reserve ? (free_psram - reserve) & ~(size_t)3u : 0;
    return available < wanted ? (uint32_t)available : wanted;
}

/* Keep read-ahead across frame padding and small seeks. The target's
 * size-optimized stdio copies/refills inefficiently; feed FatFS bulk reads
 * directly into internal DMA memory, then copy committed payloads to PSRAM. */
typedef struct {
    int fd;
    uint8_t *buf;
    size_t cap;
    off_t begin;
    size_t used, pos;
} vp_sd_stream_t;

static bool sd_stream_seek(vp_sd_stream_t *f, off_t target)
{
    if (target >= f->begin && (uint64_t)(target - f->begin) <= f->used) {
        f->pos = (size_t)(target - f->begin);
        return true;
    }
    if (lseek(f->fd, target, SEEK_SET) != target) return false;
    f->begin = target;
    f->used = f->pos = 0;
    return true;
}

static size_t sd_stream_read(vp_sd_stream_t *f, void *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        if (f->pos == f->used) {
            ssize_t n;
            do { n = read(f->fd, f->buf, f->cap); }
            while (n < 0 && errno == EINTR);
            if (n <= 0) break;
            f->begin += (off_t)f->used;
            f->used = (size_t)n;
            f->pos = 0;
        }
        size_t n = len - got;
        if (n > f->used - f->pos) n = f->used - f->pos;
        memcpy((uint8_t *)dst + got, f->buf + f->pos, n);
        f->pos += n;
        got += n;
    }
    return got;
}

/* Open, validate, allocate the ring. Returns the index (PSRAM) or NULL. */
static uint8_t *reader_open(vp_session_t *s, vp_sd_stream_t *f)
{
    uint32_t hdr_words[OBV1_HEADER_SIZE / 4];
    uint8_t *hdr = (uint8_t *)hdr_words;
    obv1_info_t info;

    off_t fsize = lseek(f->fd, 0, SEEK_END);
    if (fsize <= 0 || lseek(f->fd, 0, SEEK_SET) != 0) return NULL;
    if (sd_stream_read(f, hdr, OBV1_HEADER_SIZE) != OBV1_HEADER_SIZE) {
        ESP_LOGW(TAG, "%s: short header", s->path);
        return NULL;
    }
    int rc = obv1_parse_header(hdr, OBV1_HEADER_SIZE, (uint64_t)fsize, 0, &info);
    if (rc != OBV1_OK) {
        ESP_LOGW(TAG, "%s: %s", s->path, obv1_strerror(rc));
        return NULL;
    }
    size_t ilen = (size_t)info.frame_count * obv_index_stride(&info);
    uint8_t *index = heap_caps_malloc(ilen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!index) {
        ESP_LOGE(TAG, "no PSRAM for index (%u B)", (unsigned)ilen);
        return NULL;
    }
    if (!sd_stream_seek(f, (off_t)info.index_offset) || sd_stream_read(f, index, ilen) != ilen) {
        ESP_LOGW(TAG, "%s: short index", s->path);
        heap_caps_free(index);
        return NULL;
    }
    rc = obv1_check_index(&info, index, ilen);
    if (rc != OBV1_OK) {
        ESP_LOGW(TAG, "%s: index %s", s->path, obv1_strerror(rc));
        heap_caps_free(index);
        return NULL;
    }

    bool timed = info.version == OBV2_VERSION;
    if (timed && (s->mode != VIDEO_MODE_FULLSCREEN || s->loop)) {
        heap_caps_free(index);
        return NULL;
    }
    if (info.version == OBV3_VERSION && (s->mode != VIDEO_MODE_COMPOSITE || !s->loop)) {
        heap_caps_free(index);
        return NULL;
    }
    bool theme_loop = s->mode == VIDEO_MODE_COMPOSITE && s->loop && !s->value_driven;
    uint32_t cap = timed ? obv_timed_ring_size(&info, index) : ring_size_for(&info, theme_loop);
    if (theme_loop) cap = theme_ring_budget(&info, cap);
    if (s->value_driven) cap = 3u * (info.max_frame_size + VP_REC_HDR + 4u);
    uint32_t floor = 2u * (info.max_frame_size + VP_REC_HDR + 4u);
    uint8_t *ring = NULL;
    while (!ring && cap >= floor) {
        ring = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ring) { if (timed) break; cap = (cap / 2u) & ~3u; }
    }
    if (!ring) {
        ESP_LOGE(TAG, "no PSRAM for the ring");
        heap_caps_free(index);
        return NULL;
    }

    ESP_LOGI(TAG, "SD ring %lu KiB%s", (unsigned long)(cap / 1024u),
             theme_loop ? " (theme target 2500 ms)" : "");
    taskENTER_CRITICAL(&s->lock);
    s->info = info;
    s->timed_index = timed ? index : NULL;
    s->ring = ring;
    s->ring_cap = cap;
    s->hdr_ready = true;
    taskEXIT_CRITICAL(&s->lock);
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
    return index;
}

/* The index is immutable after hdr_ready; both workers sample the same target.
 * A committed ring record remains immutable until ring_release by decoder. */
static uint32_t requested_frame_snapshot(vp_session_t *s, uint32_t *revision)
{
    taskENTER_CRITICAL(&s->lock);
    double p = s->position;
    if (revision) *revision = s->position_revision;
    taskEXIT_CRITICAL(&s->lock);
    return (uint32_t)floor(p * (s->info.frame_count - 1u) + 0.5);
}

static uint32_t requested_frame(vp_session_t *s)
{ return requested_frame_snapshot(s, NULL); }

static void reader_task(void *arg)
{
    vp_session_t *s = arg;
    uint8_t *index = NULL;
    bool ok = false;
    bool gate_owned = reader_gate_acquire(s);
    vp_sd_stream_t file = { .fd = gate_owned && !s->quit ? open(s->path, O_RDONLY) : -1 };
    if (!gate_owned || s->quit) goto done;
    if (file.fd < 0) ESP_LOGW(TAG, "cannot open %s", s->path);
    else {
        file.cap = VP_SD_BUFFER_BYTES;
        while (!file.buf && file.cap >= VP_SD_BUFFER_MIN) {
            file.buf = heap_caps_aligned_alloc(16, file.cap,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!file.buf && file.cap > VP_SD_BUFFER_MIN) file.cap /= 2u;
        }
        if (!file.buf) {
            const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
            ESP_LOGW(TAG, "cannot allocate SD read buffer for %s (internal DMA free=%lu largest=%lu)",
                     s->path, (unsigned long)heap_caps_get_free_size(caps),
                     (unsigned long)heap_caps_get_largest_free_block(caps));
        } else {
            if (file.cap < VP_SD_BUFFER_BYTES) {
                ESP_LOGW(TAG, "using reduced %lu-byte SD read buffer for %s",
                         (unsigned long)file.cap, s->path);
            }
            index = reader_open(s, &file);
        }
    }

    ok = index != NULL;
    uint32_t i = 0;
    uint32_t sent_revision = UINT32_MAX, revision = 0;
    while (ok && !s->quit) {
        if (s->value_driven) {
            i = requested_frame_snapshot(s, &revision);
            if (s->paused || revision == sent_revision) {
                wait_ev(s, EV_WAKE_READER, portMAX_DELAY);
                continue;
            }
        }
        if (i >= s->info.frame_count) {
            if (!s->loop) break;
            i = 0;
        }
        uint32_t off, size, at, waste;
        obv_index_frame(&s->info, index, i, &off, &size);
        uint32_t need = VP_REC_HDR + ((size + 3u) & ~3u);
        if (!ring_reserve(s, need, &at, &waste)) break;

        int64_t read_start = esp_timer_get_time();
        off_t target = (off_t)s->info.data_offset + (off_t)off;
        if (!sd_stream_seek(&file, target)) {
            ok = false;
            break;
        }
        uint32_t *h = (uint32_t *)(s->ring + at);
        h[0] = i;
        h[1] = size;
        if (sd_stream_read(&file, s->ring + at + VP_REC_HDR, size) != size) {
            ESP_LOGW(TAG, "%s: read failed at frame %lu", s->path, (unsigned long)i);
            ok = false;
            break;
        }
        uint64_t read_us = (uint64_t)(esp_timer_get_time() - read_start);
        taskENTER_CRITICAL(&s->lock);
        s->io.bytes += size;
        s->io.total_us += read_us;
        if (read_us > s->io.max_us) s->io.max_us = read_us;
        s->io.frames++;
        taskEXIT_CRITICAL(&s->lock);
        ring_commit(s, at, need, waste);
        sent_revision = revision;
        i++;
    }

done:
    if (file.fd >= 0) close(file.fd);
    heap_caps_free(file.buf);
    if (index != s->timed_index) heap_caps_free(index);
    if (gate_owned) reader_gate_release();
    if (!s->quit) {
        if (ok) s->eof = true;
        else s->reader_err = true;
    }
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
    session_unref(s);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------- fullscreen */

/* Wait until the last queued panel transfer (ours or LVGL's) has finished. */
static bool fs_wait_idle(vp_session_t *s, uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while (s->draw_buf->flushing) {
        int64_t el = esp_timer_get_time() - start;
        if (el > (int64_t)timeout_ms * 1000) return false;
        if (el < 2000) esp_rom_delay_us(20);   /* a strip takes < 1 ms on the wire */
        else vTaskDelay(1);
    }
    return true;
}

static bool fs_draw(vp_session_t *s, int x, int y, int w, int h, const uint8_t *buf)
{
    if (!fs_wait_idle(s, VP_FLUSH_WAIT_MS)) {
        ESP_LOGE(TAG, "panel transfer timed out");
        return false;
    }
    s->draw_buf->flushing = 1;   /* cleared by the flush-done ISR */
    if (esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, buf) != ESP_OK) {
        s->draw_buf->flushing = 0;
        return false;
    }
    return true;
}

/* Black border around a video smaller than the panel. */
static bool fs_clear(vp_session_t *s, vp_out_t *o)
{
    const uint32_t line = VP_LCD_W * 2u;
    uint32_t bytes = s->strip_cap < VP_STRIP_MAX_BYTES ? s->strip_cap : VP_STRIP_MAX_BYTES;
    uint32_t rows = (bytes / line) & ~1u;
    if (!rows) return false;
    memset(s->strip[0], 0, rows * line);
    for (uint32_t y = 0; y < VP_LCD_H; y += rows) {
        uint32_t n = VP_LCD_H - y < rows ? VP_LCD_H - y : rows;
        if (!fs_draw(s, 0, (int)y, VP_LCD_W, (int)n, s->strip[0])) return false;
    }
    o->next = 1;   /* strip 0 may still be in flight */
    return true;
}

/* Size strips (and the whole-frame buffer) from the first frame.
 * ESP_ERR_NOT_SUPPORTED: block mode does not fit, use whole frames. */
static esp_err_t fs_prepare(vp_session_t *s, vp_out_t *o, const media_jpeg_info_t *ji)
{
    if (!fs_wait_idle(s, VP_FLUSH_WAIT_MS)) return ESP_ERR_TIMEOUT;
    const uint32_t line = (uint32_t)ji->width * 2u;
    if (!s->strip[0]) {
        uint32_t cap = o->block && ji->block_len > line * 16u ? ji->block_len : line * 16u;
        cap = (cap + 15u) & ~15u;
        uint8_t *a = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        uint8_t *b = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!a || !b) {
            heap_caps_free(a);
            heap_caps_free(b);
            ESP_LOGE(TAG, "no internal DMA RAM for strips (2 x %lu B)", (unsigned long)cap);
            return ESP_ERR_NO_MEM;
        }
        s->strip[0] = a;
        s->strip[1] = b;
        s->strip_cap = cap;
        s->strip_owned = true;
    }

    if (o->block) {
        uint32_t row = (uint32_t)ji->mcu_rows * line;
        if (ji->block_len > s->strip_cap || row > VP_STRIP_MAX_BYTES) return ESP_ERR_NOT_SUPPORTED;
        /* k MCU rows per draw: (k-1) rows + one full block must fit the strip */
        uint32_t k = 1;
        while (k * row + ji->block_len <= s->strip_cap && (k + 1u) * row <= VP_STRIP_MAX_BYTES) k++;
        o->strip_lines = (uint16_t)(k * ji->mcu_rows);
    } else {
        uint32_t bytes = s->strip_cap < VP_STRIP_MAX_BYTES ? s->strip_cap : VP_STRIP_MAX_BYTES;
        uint32_t lines = (bytes / line) & ~1u;
        if (!lines) return ESP_ERR_NO_MEM;
        o->strip_lines = (uint16_t)lines;
        if (!s->fb[0]) {
            uint32_t cap = (ji->block_len + 15u) & ~15u;
            s->fb[0] = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s->fb[0]) return ESP_ERR_NO_MEM;
            s->fb_cap = cap;
        }
    }

    o->x0 = (uint16_t)((VP_LCD_W - ji->width) / 2u);    /* multiples of 4: w, h % 8 == 0 */
    o->y0 = (uint16_t)((VP_LCD_H - ji->height) / 2u);
    if ((ji->width < VP_LCD_W || ji->height < VP_LCD_H) && !fs_clear(s, o)) return ESP_ERR_TIMEOUT;
    o->prepared = true;
    ESP_LOGI(TAG, "fullscreen %s decode, %u lines per draw, %s strips (%lu B)",
             o->block ? "block" : "whole-frame", o->strip_lines,
             s->strip_owned ? "own" : "LVGL", (unsigned long)s->strip_cap);
    return ESP_OK;
}

static bool wait_until(vp_session_t *s, int64_t t);

static bool fs_wait_due(vp_session_t *s, vp_out_t *o, int64_t due)
{
    int64_t start = esp_timer_get_time();
    bool ready = wait_until(s, due);
    o->pace_wait_us += (uint32_t)(esp_timer_get_time() - start);
    return ready;
}

static esp_err_t fs_blocks(vp_session_t *s, vp_out_t *o, const media_jpeg_info_t *ji, int64_t due)
{
    const uint32_t line = (uint32_t)ji->width * 2u;
    uint32_t filled = 0, y = 0;
    for (uint32_t b = 0; b < ji->blocks; b++) {
        uint16_t n = 0;
        esp_err_t err = media_jpeg_next_block(o->j, s->strip[o->next] + filled * line, &n);
        if (err != ESP_OK) return err;
        filled += n;
        bool last = b + 1u == ji->blocks;
        if (filled && (last || filled + ji->mcu_rows > o->strip_lines)) {
            /* Decode into the free odd/even strip ahead of its deadline.
             * Start panel transfer at the deadline, never overwrite DMA data. */
            if (!y && !fs_wait_due(s, o, due)) return ESP_ERR_INVALID_STATE;
            if (!fs_draw(s, o->x0, o->y0 + (int)y, ji->width, (int)filled, s->strip[o->next])) {
                return ESP_ERR_TIMEOUT;
            }
            y += filled;
            filled = 0;
            o->next ^= 1u;
        }
    }
    return ESP_OK;
}

static esp_err_t fs_frame(vp_session_t *s, vp_out_t *o, const media_jpeg_info_t *ji, int64_t due)
{
    if (ji->block_len > s->fb_cap) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = media_jpeg_decode_frame(o->j, s->fb[0], s->fb_cap);
    if (err != ESP_OK) return err;
    if (!fs_wait_due(s, o, due)) return ESP_ERR_INVALID_STATE;
    const uint32_t line = (uint32_t)ji->width * 2u;
    for (uint32_t y = 0; y < ji->height; ) {
        uint32_t n = ji->height - y < o->strip_lines ? ji->height - y : o->strip_lines;
        memcpy(s->strip[o->next], s->fb[0] + (size_t)y * line, (size_t)n * line);
        if (!fs_draw(s, o->x0, o->y0 + (int)y, ji->width, (int)n, s->strip[o->next])) {
            return ESP_ERR_TIMEOUT;
        }
        o->next ^= 1u;
        y += n;
    }
    return ESP_OK;
}

static bool dec_frame_mode(vp_out_t *o)
{
    media_jpeg_close(o->j);
    o->j = NULL;
    o->block = false;
    o->prepared = false;
    return media_jpeg_open(false, &o->j) == ESP_OK;
}

static esp_err_t fs_present(vp_session_t *s, vp_out_t *o, const uint8_t *jpg, uint32_t len, int64_t due)
{
    media_jpeg_info_t ji;
    o->pace_wait_us = 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = media_jpeg_begin(o->j, jpg, len, &ji);
        if (err == ESP_OK && (ji.width != s->info.width || ji.height != s->info.height)) {
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK && !o->prepared) err = fs_prepare(s, o, &ji);
        if (err != ESP_ERR_NOT_SUPPORTED || !o->block) break;
        ESP_LOGW(TAG, "block decode unusable, falling back to whole frames");
        if (!dec_frame_mode(o)) return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) return err;
    err = o->block ? fs_blocks(s, o, &ji, due) : fs_frame(s, o, &ji, due);
    /* Publish first-frame readiness only after its last DMA strip is on-panel. */
    if (err == ESP_OK && !s->started && !fs_wait_idle(s, VP_FLUSH_WAIT_MS)) {
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

/* ----------------------------------------------------------- composite */

void video_player_ui_step(void)
{
    vp_session_t *s = s_cur;
    if (!s || s->mode != VIDEO_MODE_COMPOSITE || s->paused || s->quit) return;
    taskENTER_CRITICAL(&s->lock);
    int p = s->pending;
    bool has_alpha = s->pending_alpha;
    if (p >= 0) {
        int64_t now = esp_timer_get_time();
        if (s->ui_last_us) {
            uint64_t gap = (uint64_t)(now - s->ui_last_us);
            s->ui_gap_us += gap;
            if (gap > s->ui_gap_max_us) s->ui_gap_max_us = gap;
            s->ui_intervals++;
        }
        s->ui_last_us = now;
        s->front = (int8_t)p;
        s->pending = -1;
    }
    taskEXIT_CRITICAL(&s->lock);
    if (p < 0) return;
    /* Runs in the LVGL task, so no render is reading the old front now. */
    if (has_alpha) video_view_set_frame_alpha(s->view, s->fb[p], s->info.width, s->info.height);
    else video_view_set_frame(s->view, (const uint16_t *)s->fb[p], s->info.width, s->info.height);
    /* Invalidation resumes the timer but does not make its next deadline due.
     * A frame wake must render in this handler pass, not wait another 16 ms. */
    lv_timer_t *refresh = _lv_disp_get_refr_timer(lv_disp_get_default());
    if (refresh) lv_timer_ready(refresh);
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
}

/* Wait until the UI took the last published frame. */
static bool comp_wait_slot(vp_session_t *s)
{
    for (;;) {
        if (s->quit || s->paused) return false;
        taskENTER_CRITICAL(&s->lock);
        bool free_slot = s->pending < 0;
        taskEXIT_CRITICAL(&s->lock);
        if (free_slot) return true;
        wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
    }
}

static esp_err_t comp_decode(vp_session_t *s, vp_out_t *o, const uint8_t *jpg, uint32_t len,
                             int *out_b)
{
    bool has_alpha = s->info.version == OBV3_VERSION;
    const uint8_t *alpha_data = NULL;
    size_t alpha_len = 0;
    if (has_alpha) {
        if (len < 13u) return ESP_ERR_INVALID_SIZE;
        uint32_t jpeg_len = (uint32_t)jpg[0] | ((uint32_t)jpg[1] << 8) |
                            ((uint32_t)jpg[2] << 16) | ((uint32_t)jpg[3] << 24);
        uint32_t packed_alpha_len = (uint32_t)jpg[4] | ((uint32_t)jpg[5] << 8) |
                                    ((uint32_t)jpg[6] << 16) | ((uint32_t)jpg[7] << 24);
        if (jpeg_len < 4 || packed_alpha_len == 0 || jpeg_len > len - 8u ||
            packed_alpha_len != len - 8u - jpeg_len) return ESP_ERR_INVALID_SIZE;
        alpha_data = jpg + 8u + jpeg_len;
        alpha_len = packed_alpha_len;
        jpg += 8;
        len = jpeg_len;
    }
    media_jpeg_info_t ji;
    esp_err_t err = media_jpeg_begin(o->j, jpg, len, &ji);
    if (err != ESP_OK) return err;
    if (ji.width != s->info.width || ji.height != s->info.height) return ESP_ERR_INVALID_SIZE;
    if (!s->fb[0]) {
        if (ji.block_len < (uint32_t)ji.width * ji.height * 2u) return ESP_ERR_INVALID_SIZE;
        uint64_t required = (uint64_t)ji.width * ji.height * (has_alpha ? 3u : 2u);
        if (required > UINT32_MAX - 15u) return ESP_ERR_INVALID_SIZE;
        uint32_t cap = ((uint32_t)required + 15u) & ~15u;
        uint8_t *a = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *b = heap_caps_aligned_alloc(16, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *alpha = has_alpha ? heap_caps_malloc((size_t)ji.width * ji.height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!a || !b || (has_alpha && !alpha)) {
            heap_caps_free(a);
            heap_caps_free(b);
            heap_caps_free(alpha);
            return ESP_ERR_NO_MEM;
        }
        s->fb_cap = cap;
        s->fb[1] = b;
        s->fb[0] = a;
        s->alpha_tmp = alpha;
    }
    if (ji.block_len > s->fb_cap) return ESP_ERR_INVALID_SIZE;
    taskENTER_CRITICAL(&s->lock);
    int back = s->front == 0 ? 1 : 0;
    taskEXIT_CRITICAL(&s->lock);
    err = media_jpeg_decode_frame(o->j, s->fb[back], s->fb_cap);
    if (err == ESP_OK && has_alpha) {
        size_t pixels = (size_t)ji.width * ji.height;
        /* Opaque videos also use OBV3. Keep those frames in RGB565 so LVGL
         * can cover the background and avoid per-pixel alpha compositing. */
        has_alpha = !obv3_alpha_is_opaque(alpha_data, alpha_len, pixels);
    }
    if (err == ESP_OK && has_alpha) {
        size_t pixels = (size_t)ji.width * ji.height;
        if (!obv3_unpack_alpha(alpha_data, alpha_len, s->alpha_tmp, pixels)) return ESP_ERR_INVALID_SIZE;
        /* Expand RGB565 to LVGL's packed RGB565+alpha format in place. */
        for (size_t i = pixels; i > 0; --i) {
            size_t pixel = i - 1u;
            /* At pixel 1 the first destination byte overlaps the second
             * source byte, so read both before writing either. */
            uint8_t lo = s->fb[back][pixel * 2u];
            uint8_t hi = s->fb[back][pixel * 2u + 1u];
            s->fb[back][pixel * 3u] = lo;
            s->fb[back][pixel * 3u + 1u] = hi;
            s->fb[back][pixel * 3u + 2u] = s->alpha_tmp[pixel];
        }
    }
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s->lock);
        s->pending_alpha = has_alpha;
        taskEXIT_CRITICAL(&s->lock);
    }
    *out_b = back;
    return err;
}

static void comp_publish(vp_session_t *s, int b)
{
    taskENTER_CRITICAL(&s->lock);
    s->pending = (int8_t)b;
    taskEXIT_CRITICAL(&s->lock);
    lvgl_runtime_wake();
}

/* ------------------------------------------------------------- decoder */

/* Sleep until wall time t; false when pause / quit interrupts. */
static bool wait_until(vp_session_t *s, int64_t t)
{
    for (;;) {
        if (s->quit || s->paused) return false;
        int64_t d = t - esp_timer_get_time();
        if (d < 1000) return true;
        uint32_t ms = (uint32_t)(d / 1000);
        wait_ev(s, EV_WAKE_DECODER, pdMS_TO_TICKS(ms ? ms : 1));
    }
}

static bool watermark_reached(vp_session_t *s, uint32_t frames)
{
    bool fin = s->eof || s->reader_err;
    taskENTER_CRITICAL(&s->lock);
    bool ok = s->frames_buffered >= frames || s->ring_full;
    taskEXIT_CRITICAL(&s->lock);
    return ok || fin;
}

/* Diagnostics describe decoder publication, not measured panel refresh FPS. */
static void pace_sample(vp_session_t *s, vp_pace_t *p, uint32_t proc_us,
                        uint32_t ui_wait_us, int64_t now)
{
    (void)s; /* Used only when periodic diagnostics are enabled. */
    p->ema = p->samples ? (uint32_t)(((uint64_t)p->ema * 7u + proc_us) / 8u) : proc_us;
    p->ui_ema = p->samples ? (uint32_t)(((uint64_t)p->ui_ema * 7u + ui_wait_us) / 8u) : ui_wait_us;
    p->samples++;
    p->fps_n++;
    int64_t span = now - p->fps_t;
    if (span >= VP_STATS_US) {
#if VP_DIAG_LOG
        vp_io_stats_t io;
        uint64_t ui_gap_us, ui_gap_max_us;
        uint32_t ui_intervals;
        taskENTER_CRITICAL(&s->lock);
        io = s->io;
        memset(&s->io, 0, sizeof(s->io));
        ui_gap_us = s->ui_gap_us;
        ui_gap_max_us = s->ui_gap_max_us;
        ui_intervals = s->ui_intervals;
        s->ui_gap_us = s->ui_gap_max_us = 0;
        s->ui_intervals = 0;
        taskEXIT_CRITICAL(&s->lock);
        uint32_t fps10 = (uint32_t)(((int64_t)p->fps_n * 10000000 + span / 2) / span);
        ESP_LOGI(TAG, "queued %lu.%lu fps, decode %lu us, UI wait %lu us, dropped %lu, underruns %lu",
                 (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u),
                 (unsigned long)p->ema, (unsigned long)p->ui_ema,
                 (unsigned long)s->frames_dropped, (unsigned long)s->underruns);
        ESP_LOGI(TAG, "SD %lu KiB/s, frame read avg/max %lu/%lu us (%lu frames, %lu KiB)",
                 (unsigned long)(io.total_us ? io.bytes * 1000000u / (io.total_us * 1024u) : 0),
                 (unsigned long)(io.frames ? io.total_us / io.frames : 0),
                 (unsigned long)io.max_us, (unsigned long)io.frames,
                 (unsigned long)(io.bytes / 1024u));
        if (s->mode == VIDEO_MODE_COMPOSITE)
            ESP_LOGI(TAG, "UI frame gap avg/max %lu/%lu us (%lu intervals)",
                     (unsigned long)(ui_intervals ? ui_gap_us / ui_intervals : 0),
                     (unsigned long)ui_gap_max_us, (unsigned long)ui_intervals);
#endif
        p->fps_n = 0;
        p->fps_t = now;
    }
}

/* Random access has no watermark or clock. Drop obsolete queued requests before
 * decoding, but present completed work even during a continuously moving sweep.
 * This bounds latency without starving presentation when inputs exceed decode FPS. */
static void dec_position(vp_session_t *s, vp_out_t *o)
{
    while (!s->quit) {
        if (s->paused) { wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY); continue; }
        uint32_t size, rec;
        const uint8_t *jpg;
        if (!ring_peek(s, &size, &jpg, &rec)) {
            if (s->reader_err) { s->state = VIDEO_STATE_ERROR; return; }
            wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
            continue;
        }
        uint32_t frame;
        memcpy(&frame, jpg - VP_REC_HDR, sizeof(frame));
        if (frame != requested_frame(s)) { ring_release(s, rec); continue; }
        if (!comp_wait_slot(s)) continue;
        /* UI handoff may have waited while another value arrived. */
        if (frame != requested_frame(s)) { ring_release(s, rec); continue; }
        int b = -1;
        esp_err_t err = comp_decode(s, o, jpg, size, &b);
        ring_release(s, rec);
        if (err != ESP_OK) {
            /* There is no next clock tick to retry an unchanged numeric target.
             * End the session so the element can fall back to its poster. */
            s->state = VIDEO_STATE_ERROR;
            return;
        }
        if (s->quit) break;
        comp_publish(s, b);
        s->started = true;
        if (!s->paused) s->state = VIDEO_STATE_PLAYING;
    }
}

static void dec_run(vp_session_t *s, vp_out_t *o)
{
    const uint32_t fps = s->info.fps;
    const int64_t frame_us = 1000000 / fps;
    const uint32_t wm = (fps * VP_WATERMARK_MS + 999u) / 1000u;
    const uint32_t late_limit = fps / 2u ? fps / 2u : 1u;
    const uint32_t cap = s->target_fps && s->target_fps < fps ? s->target_fps : fps;
    vp_pace_t p = {.buffering = true};
    uint32_t err_run = 0;

    while (!s->quit) {
        if (s->paused) {
            wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
            p.rebase = true;
            continue;
        }
        if (p.buffering) {
            /* Keep the startup cushion, but resume a starved theme as soon as
             * one frame arrives. Refilling 400 ms after every underrun causes
             * repeated play/freeze bursts when SD throughput is below source FPS. */
            uint32_t resume_wm = p.shown && s->mode == VIDEO_MODE_COMPOSITE ? 1u : wm;
            if (!watermark_reached(s, resume_wm)) {
                wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
                continue;
            }
            p.buffering = false;
            p.rebase = true;
        }

        bool fin = s->eof || s->reader_err;
        uint32_t size, rec;
        const uint8_t *jpg;
        if (!ring_peek(s, &size, &jpg, &rec)) {
            if (fin) {
                s->state = s->reader_err ? VIDEO_STATE_ERROR : VIDEO_STATE_STOPPED;
                return;
            }
            if (p.shown) {
                s->underruns++;
            }
            p.buffering = true;
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t due_rel = (int64_t)(p.pos * 1000000ull / fps);
        if (p.rebase) {
            p.t0 = now - due_rel;
            p.rebase = false;
        }
        int64_t due = p.t0 + due_rel;

        /* Target buckets distribute an explicit FPS cap over the source
         * timeline, without rounding 30 -> 24 down to every second frame. */
        uint64_t slot = p.pos * cap / fps;
        bool show = !p.shown || slot != p.last_slot;
        if (!show) {
            s->frames_dropped++;
            ring_release(s, rec);
            p.pos++;
            continue;
        }
        uint32_t ui_wait_us = 0;
        if (s->mode == VIDEO_MODE_COMPOSITE) {
            int64_t t = esp_timer_get_time();
            if (!comp_wait_slot(s)) continue;
            now = esp_timer_get_time();
            ui_wait_us = (uint32_t)(now - t);
        }
        /* UI rendering may have held the slot long enough to miss this frame.
         * Check after that wait so stale records do not consume decode time. */
        if (p.shown && now > due + frame_us) {
            if (++p.late_run < late_limit) {
                s->frames_dropped++;
                ring_release(s, rec);
                p.pos++;
                continue;
            }
            p.t0 = now - due_rel;   /* hopelessly behind: show this one and rebase */
            due = now;
        }

        esp_err_t err;
        uint32_t proc_us;
        if (s->mode == VIDEO_MODE_FULLSCREEN) {
            int64_t t = esp_timer_get_time();
            err = fs_present(s, o, jpg, size, due);
            if (err == ESP_ERR_INVALID_STATE && (s->paused || s->quit)) continue;
            proc_us = (uint32_t)(esp_timer_get_time() - t) - o->pace_wait_us;
            PERF_RECORD(PERF_VIDEO_FULLSCREEN, proc_us, 1);
        } else {
            int64_t t = esp_timer_get_time();
            int b = 0;
            err = comp_decode(s, o, jpg, size, &b);
            proc_us = (uint32_t)(esp_timer_get_time() - t);
            PERF_RECORD(PERF_VIDEO_COMPOSITE, proc_us, 1);
            if (err == ESP_OK) {
                if (!wait_until(s, due)) continue;
                comp_publish(s, b);
            }
        }
        ring_release(s, rec);
        p.pos++;

        if (err != ESP_OK) {
            s->frames_dropped++;
            if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NO_MEM || ++err_run > VP_MAX_DEC_ERRORS) {
                ESP_LOGE(TAG, "giving up: %s", esp_err_to_name(err));
                s->state = VIDEO_STATE_ERROR;
                return;
            }
            ESP_LOGW(TAG, "frame %llu: %s", (unsigned long long)(p.pos - 1u), esp_err_to_name(err));
            continue;
        }
        err_run = 0;
        p.late_run = 0;
        now = esp_timer_get_time();
        if (!p.shown) {
            p.shown = true;
            p.fps_t = now;
            s->started = true;
            if (!s->paused) s->state = VIDEO_STATE_PLAYING;
            ESP_LOGI(TAG, "first frame %lld ms after start", (long long)((now - s->t_start) / 1000));
        }
        p.last_slot = slot;
        pace_sample(s, &p, proc_us, ui_wait_us, now);
    }
}

/* Timed boot frames keep their source cadence. The legacy/theme policy above
 * is deliberately separate: no target FPS cap or clock rebasing.
 * Pauses shift the timeline; expired frames are released without decoding so
 * SD/decode stalls do not accumulate delay or abort an otherwise valid file. */
static bool timed_wait(vp_session_t *s, int64_t *origin, uint32_t rel)
{
    while (!s->quit) {
        if (s->paused) {
            int64_t pause = esp_timer_get_time();
            do { wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY); } while (s->paused && !s->quit);
            *origin += esp_timer_get_time() - pause;
        }
        if (wait_until(s, *origin + rel)) return true;
    }
    return false;
}
static void dec_timed(vp_session_t *s, vp_out_t *o)
{
    uint32_t prebuffer = obv_timed_prebuffer(&s->info, s->timed_index);
    for (;;) {
        if (s->quit) return;
        taskENTER_CRITICAL(&s->lock);
        bool ready = s->frames_buffered >= prebuffer;
        taskEXIT_CRITICAL(&s->lock);
        if (s->reader_err) { s->state = VIDEO_STATE_ERROR; return; }
        if (ready) break;
        if (s->eof) { s->state = VIDEO_STATE_ERROR; return; }
        wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
    }
    int64_t origin = esp_timer_get_time();
    ESP_LOGI(TAG, "OBV2 prebuffer ready: %lu frames, %lld ms after start",
             (unsigned long)prebuffer, (long long)((origin - s->t_start) / 1000));
    for (uint32_t n = 0; n < s->info.frame_count && !s->quit; n++) {
        uint32_t size, rec;
        const uint8_t *jpg;
        while (!ring_peek(s, &size, &jpg, &rec)) {
            if (s->quit) return;
            if (s->reader_err || s->eof) { s->state = VIDEO_STATE_ERROR; return; }
            s->underruns++;
            wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
        }
        uint32_t time = obv_frame_time(s->timed_index, n);
        uint32_t duration = obv_frame_duration(s->timed_index, n);
        if (s->paused && !timed_wait(s, &origin, time)) return;
        /* Keep the first/last image. Skip only frames whose entire display
         * interval has already elapsed; never rewrite a committed ring record. */
        if (n && n + 1 < s->info.frame_count &&
            esp_timer_get_time() >= origin + time + duration) {
            ring_release(s, rec);
            s->frames_dropped++;
            continue;
        }
        int64_t begin = esp_timer_get_time();
        esp_err_t err;
        for (;;) {
            begin = esp_timer_get_time();
            err = fs_present(s, o, jpg, size, origin + time);
            if (err != ESP_ERR_INVALID_STATE || (!s->paused && !s->quit)) break;
            /* Keep the committed record and shift the timeline on pause.
             * Header/decode can restart; a partial frame was never published. */
            if (!timed_wait(s, &origin, time)) return;
        }
        int64_t presented_at = esp_timer_get_time();
        ring_release(s, rec);
        PERF_RECORD(PERF_VIDEO_FULLSCREEN, presented_at - begin - o->pace_wait_us, 1);
        (void)begin;
        if (err != ESP_OK) { s->state = VIDEO_STATE_ERROR; return; }
        if (!n) {
            /* One-time JPEG/strip setup is outside the source timeline. */
            origin = esp_timer_get_time();
            s->started = true;
            s->state = s->paused ? VIDEO_STATE_PAUSED : VIDEO_STATE_PLAYING;
            ESP_LOGI(TAG, "OBV2 first frame on LCD: %lld ms after start, decode/present %lld us",
                     (long long)((presented_at - s->t_start) / 1000),
                     (long long)(presented_at - begin));
        }
    }
    uint32_t last = s->info.frame_count - 1;
    uint32_t end = obv_frame_time(s->timed_index, last) + obv_frame_duration(s->timed_index, last);
    if (timed_wait(s, &origin, end)) {
        s->state = VIDEO_STATE_STOPPED;
        ESP_LOGI(TAG, "OBV2 playback completed: %lu frames, %lu skipped, %lld ms on timeline",
                 (unsigned long)s->info.frame_count, (unsigned long)s->frames_dropped,
                 (long long)((esp_timer_get_time() - origin) / 1000));
    }
}

static void decoder_task(void *arg)
{
    vp_session_t *s = arg;
    vp_out_t o = {0};

    while (!s->quit && !s->hdr_ready && !s->reader_err) wait_ev(s, EV_WAKE_DECODER, portMAX_DELAY);
    if (!s->quit && s->hdr_ready) {
        const obv1_info_t *in = &s->info;
        bool block = s->mode == VIDEO_MODE_FULLSCREEN && (in->flags & OBV1_FLAG_BLOCK_SAFE) &&
                     media_jpeg_block_ok(in->subsampling);
        if (media_jpeg_open(block, &o.j) == ESP_OK) {
            o.block = block;
            ESP_LOGI(TAG, "%s: %ux%u %u fps, %lu frames, sub %u, max frame %lu B, ring %lu KB",
                     s->path, in->width, in->height, in->fps, (unsigned long)in->frame_count,
                     in->subsampling, (unsigned long)in->max_frame_size,
                     (unsigned long)(s->ring_cap / 1024u));
            if (s->mode == VIDEO_MODE_FULLSCREEN && !fs_wait_idle(s, VP_FLUSH_WAIT_MS)) {
                ESP_LOGW(TAG, "LVGL flush still pending");
            }
            if (in->version == OBV2_VERSION) dec_timed(s, &o);
            else if (s->value_driven) dec_position(s, &o);
            else dec_run(s, &o);
        } else {
            s->state = VIDEO_STATE_ERROR;
        }
    } else if (!s->quit) {
        s->state = VIDEO_STATE_ERROR;
    }

    /* Error/end also releases a producer waiting for ring space. */
    if (s->info.version == OBV2_VERSION || s->value_driven) {
        s->quit = true;
        xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
    }
    media_jpeg_close(o.j);
    if (s->mode == VIDEO_MODE_FULLSCREEN) {
        if (!fs_wait_idle(s, 100)) {
            ESP_LOGE(TAG, "last panel transfer did not finish; retaining DMA buffers until idle");
            while (!fs_wait_idle(s, 100)) {
                /* The LCD DMA may still be reading a strip; freeing it is unsafe. */
            }
        }
        if (s->strip_owned) {
            heap_caps_free(s->strip[0]);
            heap_caps_free(s->strip[1]);
            s->strip_owned = false;
        }
        s->strip[0] = s->strip[1] = NULL;
        heap_caps_free(s->fb[0]);
        s->fb[0] = NULL;
    }
    ESP_LOGD(TAG, "decoder exit, stack headroom %u B", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xEventGroupSetBits(s->ev, EV_DECODER_DONE);
    session_unref(s);
    vTaskDelete(NULL);
}

/* ----------------------------------------------------------------- API */

/* LVGL invalidation and layout updates resume its display timer automatically.
 * Pausing alone cannot protect the borrowed DMA buffers: keep a harmless
 * callback installed until the decoder has released both buffers and panel.
 * This callback runs only in the LVGL task and never references the session. */
static void fullscreen_refr_guard(lv_timer_t *timer)
{
    lv_timer_pause(timer);
}

/* Stop the tasks and give the panel / view back (LVGL task). */
static void session_halt(vp_session_t *s)
{
    s->quit = true;
    xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
    if (s->mode == VIDEO_MODE_COMPOSITE && s->view) video_view_clear(s->view);
    EventBits_t bits = xEventGroupWaitBits(s->ev, EV_DECODER_DONE, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(VP_STOP_WAIT_MS));
    if (!(bits & EV_DECODER_DONE)) {
        ESP_LOGW(TAG, "decoder is still releasing the JPEG engine; waiting before reuse");
        xEventGroupWaitBits(s->ev, EV_DECODER_DONE, pdFALSE, pdFALSE, portMAX_DELAY);
    }
    if (s->refr_paused) {
        lv_timer_set_cb(s->refr_timer, s->refr_cb);
        lv_timer_resume(s->refr_timer);
        s->refr_paused = false;
        /* the panel shows video, not LVGL's last frame */
        lv_obj_t *scr = lv_disp_get_scr_act(s->disp);
        if (scr) lv_obj_invalidate(scr);
    }
}

static bool spawn(vp_session_t *s, TaskFunction_t fn, const char *name, uint32_t stack,
                  UBaseType_t prio, BaseType_t core)
{
    session_ref(s);
    if (xTaskCreatePinnedToCore(fn, name, stack, s, prio, NULL, core) == pdPASS) return true;
    session_unref(s);
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGE(TAG, "cannot create %s (stack=%lu, internal free=%lu largest=%lu)", name,
             (unsigned long)stack, (unsigned long)heap_caps_get_free_size(caps),
             (unsigned long)heap_caps_get_largest_free_block(caps));
    return false;
}

esp_err_t video_player_start(const video_config_t *config)
{
    if (!config || !config->path || !config->path[0]) return ESP_ERR_INVALID_ARG;
    if (config->source != VIDEO_SRC_SD) return ESP_ERR_NOT_SUPPORTED;   /* flash = legacy, boot_video */
    if (config->mode == VIDEO_MODE_COMPOSITE && !config->video_view) return ESP_ERR_INVALID_ARG;
    if (config->value_driven && (config->mode != VIDEO_MODE_COMPOSITE || !isfinite(config->position)))
        return ESP_ERR_INVALID_ARG;
    video_player_stop();

    vp_session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    if (strlen(config->path) >= sizeof(s->path)) {
        heap_caps_free(s);
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(s->path, config->path);
    portMUX_INITIALIZE(&s->lock);
    s->mode = config->mode;
    s->loop = config->loop;
    s->value_driven = config->value_driven;
    s->position = fmax(0.0, fmin(1.0, config->position));
    s->target_fps = config->target_fps;
    s->view = config->mode == VIDEO_MODE_COMPOSITE ? config->video_view : NULL;
    s->front = -1;
    s->pending = -1;
    s->state = VIDEO_STATE_LOADING;
    s->refs = 1;
    s->t_start = esp_timer_get_time();
    s->ev = xEventGroupCreate();
    if (!s->ev) {
        session_free(s);
        return ESP_ERR_NO_MEM;
    }

    if (s->mode == VIDEO_MODE_FULLSCREEN) {
        s->disp = lv_disp_get_default();
        s->draw_buf = s->disp ? lv_disp_get_draw_buf(s->disp) : NULL;
        s->refr_timer = s->disp ? _lv_disp_get_refr_timer(s->disp) : NULL;
        if (!s->draw_buf || !s->refr_timer || !panel_handle) {
            session_free(s);
            return ESP_ERR_INVALID_STATE;
        }
        /* Borrow LVGL's draw buffers as strips when they are internal DMA RAM:
         * the guarded refresh callback prevents LVGL rendering even if a
         * screen or layout change automatically resumes the timer. */
        lv_disp_draw_buf_t *db = s->draw_buf;
        if (db->buf1 && db->buf2 && esp_ptr_internal(db->buf1) && esp_ptr_dma_capable(db->buf1) &&
            esp_ptr_internal(db->buf2) && esp_ptr_dma_capable(db->buf2)) {
            uint32_t bytes = db->size * sizeof(lv_color_t);
            s->strip[0] = align16(db->buf1);
            s->strip[1] = align16(db->buf2);
            uint32_t skew0 = (uint32_t)(s->strip[0] - (uint8_t *)db->buf1);
            uint32_t skew1 = (uint32_t)(s->strip[1] - (uint8_t *)db->buf2);
            s->strip_cap = bytes - (skew0 > skew1 ? skew0 : skew1);
        }
        s->refr_cb = s->refr_timer->timer_cb;
        lv_timer_set_cb(s->refr_timer, fullscreen_refr_guard);
        lv_timer_pause(s->refr_timer);
        s->refr_paused = true;
    }

    /* Keep the decoder responsive, but make SD prefetch cooperative with the
     * BLE host on core 0. Large boot videos can otherwise monopolize that
     * core while the reader fills the ring. */
    const UBaseType_t worker_prio = s->mode == VIDEO_MODE_COMPOSITE
        ? VP_THEME_WORKER_PRIO : VP_BOOT_WORKER_PRIO;
    const UBaseType_t reader_prio = VP_SD_READER_PRIO;
    bool ok = spawn(s, decoder_task, "vid_dec", VP_DECODER_STACK, worker_prio, 1);
    if (!ok) xEventGroupSetBits(s->ev, EV_DECODER_DONE);
    ok = ok && spawn(s, reader_task, "vid_read", VP_READER_STACK, reader_prio, 0);
    if (!ok) {
        session_halt(s);
        session_unref(s);
        return ESP_ERR_NO_MEM;
    }
    if (++s_next_session_id == 0) ++s_next_session_id;
    s->id = s_next_session_id;
    s_cur = s;
    ESP_LOGI(TAG, "start %s (%s)", s->path, s->mode == VIDEO_MODE_FULLSCREEN ? "fullscreen" : "composite");
    return ESP_OK;
}

esp_err_t video_player_stop(void)
{
    vp_session_t *s = s_cur;
    if (!s) return ESP_OK;
    s_cur = NULL;
    session_halt(s);
    session_unref(s);
    return ESP_OK;
}

esp_err_t video_player_pause(void)
{
    vp_session_t *s = s_cur;
    if (!s || (s->state != VIDEO_STATE_PLAYING && s->state != VIDEO_STATE_LOADING)) {
        return ESP_ERR_INVALID_STATE;
    }
    s->paused = true;
    s->state = VIDEO_STATE_PAUSED;
    /* LVGL control context: freeze presentation too. An already-published
     * frame (including a racing publication) stays pending until resume. */
    xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
    return ESP_OK;
}

esp_err_t video_player_resume(void)
{
    vp_session_t *s = s_cur;
    if (!s || s->state != VIDEO_STATE_PAUSED) return ESP_ERR_INVALID_STATE;
    s->state = s->started ? VIDEO_STATE_PLAYING : VIDEO_STATE_LOADING;
    s->paused = false;
    if (s->mode == VIDEO_MODE_COMPOSITE) lvgl_runtime_wake();
    xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
    return ESP_OK;
}

esp_err_t video_player_set_position(double position)
{
    vp_session_t *s = s_cur;
    if (!s || !s->value_driven || !isfinite(position)) return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s->lock);
    double next = fmax(0.0, fmin(1.0, position));
    bool changed = s->position != next && (!s->hdr_ready ||
        floor(s->position * (s->info.frame_count - 1u) + 0.5) !=
        floor(next * (s->info.frame_count - 1u) + 0.5));
    if (changed) s->position_revision++;
    s->position = next;
    taskEXIT_CRITICAL(&s->lock);
    // Sub-frame RPM/throttle jitter cannot change the picture. Avoid waking
    // both workers until the quantized target actually changes (ABA included).
    if (changed) xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
    return ESP_OK;
}

video_state_t video_player_get_state(void)
{
    return s_cur ? (video_state_t)s_cur->state : VIDEO_STATE_STOPPED;
}

lv_obj_t *video_player_current_view(void)
{
    vp_session_t *s = s_cur;
    return s && s->mode == VIDEO_MODE_COMPOSITE ? s->view : NULL;
}

uint32_t video_player_session_id(void)
{
    return s_cur ? s_cur->id : 0;
}

bool video_player_fullscreen_active(void)
{
    vp_session_t *s = s_cur;
    return s && s->mode == VIDEO_MODE_FULLSCREEN;
}

bool video_player_first_frame_presented(void)
{
    return s_cur && s_cur->started;
}

void video_player_release_all(void)
{
    bool take = lvgl_mux && xSemaphoreGetMutexHolder(lvgl_mux) != xTaskGetCurrentTaskHandle();
    if (take) xSemaphoreTake(lvgl_mux, portMAX_DELAY);
    video_player_stop();
    if (take) xSemaphoreGive(lvgl_mux);
}
