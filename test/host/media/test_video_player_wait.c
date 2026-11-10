/* White-box execution of real player control paths, not a copied state machine.
 * Scripted SDK event bits latch across the predicate/wait gap. Single-threaded
 * virtual time does not model ESP32 scheduling, DMA, JPEG or SD performance. */
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include VP_TEST_SOURCE
#include VP_RUNTIME_SOURCE

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
struct player_test_event { EventBits_t bits; };
struct media_jpeg { uint16_t width, height, marker; };
typedef struct { TaskFunction_t fn; void *arg; bool deleted; } fake_task_t;
static fake_task_t tasks[2];
static unsigned task_count, task_delete_count, create_attempts;
static int current_task = -1;
static unsigned fail_task_attempt;
static void *allocations[64];
static unsigned live_allocations;
static size_t max_dma_alloc = SIZE_MAX;
static size_t dma_alloc_sizes[16];
static unsigned dma_alloc_attempts;
static unsigned waits, signal_waits, timed_waits, sets_reader, sets_decoder;
static unsigned decoded, presented, panel_draws, invalidations, timer_deletes, ui_steps;
static uint8_t *block_outputs[4];
static int64_t block_times[4];
static unsigned block_count;
static uint16_t decoded_marker[256], shown_marker[256];
static int64_t decoded_at[256], now_us;
static unsigned fullscreen_frames;
static uint16_t fullscreen_marker[16];
static int64_t fullscreen_at[16];
static bool allow_timed, auto_ui, delay_frame_dma, fail_decode;
static int64_t dma_done_at;
static unsigned dma_wait_checks;
static unsigned actions;
static bool runtime_active, mux_held;
static unsigned runtime_handlers, runtime_waits, runtime_limit, wake_calls, event_returns, timeout_returns;
static TickType_t runtime_ticks[16];
static uint32_t notify_bits, handler_delay = 500;
static jmp_buf runtime_escape;
static void (*at_handler)(void), (*before_notify_wait)(void), (*notify_block)(void);
#if CONFIG_OBD_PERF_STATS
static unsigned perf_counts[PERF_METRIC_COUNT];
void perf_stats_record(perf_metric_t metric, int64_t duration_us, uint32_t units)
{
    CHECK(metric < PERF_METRIC_COUNT && duration_us >= 0 && units == 1);
    if (metric == PERF_LVGL_WAIT || metric == PERF_LVGL_HANDLER) CHECK(runtime_active && mux_held);
    perf_counts[metric]++;
}
#endif

static void (*before_wait)(vp_session_t *, EventBits_t);
static void (*on_block)(vp_session_t *, EventBits_t);
static vp_session_t *test_session;
static void (*during_decode)(void);
static void (*after_publication)(vp_session_t *);
static void service_ui(vp_session_t *s);
static void run_task(unsigned i);
static void finish(void);
static lv_obj_t view, screen;
static _Alignas(16) uint8_t draw1[360 * 40 * 2], draw2[360 * 40 * 2];
static unsigned refresh_draws;
static void draw_refresh(lv_timer_t *timer)
{
    CHECK(timer);
    refresh_draws++;
    memset(draw1, 0, sizeof(draw1));
}
static lv_timer_t refresh = { .timer_cb = draw_refresh };
static lv_disp_draw_buf_t draw_buf = { draw1, draw2, 360 * 40, 0 };
static lv_disp_t display = { &draw_buf, &refresh, &screen };
SemaphoreHandle_t lvgl_mux;
void *panel_handle = &display;
static char file_path[96];

static void remember(void *p)
{
    CHECK(p);
    for (unsigned i = 0; i < COUNT_OF(allocations); i++) {
        if (!allocations[i]) { allocations[i] = p; live_allocations++; return; }
    }
    CHECK(false);
}
static size_t free_psram = 8u * 1024u * 1024u;
size_t heap_caps_get_free_size(unsigned caps)
{
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return 2048;
    CHECK(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return free_psram;
}
size_t heap_caps_get_largest_free_block(unsigned caps)
{
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return 1024;
    CHECK(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return max_dma_alloc;
}
void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    void *p = malloc(n); remember(p); return p;
}
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    void *p = calloc(n, size); remember(p); return p;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned caps)
{
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) {
        CHECK(dma_alloc_attempts < COUNT_OF(dma_alloc_sizes));
        dma_alloc_sizes[dma_alloc_attempts++] = n;
        if (n > max_dma_alloc) return NULL;
    }
    void *p = NULL;
    CHECK(posix_memalign(&p, alignment, n) == 0); remember(p); return p;
}
void heap_caps_free(void *p)
{
    if (!p) return;
    for (unsigned i = 0; i < COUNT_OF(allocations); i++) {
        if (allocations[i] == p) { allocations[i] = NULL; live_allocations--; free(p); return; }
    }
    CHECK(false); /* catches duplicate frees and freeing borrowed LVGL strips */
}
bool esp_ptr_internal(const void *p) { return p != NULL; }
bool esp_ptr_dma_capable(const void *p) { return p != NULL; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "stub"; }
void player_test_log(const char *tag, const char *fmt, ...)
{
    CHECK(!strcmp(tag, "video_player"));
    char msg[256]; va_list ap; va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
}
int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_delay_us(uint32_t us)
{
    now_us += us;
    if (dma_done_at) {
        CHECK(draw_buf.flushing && !video_player_first_frame_presented());
        dma_wait_checks++;
        if (now_us >= dma_done_at) { dma_done_at = 0; draw_buf.flushing = 0; }
    }
}
void player_test_enter(portMUX_TYPE *lock) { CHECK(!lock->depth); lock->depth++; }
void player_test_exit(portMUX_TYPE *lock)
{
    CHECK(lock->depth == 1); lock->depth--;
    if (after_publication && test_session && lock == &test_session->lock && test_session->pending >= 0) {
        void (*cb)(vp_session_t *) = after_publication;
        after_publication = NULL;
        cb(test_session); /* scripted LVGL control after the producer releases its lock */
    }
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                 void *arg, UBaseType_t prio, TaskHandle_t *handle, BaseType_t core)
{
    CHECK(!handle && fn && arg);
    vp_session_t *s = arg;
    UBaseType_t expected_prio = s->mode == VIDEO_MODE_COMPOSITE ? 3 : 5;
    CHECK((!strcmp(name, "vid_dec") && stack == 6144 && prio == expected_prio && core == 1) ||
          (!strcmp(name, "vid_read") && stack == 4096 && prio == expected_prio && core == 0));
    if (++create_attempts == fail_task_attempt) return pdFAIL;
    CHECK(task_count < COUNT_OF(tasks));
    tasks[task_count++] = (fake_task_t){ fn, arg, false };
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task)
{
    CHECK(!task && current_task >= 0 && current_task < (int)task_count);
    CHECK(!tasks[current_task].deleted);
    tasks[current_task].deleted = true; task_delete_count++;
    /* Production calls this only as its last statement, so returning is safe. */
}
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { CHECK(!task); return 2048; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &current_task; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    CHECK(sem && ticks == portMAX_DELAY);
    if (runtime_active) { CHECK(!mux_held); mux_held = true; }
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{ CHECK(sem); if (runtime_active) { CHECK(mux_held); mux_held = false; } return pdTRUE; }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action)
{
    CHECK(task == &current_task && value == 1 && action == eSetBits);
    CHECK(!test_session || !test_session->lock.depth);
    wake_calls++; notify_bits |= value; return pdPASS;
}
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit_bits, uint32_t *value, TickType_t ticks)
{
    CHECK(runtime_active && !mux_held && !entry && exit_bits == UINT32_MAX && !value);
    CHECK(runtime_waits < COUNT_OF(runtime_ticks)); runtime_ticks[runtime_waits++] = ticks;
    if (before_notify_wait) before_notify_wait();
    if (!notify_bits && notify_block) notify_block();
    bool event = notify_bits != 0;
    if (event) { notify_bits &= ~exit_bits; event_returns++; }
    else { now_us += (int64_t)ticks * 1000; timeout_returns++; }
    if (runtime_waits == runtime_limit) longjmp(runtime_escape, 1);
    return event ? pdTRUE : pdFALSE;
}
static nvs_user_cfg_t runtime_cfg;
static bool runtime_elm_connected, check_tpms_schedule;
static unsigned tpms_service_calls;
nvs_user_cfg_t nvs_cfg_read(void) { return runtime_cfg; }
bool elm327_ble_is_connected(void) { return runtime_elm_connected; }
uint32_t lv_tick_get(void) { return (uint32_t)(now_us / 1000); }
uint32_t lv_tick_elaps(uint32_t previous) { return lv_tick_get() - previous; }
void tpms_ble_service_step(bool allow_background)
{
    CHECK(mux_held);
    if (check_tpms_schedule) {
        const bool expected[] = {true, false, true, true, false};
        CHECK(tpms_service_calls < COUNT_OF(expected));
        CHECK(allow_background == expected[tpms_service_calls]);
    }
    tpms_service_calls++;
}
uint32_t lv_timer_handler(void)
{
    CHECK(runtime_active && mux_held); runtime_handlers++;
    if (at_handler) at_handler();
    return handler_delay;
}
void *xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) { CHECK(sem); return &current_task; }

EventGroupHandle_t xEventGroupCreate(void)
{ return heap_caps_calloc(1, sizeof(struct player_test_event), 0); }
void vEventGroupDelete(EventGroupHandle_t ev) { heap_caps_free(ev); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t ev, EventBits_t bits)
{
    CHECK(ev);
    sets_reader += !!(bits & EV_WAKE_READER); sets_decoder += !!(bits & EV_WAKE_DECODER);
    return ev->bits |= bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t ev, EventBits_t bits, BaseType_t clear,
                              BaseType_t all, TickType_t ticks)
{
    CHECK(ev && !all && ++waits < 2048); /* bounded, including multi-second playback */
    if (bits == EV_DECODER_DONE) {
        CHECK(!clear && (ticks == pdMS_TO_TICKS(1000) || ticks == portMAX_DELAY));
    } else {
        CHECK(clear && (bits == EV_WAKE_READER || bits == EV_WAKE_DECODER));
        CHECK(allow_timed || ticks == portMAX_DELAY);
        if (ticks == portMAX_DELAY) signal_waits++; else timed_waits++;
    }
    if (before_wait) {
        void (*cb)(vp_session_t *, EventBits_t) = before_wait;
        before_wait = NULL;
        cb(test_session, bits); /* event published after predicate, before wait registration */
    }
    if (!(ev->bits & bits)) {
        if (ticks != portMAX_DELAY && bits != EV_DECODER_DONE) {
            now_us += (int64_t)ticks * 1000;
            return ev->bits;
        }
        if (on_block) on_block(test_session, bits);
        else if (bits == EV_DECODER_DONE && task_count > 0 && !tasks[0].deleted) {
            /* FreeRTOS schedules the decoder while LVGL waits for session
             * shutdown. The host harness runs each worker explicitly. */
            run_task(0);
        }
        else if (auto_ui && bits == EV_WAKE_DECODER && test_session->pending >= 0) {
            CHECK(!test_session->paused);
            service_ui(test_session);
        } else CHECK(false); /* an indefinite waiter has no producer: regression */
        CHECK(ev->bits & bits);
    }
    EventBits_t result = ev->bits;
    if (clear) ev->bits &= ~bits;
    return result;
}

lv_disp_t *lv_disp_get_default(void) { return &display; }
lv_disp_draw_buf_t *lv_disp_get_draw_buf(lv_disp_t *disp) { return disp->draw_buf; }
lv_timer_t *_lv_disp_get_refr_timer(lv_disp_t *disp) { return disp->timer; }
lv_obj_t *lv_disp_get_scr_act(lv_disp_t *disp) { return disp->screen; }
void lv_obj_invalidate(lv_obj_t *obj)
{
    CHECK(obj == &screen); invalidations++;
    lv_timer_resume(&refresh); /* LVGL's real invalidation path does this too. */
}
lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t period, void *data)
{ (void)cb; (void)period; (void)data; CHECK(false); return NULL; }
void lv_timer_del(lv_timer_t *timer) { timer_deletes++; heap_caps_free(timer); }
void lv_timer_set_cb(lv_timer_t *timer, lv_timer_cb_t cb) { CHECK(timer); timer->timer_cb = cb; }
void lv_timer_pause(lv_timer_t *timer) { CHECK(timer); timer->paused = true; }
void lv_timer_resume(lv_timer_t *timer) { CHECK(timer); timer->paused = false; }
void lv_timer_ready(lv_timer_t *timer) { CHECK(timer == &refresh); timer->ready = true; }
void video_view_clear(lv_obj_t *obj) { CHECK(obj == &view); }
void video_view_set_frame(lv_obj_t *obj, const uint16_t *data, uint16_t w, uint16_t h)
{
    CHECK(obj == &view && data && w == 16 && h == 16 && presented < COUNT_OF(shown_marker));
    CHECK(!runtime_active || mux_held);
    shown_marker[presented++] = data[0];
}
void video_view_set_frame_alpha(lv_obj_t *obj, const uint8_t *data, uint16_t w, uint16_t h)
{
    CHECK(obj == &view && data && w == 16 && h == 16 && presented < COUNT_OF(shown_marker));
    CHECK(!runtime_active || mux_held);
    shown_marker[presented++] = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}
esp_err_t esp_lcd_panel_draw_bitmap(void *panel, int x0, int y0, int x1, int y1, const void *pixels)
{
    CHECK(panel == panel_handle && pixels && draw_buf.flushing);
    CHECK(x0 >= 0 && y0 >= 0 && x1 <= 360 && y1 <= 360 && x1 > x0 && y1 > y0);
    /* Fake 16x16 frames fit a single strip; ignore the initial black border. */
    if (x1 - x0 == 16 && y1 - y0 == 16) {
        CHECK(fullscreen_frames < COUNT_OF(fullscreen_at));
        fullscreen_marker[fullscreen_frames] = ((const uint16_t *)pixels)[0];
        fullscreen_at[fullscreen_frames++] = now_us;
        if (delay_frame_dma) {
            CHECK(!video_player_first_frame_presented());
            dma_done_at = now_us + 200;
        }
    }
    panel_draws++;
    if (!dma_done_at) draw_buf.flushing = 0; /* fake ISR completion */
    return ESP_OK;
}
size_t sd_card_read(FILE *f, void *dst, size_t len) { return fread(dst, 1, len, f); }
esp_err_t media_jpeg_open(bool block, media_jpeg_t **out)
{
    CHECK(!block); /* tested fake streams use whole-frame decode */
    *out = heap_caps_calloc(1, sizeof(**out), 0); return ESP_OK;
}
void media_jpeg_close(media_jpeg_t *j) { heap_caps_free(j); }
bool media_jpeg_block_ok(uint8_t sub) { (void)sub; return false; }
esp_err_t media_jpeg_begin(media_jpeg_t *j, const uint8_t *data, size_t len, media_jpeg_info_t *info)
{
    CHECK(j && data && len == 4);
    j->marker = data[0]; j->width = 16; j->height = 16;
    *info = (media_jpeg_info_t){ 16, 16, 0, 512, 1 }; return ESP_OK;
}
esp_err_t media_jpeg_decode_frame(media_jpeg_t *j, uint8_t *out, size_t cap)
{
    CHECK(j && out && cap >= 512 && !((uintptr_t)out & 15u));
    CHECK(decoded < COUNT_OF(decoded_marker));
    decoded_marker[decoded] = j->marker; decoded_at[decoded++] = now_us;
    if (during_decode) { void (*cb)(void) = during_decode; during_decode = NULL; cb(); }
    if (fail_decode) return ESP_FAIL;
    for (unsigned i = 0; i < 512 / sizeof(uint16_t); i++) ((uint16_t *)out)[i] = j->marker;
    return ESP_OK;
}
esp_err_t media_jpeg_next_block(media_jpeg_t *j, uint8_t *out, uint16_t *lines)
{
    CHECK(j && out && lines && block_count < COUNT_OF(block_outputs));
    block_outputs[block_count] = out; block_times[block_count++] = now_us;
    memset(out, j->marker, 16 * 8 * 2); *lines = 8;
    now_us += 4000;
    return ESP_OK;
}

static void run_task(unsigned i)
{
    CHECK(i < task_count && !tasks[i].deleted);
    int saved = current_task; current_task = (int)i;
    tasks[i].fn(tasks[i].arg);
    CHECK(tasks[i].deleted); current_task = saved;
}
static vp_session_t *start(video_mode_t mode, const char *path)
{
    video_config_t cfg = { .mode = mode, .source = VIDEO_SRC_SD, .path = path,
                          .video_view = mode == VIDEO_MODE_COMPOSITE ? &view : NULL };
    CHECK(video_player_start(&cfg) == ESP_OK);
    CHECK(task_count == 2 && s_cur && s_cur->refs == 3 && video_player_session_id() != 0);
    test_session = s_cur; return s_cur;
}
static vp_session_t *prepared(video_mode_t mode, uint32_t cap)
{
    vp_session_t *s = start(mode, "/__obd_host_no_file__/video.obv");
    s->info = (obv1_info_t){ .width = 16, .height = 16, .fps = 24, .frame_count = 3 };
    s->ring = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM); s->ring_cap = cap;
    s->hdr_ready = true; return s;
}
static void frame(vp_session_t *s, uint8_t marker)
{
    uint32_t at, waste;
    CHECK(ring_reserve(s, 12, &at, &waste));
    uint32_t *h = (uint32_t *)(s->ring + at); h[0] = marker; h[1] = 4;
    memset(s->ring + at + 8, marker, 4); ring_commit(s, at, 12, waste);
}
static void clear_decoder_event(vp_session_t *s)
{ s->ev->bits &= ~EV_WAKE_DECODER; } /* discard initial publications to test an actual blocking wait */

static void release_reader_gate_during_wait(vp_session_t *s, EventBits_t bit)
{
    CHECK(s == test_session && bit == EV_WAKE_READER && s_reader_gate_owned);
    reader_gate_release();
}

static void stop_reader_gate_during_wait(vp_session_t *s, EventBits_t bit)
{
    CHECK(s == test_session && bit == EV_WAKE_READER && s_reader_gate_owned);
    s->quit = true;
    xEventGroupSetBits(s->ev, EV_WAKE_READER);
}

static void test_reader_gate_handoff_and_stop(void)
{
    vp_session_t *s = start(VIDEO_MODE_COMPOSITE, "/__obd_host_no_file__/video.obv");
    allow_timed = true;
    s_reader_gate_owned = true; /* previous reader still owns its DMA buffer */
    before_wait = release_reader_gate_during_wait;
    CHECK(reader_gate_acquire(s));
    CHECK(s_reader_gate_owned && now_us == VP_READER_RETRY_MS * 1000);
    before_wait = stop_reader_gate_during_wait;
    CHECK(!reader_gate_acquire(s));
    CHECK(s_reader_gate_owned); /* stopped waiter must not release another reader */
    reader_gate_release();
    finish();
}
static void finish(void)
{
    before_wait = NULL; on_block = NULL; during_decode = NULL; after_publication = NULL;
    CHECK(video_player_stop() == ESP_OK);
    for (unsigned i = 0; i < task_count; i++) if (!tasks[i].deleted) run_task(i);
    CHECK(!s_cur && !live_allocations && task_delete_count == task_count);
    CHECK(!draw_buf.flushing && !refresh.paused && refresh.timer_cb == draw_refresh);
    if (file_path[0]) CHECK(unlink(file_path) == 0);
    test_session = NULL;
}
/* An ordinary LVGL-loop opportunity, not a scheduling/throughput model. */
static void service_ui(vp_session_t *s)
{
    CHECK(s == s_cur); ui_steps++; video_player_ui_step();
}
static void paused_ui_opportunities(vp_session_t *s)
{
    unsigned steps = ui_steps, notifications = sets_decoder, shown = presented;
    CHECK(s->paused);
    for (unsigned i = 0; i < 1000; i++) { now_us += 10000; service_ui(s); }
    CHECK(ui_steps == steps + 1000 && sets_decoder == notifications && presented == shown);
}
static void release_ring(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_READER && s->ring_full && s->used == 24);
    CHECK(sets_decoder == 1 && !decoded); /* one full-ring notification, not a 50 Hz retry */
    now_us += 1000000; ring_release(s, 12); actions++; on_block = NULL;
}
static void full_reader(bool early)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 24);
    frame(s, 1); frame(s, 2); sets_decoder = 0;
    if (early) before_wait = release_ring; else on_block = release_ring;
    uint32_t at, waste;
    CHECK(ring_reserve(s, 12, &at, &waste));
    CHECK(at == 0 && !waste && s->used == 12 && actions == 1 && signal_waits == 1);
    finish();
}
static void test_full_reader_release(void) { full_reader(false); }
static void test_full_reader_gap(void) { full_reader(true); }
static void stop_waiter(vp_session_t *s, EventBits_t bits)
{
    CHECK(s == test_session && (bits == EV_WAKE_READER || bits == EV_WAKE_DECODER));
    now_us += 1000000;
    if (current_task >= 0) {
        /* Model the UI requesting stop while the decoder is blocked. It must
         * return from its wait and finish before the UI can join it. */
        s->quit = true;
        xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
        on_block = NULL;
        actions++;
        return;
    }
    on_block = NULL; /* Let the stop wait schedule the decoder worker. */
    CHECK(video_player_stop() == ESP_OK);
    CHECK(s->quit && (s->ev->bits & EV_WAKE_READER) && (s->ev->bits & EV_WAKE_DECODER));
    CHECK(s->refs == 1); /* the decoder exited; the reader still owns a reference */
    actions++;
}
static void decoder_done_on_stop(vp_session_t *s, EventBits_t bits)
{
    CHECK(s == test_session && bits == EV_DECODER_DONE && s->quit);
    CHECK(s->mode == VIDEO_MODE_COMPOSITE && !s_cur);
    actions++;
    xEventGroupSetBits(s->ev, EV_DECODER_DONE);
    on_block = NULL;
}
static void test_composite_stop_waits_for_decoder(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    on_block = decoder_done_on_stop;
    CHECK(video_player_stop() == ESP_OK);
    CHECK(actions == 1 && (s->ev->bits & EV_DECODER_DONE));
    CHECK(!s_cur && s->refs == 2);
    finish();
}
static void test_full_reader_stop(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 24);
    frame(s, 1); frame(s, 2); on_block = stop_waiter;
    uint32_t at, waste; CHECK(!ring_reserve(s, 12, &at, &waste));
    CHECK(actions == 1 && signal_waits == 1); finish();
}
static void resume_waiter(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s->paused && !decoded);
    now_us += 10000000; CHECK(video_player_resume() == ESP_OK);
    actions++; on_block = NULL;
}
static void paused_resume(bool early)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    frame(s, 7); s->eof = true;
    CHECK(video_player_pause() == ESP_OK); clear_decoder_event(s);
    if (early) before_wait = resume_waiter; else on_block = resume_waiter;
    vp_out_t out = {0}; CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    dec_run(s, &out); media_jpeg_close(out.j);
    CHECK(decoded == 1 && decoded_marker[0] == 7 && decoded_at[0] == 10000000);
    CHECK(s->state == VIDEO_STATE_STOPPED && !s->frames_dropped && !s->underruns);
    CHECK(actions == 1 && signal_waits == 1 && !timed_waits);
    service_ui(s); CHECK(presented == 1 && shown_marker[0] == 7); finish();
}
static void test_paused_resume(void) { paused_resume(false); }
static void test_paused_gap(void) { paused_resume(true); }
static void test_paused_stop(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    CHECK(video_player_pause() == ESP_OK); clear_decoder_event(s); on_block = stop_waiter;
    vp_out_t out = {0}; dec_run(s, &out);
    CHECK(!decoded && actions == 1 && signal_waits == 1); finish();
}
static void make_file(void)
{
    uint8_t file[100] = {0};
    memcpy(file, "OBV1", 4);
#define W16(o,v) do { file[o] = (uint8_t)(v); file[(o)+1] = (uint8_t)((v) >> 8); } while (0)
#define W32(o,v) do { uint32_t n_ = (v); for (unsigned b_ = 0; b_ < 4; b_++) file[(o)+b_] = (uint8_t)(n_ >> (b_ * 8)); } while (0)
    W16(4, 1); W16(6, 64); W16(8, 16); W16(10, 16); W16(12, 24);
    file[14] = OBV1_SUB_422;
    W32(16, 3); W32(20, 64); W32(24, 88); W32(28, 12); W32(32, 4); W32(36, 125);
    for (unsigned i = 0; i < 3; i++) {
        W32(64 + i * 8, i * 4); W32(68 + i * 8, 4); memset(file + 88 + i * 4, (int)(i + 1), 4);
    }
    W32(40, obv1_crc32(0, file + 64, 24)); W32(44, obv1_crc32(0, file + 88, 12));
    W32(60, obv1_crc32(0, file, 60));
#undef W16
#undef W32
    obv1_info_t in; CHECK(obv1_parse_header(file, sizeof(file), sizeof(file), 0, &in) == OBV1_OK);
    strcpy(file_path, "/tmp/obd-video-wait-XXXXXX"); int fd = mkstemp(file_path); CHECK(fd >= 0);
    FILE *f = fdopen(fd, "wb"); CHECK(f && fwrite(file, 1, sizeof(file), f) == sizeof(file));
    CHECK(fclose(f) == 0);
}
static void read_header(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && !s->hdr_ready && !s->reader_err);
    now_us += 1000000; on_block = NULL; run_task(1); actions++;
}
static void test_header_real_reader(void)
{
    make_file(); vp_session_t *s = start(VIDEO_MODE_COMPOSITE, file_path);
    max_dma_alloc = 512; dma_alloc_attempts = 0;
    on_block = read_header; auto_ui = allow_timed = true;
    run_task(0);
    CHECK(s->hdr_ready && s->eof && s->state == VIDEO_STATE_STOPPED && decoded == 3);
    CHECK(dma_alloc_attempts == 6);
    CHECK(dma_alloc_sizes[0] == 16384 && dma_alloc_sizes[1] == 8192 &&
          dma_alloc_sizes[2] == 4096 && dma_alloc_sizes[3] == 2048 &&
          dma_alloc_sizes[4] == 1024 && dma_alloc_sizes[5] == 512);
    CHECK(signal_waits >= 1 && timed_waits == 2 && actions == 1);
    service_ui(s);
    for (unsigned i = 0; i < 3; i++) CHECK(decoded_marker[i] == i + 1 && shown_marker[i] == i + 1);
    CHECK(presented == 3); max_dma_alloc = SIZE_MAX; finish();
}
static void test_header_reader_error(void)
{
    vp_session_t *s = start(VIDEO_MODE_COMPOSITE, "/__obd_host_no_file__/video.obv");
    on_block = read_header; run_task(0);
    CHECK(s->reader_err && !s->hdr_ready && s->state == VIDEO_STATE_ERROR && !decoded);
    CHECK(signal_waits == 1 && actions == 1); finish();
}
static void test_header_stop(void)
{
    start(VIDEO_MODE_COMPOSITE, "/__obd_host_no_file__/video.obv");
    on_block = stop_waiter; run_task(0);
    CHECK(!decoded && signal_waits == 1 && actions == 1); finish();
}
static void feed_watermark(vp_session_t *s, EventBits_t bits);
static void publish_header(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && !s->hdr_ready && !s->eof);
    uint8_t buf[VP_SD_BUFFER_BYTES];
    vp_sd_stream_t f = { .fd = open(file_path, O_RDONLY), .buf = buf,
                         .cap = VP_SD_BUFFER_BYTES }; CHECK(f.fd >= 0);
    uint8_t *index = reader_open(s, &f);
    CHECK(index && s->hdr_ready && !s->reader_err && !s->frames_buffered);
    heap_caps_free(index); CHECK(close(f.fd) == 0);
    on_block = feed_watermark;
}
static void header_only(bool early)
{
    /* Publish reader_open in isolation. The deferred reader uses a missing
     * path, so cleanup cannot reopen the file and replace the existing ring. */
    make_file(); vp_session_t *s = start(VIDEO_MODE_COMPOSITE, "/__obd_host_no_file__/video.obv");
    if (early) before_wait = publish_header; else on_block = publish_header;
    run_task(0);
    CHECK(decoded == 1 && decoded_marker[0] == 5 && signal_waits == 2 && actions == 1);
    CHECK(s->state == VIDEO_STATE_STOPPED && !s->frames_dropped && !s->underruns);
    finish();
}
static void test_header_publication(void) { header_only(false); }
static void test_header_gap(void) { header_only(true); }
static void feed_watermark(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && !watermark_reached(s, 10));
    now_us += 1000000; frame(s, 5); s->eof = true;
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER); actions++; on_block = NULL;
}
static void finish_watermark_batch(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER);
    if (s->pending >= 0) {
        service_ui(s);
    } else {
        CHECK(!s->used && decoded == 10 && s->underruns == 1);
        s->eof = true; xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
        actions++; on_block = NULL;
    }
}
static void commit_watermark_batch(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && !s->used && !s->eof);
    /* Only production ring_commit publishes the wake; EOF cannot mask it. */
    for (uint8_t i = 1; i <= 10; i++) frame(s, i);
    CHECK(s->frames_buffered == 10 && !s->ring_full && watermark_reached(s, 10));
    on_block = finish_watermark_batch;
}
static void watermark_commit(bool early)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 128);
    allow_timed = auto_ui = true;
    if (early) before_wait = commit_watermark_batch; else on_block = commit_watermark_batch;
    run_task(0); service_ui(s);
    CHECK(decoded == 10 && presented == 10 && s->underruns == 1 && !s->frames_dropped);
    CHECK(actions == 1 && timed_waits == 9 && s->state == VIDEO_STATE_STOPPED);
    for (unsigned i = 0; i < 10; i++) CHECK(decoded_marker[i] == i + 1 && shown_marker[i] == i + 1);
    finish();
}
/* A slow producer must not force another 400 ms refill after startup.
 * Supply one frame per 80 ms; each must be decoded before the next read. */
static void slow_reader_frame(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER);
    if (s->pending >= 0) { service_ui(s); return; }
    CHECK(!s->used && decoded == 10 + actions && s->started && !s->eof);
    if (actions == 20) {
        s->eof = true;
        xEventGroupSetBits(s->ev, EV_WAKE_DECODER);
        on_block = NULL;
        return;
    }
    now_us += 80000;
    frame(s, (uint8_t)(11 + actions));
    actions++;
}
static void test_slow_reader_resumes_each_frame(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 512);
    for (uint8_t i = 1; i <= 10; i++) frame(s, i);
    allow_timed = auto_ui = true; on_block = slow_reader_frame;
    run_task(0); service_ui(s);
    CHECK(s->state == VIDEO_STATE_STOPPED && actions == 20);
    CHECK(decoded == 30 && presented == 30 && !s->frames_dropped);
    for (unsigned i = 10; i < decoded; i++) {
        CHECK(decoded_marker[i] == i + 1 && shown_marker[i] == i + 1);
        /* The startup tail was decoded ahead of its presentation deadline. */
        if (i > 10) CHECK(decoded_at[i] - decoded_at[i - 1] == 80000);
    }
    finish();
}
static void test_watermark_commit(void) { watermark_commit(false); }
static void test_watermark_commit_gap(void) { watermark_commit(true); }
static void test_watermark_data(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    on_block = feed_watermark; run_task(0);
    CHECK(decoded == 1 && decoded_marker[0] == 5 && signal_waits == 1 && actions == 1);
    CHECK(!s->frames_dropped && !s->underruns); finish();
}
static void watermark_terminal(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && !watermark_reached(s, 10));
    now_us += 1000000; s->eof = true;
    xEventGroupSetBits(s->ev, EV_WAKE_DECODER); actions++; on_block = NULL;
}
static void test_watermark_eof(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    on_block = watermark_terminal; run_task(0);
    CHECK(s->state == VIDEO_STATE_STOPPED && !decoded && signal_waits == 1); finish();
}
static void test_watermark_stop(void)
{
    prepared(VIDEO_MODE_COMPOSITE, 64); on_block = stop_waiter; run_task(0);
    CHECK(!decoded && signal_waits == 1 && actions == 1); finish();
}
static void test_watermark_error(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); s->reader_err = true;
    run_task(0); CHECK(s->state == VIDEO_STATE_ERROR && !decoded && !waits); finish();
}
static void drain_full_ring(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER);
    if (s->pending >= 0) {
        /* UI handoff is a different producer from EOF. Do not publish EOF
         * until both frames are consumed and the decoder awaits water. */
        service_ui(s);
    } else {
        CHECK(!s->used && decoded == 2 && s->underruns == 1);
        watermark_terminal(s, bits);
    }
}
static void test_watermark_full(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 24);
    frame(s, 1); frame(s, 2); s->ring_full = true;
    CHECK(watermark_reached(s, 10));
    on_block = drain_full_ring; auto_ui = allow_timed = true;
    run_task(0);
    CHECK(decoded == 2 && s->underruns == 1 && actions == 1 && !s->frames_dropped); finish();
}
static void ui_handoff(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s->pending == 0);
    now_us += 1000000; service_ui(s); actions++; on_block = NULL;
}
static void pending_ui(bool early)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->fb[0] = heap_caps_aligned_alloc(16, 512, 0);
    memset(s->fb[0], 0, 512); ((uint16_t *)s->fb[0])[0] = 6; s->pending = 0;
    if (early) before_wait = ui_handoff; else on_block = ui_handoff;
    CHECK(comp_wait_slot(s)); CHECK(s->pending == -1 && s->front == 0 && presented == 1);
    CHECK(shown_marker[0] == 6 && signal_waits == 1 && actions == 1 && refresh.ready); finish();
}
static void test_obv3_frame_formats(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->info.version = OBV3_VERSION;
    vp_out_t out = {0}; CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    uint8_t record[] = {4, 0, 0, 0, 4, 0, 0, 0, 17, 17, 17, 17, 255, 255, 251, 255};
    /* Reuse both buffers across opaque -> translucent -> opaque transitions. */
    for (unsigned pass = 0; pass < 3; pass++) {
        bool alpha = pass == 1;
        record[13] = record[15] = alpha ? 128 : 255;
        int back = -1;
        CHECK(comp_decode(s, &out, record, sizeof(record), &back) == ESP_OK);
        CHECK(s->pending_alpha == alpha && back != s->front);
        for (unsigned pixel = 0; pixel < 256; pixel++) {
            unsigned offset = pixel * (alpha ? 3 : 2);
            CHECK(s->fb[back][offset] == 17 && s->fb[back][offset + 1] == 0);
            if (alpha) CHECK(s->fb[back][offset + 2] == 128);
        }
        refresh.ready = false;
        comp_publish(s, back); service_ui(s);
        CHECK(s->front == back && s->pending < 0 && refresh.ready);
        CHECK(presented == pass + 1 && shown_marker[pass] == 17);
    }
    record[15] = 0; /* Still a valid plane, with real transparency. */
    int back = -1;
    CHECK(comp_decode(s, &out, record, sizeof(record), &back) == ESP_OK);
    CHECK(s->pending_alpha);
    record[14] = 252; /* Plane now expands past 256 pixels. */
    CHECK(comp_decode(s, &out, record, sizeof(record), &back) == ESP_ERR_INVALID_SIZE);
    CHECK(s->pending < 0 && presented == 3);
    media_jpeg_close(out.j); finish();
}
static void delayed_ui(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s->pending >= 0);
    now_us += 100000;
    service_ui(s);
}
static void test_ui_wait_rechecks_deadline(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    frame(s, 0); frame(s, 1); frame(s, 2); s->eof = true;
    on_block = delayed_ui;
    vp_out_t out = {0}; CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    dec_run(s, &out); media_jpeg_close(out.j);
    CHECK(decoded == 2 && decoded_marker[0] == 0 && decoded_marker[1] == 2);
    CHECK(s->frames_dropped == 1 && decoded_at[1] == 100000);
    service_ui(s); CHECK(presented == 2 && shown_marker[1] == 2);
    finish();
}
static void test_ui_handoff(void) { pending_ui(false); }
static void test_ui_gap(void) { pending_ui(true); }
static void pause_slot(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s->pending == 0);
    CHECK(video_player_pause() == ESP_OK); actions++; on_block = NULL;
}
static void test_ui_pause(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); s->pending = 0;
    on_block = pause_slot; CHECK(!comp_wait_slot(s)); CHECK(signal_waits == 1 && actions == 1);
    finish();
}
static void test_ui_stop(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); s->pending = 0;
    on_block = stop_waiter; CHECK(!comp_wait_slot(s)); CHECK(signal_waits == 1 && actions == 1);
    finish();
}
static void test_paced_frames(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); frame(s, 1); frame(s, 2); s->eof = true;
    allow_timed = auto_ui = true; run_task(0); service_ui(s);
    CHECK(decoded == 2 && presented == 2 && decoded_at[0] == 0 && decoded_at[1] == 0);
    /* Composite decodes early then waits to publish; unchanged ms floor gives 41 ms. */
    CHECK(now_us == 41000 && timed_waits == 1 && !s->frames_dropped && !s->underruns);
    CHECK(shown_marker[0] == 1 && shown_marker[1] == 2); finish();
}
/* Theme prefetch must leave enough PSRAM for decoded frames and the UI. */
static void test_theme_ring_size_and_budget(void)
{
    obv1_info_t info = { .version = OBV3_VERSION, .width = 360, .height = 360,
                        .fps = 24, .frame_count = 240, .max_frame_size = 16384,
                        .data_size = 240u * 16384u };
    uint32_t legacy = ring_size_for(&info, false), theme = ring_size_for(&info, true);
    CHECK(theme > legacy && theme >= 24u * 16384u * 5u / 2u);
    CHECK(theme_ring_budget(&info, theme) == theme);
    free_psram = 2u * 1024u * 1024u;
    size_t reserved = 2u * ((360u * 360u * 3u + 15u) & ~15u) + 360u * 360u + VP_THEME_RESERVE;
    CHECK(theme_ring_budget(&info, VP_THEME_RING_MAX) == ((free_psram - reserved) & ~3u));
    free_psram = reserved - 1;
    CHECK(theme_ring_budget(&info, theme) == 0);
    info.data_size = 240u * 65536u; info.max_frame_size = 65536;
    CHECK(ring_size_for(&info, true) == VP_THEME_RING_MAX);
    info.frame_count = 2; info.data_size = 2u * 65536u;
    CHECK(ring_size_for(&info, true) <= info.data_size + 3u * (65536u + VP_REC_HDR + 4u));
}
static void slow_decode(void) { now_us += 120000; during_decode = slow_decode; }
static uint32_t decode_cost_us;
static void costed_decode(void) { now_us += decode_cost_us; during_decode = costed_decode; }
/* No startup cap when source FPS is achievable. */
static void test_native_pacing(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 4096); s->info.fps = 60;
    for (unsigned i = 1; i <= 240; i++) frame(s, (uint8_t)i);
    s->eof = true; allow_timed = auto_ui = true;
    decode_cost_us = 10000; during_decode = costed_decode;
    run_task(0); service_ui(s);
    CHECK(decoded == 240 && presented == 240 && !s->frames_dropped);
    CHECK(now_us >= 3980000 && now_us <= 4000000); finish();
}
/* A 40 ms decoder can sustain 25 fps; it must not be quantized to half rate. */
static void test_near_budget_pacing(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 4096); s->info.fps = 30;
    for (unsigned i = 1; i <= 240; i++) frame(s, (uint8_t)i);
    s->eof = true; allow_timed = auto_ui = true;
    decode_cost_us = 40000; during_decode = costed_decode;
    run_task(0); service_ui(s);
    CHECK(decoded >= 195 && decoded <= 202 && decoded == presented);
    CHECK(now_us >= 7960000 && now_us <= 8080000);
    CHECK(!s->underruns);
    for (unsigned i = 1; i < decoded; i++) CHECK(decoded_at[i] - decoded_at[i - 1] <= 41000);
    finish();
}
static void test_slow_theme_keeps_playing(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 4096);
    for (unsigned i = 1; i <= 240; i++) frame(s, (uint8_t)i);
    s->eof = true; allow_timed = auto_ui = true; during_decode = slow_decode;
    run_task(0); service_ui(s);
    CHECK(s->state == VIDEO_STATE_STOPPED && decoded == presented && decoded > 40);
    for (unsigned i = 1; i < decoded; i++) CHECK(decoded_at[i] - decoded_at[i - 1] < 500000);
    finish();
}
static void recovering_decode(void)
{
    now_us += decoded < 20 ? 120000 : 10000;
    during_decode = recovering_decode;
}
static void test_slow_pacing_recovers(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 4096);
    s->info.fps = 30;
    for (unsigned i = 1; i <= 240; i++) frame(s, (uint8_t)i);
    s->eof = true; allow_timed = auto_ui = true;
    during_decode = recovering_decode;
    run_task(0); service_ui(s);
    CHECK(decoded > 180 && decoded == presented);
    /* Recovery follows the source cadence immediately, without waiting for
     * several one-second adaptive-tier cooldowns. */
    for (unsigned i = decoded - 60; i < decoded; i++) {
        CHECK(decoded_marker[i] == decoded_marker[i - 1] + 1);
        CHECK(decoded_at[i] - decoded_at[i - 1] <= 34000);
    }
    finish();
}
static void test_target_fps_pacing(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 4096);
    s->info.fps = 30; s->target_fps = 24;
    for (unsigned i = 1; i <= 240; i++) frame(s, (uint8_t)i);
    s->eof = true; allow_timed = auto_ui = true;
    decode_cost_us = 10000; during_decode = costed_decode;
    run_task(0); service_ui(s);
    CHECK(decoded == 192 && presented == 192 && s->frames_dropped == 48);
    CHECK(now_us >= 7960000 && now_us <= 8000000);
    finish();
}
static void pause_deadline(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s == test_session && !s->paused);
    now_us += 1000; CHECK(video_player_pause() == ESP_OK); actions++;
}
static void test_deadline_pause(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    allow_timed = true; before_wait = pause_deadline;
    CHECK(!wait_until(s, 10000000));
    CHECK(s->paused && now_us == 1000 && timed_waits == 1 && actions == 1); finish();
}
static void test_deadline_stop(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    allow_timed = true; before_wait = stop_waiter;
    CHECK(!wait_until(s, 10000000));
    CHECK(s->quit && now_us == 1000000 && timed_waits == 1 && actions == 1); finish();
}
static void test_fullscreen_finish(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64); frame(s, 9); s->eof = true;
    CHECK(refresh.paused); run_task(0);
    CHECK(s->state == VIDEO_STATE_STOPPED && decoded == 1 && panel_draws == 10);
    CHECK(refresh.paused && !draw_buf.flushing && (s->ev->bits & EV_DECODER_DONE));
    finish(); CHECK(invalidations == 1 && !timer_deletes);
}
static void test_fullscreen_decode_ahead(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    vp_out_t out = {0};
    CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    uint8_t jpg[] = {9, 9, 9, 9};
    allow_timed = true; during_decode = costed_decode; decode_cost_us = 10000;
    CHECK(fs_present(s, &out, jpg, sizeof(jpg), 50000) == ESP_OK);
    CHECK(decoded == 1 && decoded_at[0] == 0);
    CHECK(fullscreen_at[0] == 50000 && out.pace_wait_us == 40000);
    /* Frame two decodes while frame one is already displayed. */
    s->started = true; jpg[0] = 10;
    CHECK(fs_present(s, &out, jpg, sizeof(jpg), 100000) == ESP_OK);
    CHECK(decoded_at[1] == 50000 && fullscreen_at[1] == 100000);
    media_jpeg_close(out.j); finish();
}
static void test_fullscreen_strip_pingpong(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    vp_out_t out = {.block = true};
    CHECK(media_jpeg_open(false, &out.j) == ESP_OK); out.j->marker = 7;
    media_jpeg_info_t ji = {16, 16, 8, 256, 2};
    CHECK(fs_prepare(s, &out, &ji) == ESP_OK);
    out.strip_lines = 8; allow_timed = true;
    unsigned before = panel_draws;
    CHECK(fs_blocks(s, &out, &ji, 50000) == ESP_OK);
    CHECK(block_count == 2 && block_times[0] == 0 && block_times[1] == 50000);
    CHECK(block_outputs[0] != block_outputs[1] && panel_draws == before + 2);
    CHECK(fs_blocks(s, &out, &ji, 100000) == ESP_OK);
    CHECK(block_outputs[2] == block_outputs[0] && block_outputs[3] == block_outputs[1]);
    CHECK(block_times[2] == 54000 && block_times[3] == 100000);
    media_jpeg_close(out.j); finish();
}
static void test_fullscreen_decode_pause(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    vp_out_t out = {0}; CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    uint8_t jpg[] = {9, 9, 9, 9};
    allow_timed = true; before_wait = pause_deadline;
    CHECK(fs_present(s, &out, jpg, sizeof(jpg), 50000) == ESP_ERR_INVALID_STATE);
    CHECK(s->paused && decoded == 1 && !fullscreen_frames);
    before_wait = NULL; CHECK(video_player_resume() == ESP_OK);
    CHECK(fs_present(s, &out, jpg, sizeof(jpg), 50000) == ESP_OK);
    CHECK(decoded == 2 && fullscreen_frames == 1 && fullscreen_at[0] == 50000);
    media_jpeg_close(out.j); finish();
}
static void test_first_frame_waits_for_dma(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    CHECK(!video_player_first_frame_presented());
    frame(s, 9); s->eof = true; delay_frame_dma = true;
    run_task(0);
    CHECK(dma_wait_checks && !dma_done_at && !draw_buf.flushing && now_us == 200);
    CHECK(video_player_first_frame_presented() && s->state == VIDEO_STATE_STOPPED);
    finish();
    CHECK(!video_player_first_frame_presented());
}
static vp_session_t *prepared_timed(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    s->info.version = OBV2_VERSION;
    s->timed_index = heap_caps_calloc(3, OBV2_INDEX_ENTRY, MALLOC_CAP_SPIRAM);
    const uint32_t times[] = {0, 33333, 100000};
    const uint32_t durations[] = {33333, 66667, 50000};
    for (uint32_t n = 0; n < 3; n++) {
        uint8_t *e = s->timed_index + n * OBV2_INDEX_ENTRY;
        for (unsigned b = 0; b < 4; b++) {
            e[8 + b] = (uint8_t)(times[n] >> (8 * b));
            e[12 + b] = (uint8_t)(durations[n] >> (8 * b));
        }
    }
    allow_timed = true;
    return s;
}
static void test_timed_fullscreen_finish(void)
{
    vp_session_t *s = prepared_timed();
    frame(s, 1); frame(s, 2); frame(s, 3); s->eof = true;
    run_task(0);
    CHECK(s->state == VIDEO_STATE_STOPPED && decoded == 3 && !s->frames_dropped);
    CHECK(fullscreen_frames == 3 && panel_draws == 12);
    for (unsigned n = 0; n < fullscreen_frames; n++) CHECK(fullscreen_marker[n] == n + 1);
    /* wait_until uses millisecond ticks and may submit less than 1 ms early. */
    CHECK(fullscreen_at[0] == 0 && fullscreen_at[1] >= 33000 && fullscreen_at[1] <= 33333 &&
          fullscreen_at[2] == 100000);
    CHECK(now_us == 150000); /* hold the last frame's duration, no legacy grace wait */
    finish();
}
/* Model decode cost independently of source cadence. No hardware FPS claim. */
static vp_session_t *prepared_timed_cfr(void)
{
    vp_session_t *s = prepared_timed();
    heap_caps_free(s->timed_index);
    s->info.frame_count = 10;
    s->timed_index = heap_caps_calloc(10, OBV2_INDEX_ENTRY, MALLOC_CAP_SPIRAM);
    heap_caps_free(s->ring);
    s->ring_cap = 256;
    s->ring = heap_caps_malloc(s->ring_cap, MALLOC_CAP_SPIRAM);
    for (uint32_t n = 0; n < 10; n++) {
        uint8_t *e = s->timed_index + n * OBV2_INDEX_ENTRY;
        uint32_t time = n * 33333u;
        for (unsigned b = 0; b < 4; b++) {
            e[8 + b] = (uint8_t)(time >> (8 * b));
            e[12 + b] = (uint8_t)(33333u >> (8 * b));
        }
        frame(s, (uint8_t)(n + 1));
    }
    s->eof = true;
    return s;
}
static void test_timed_slow_decode_completes(void)
{
    vp_session_t *s = prepared_timed_cfr();
    decode_cost_us = 60000; during_decode = costed_decode;
    run_task(0);
    CHECK(s->state == VIDEO_STATE_STOPPED && s->frames_dropped > 0);
    CHECK(decoded + s->frames_dropped == 10 && fullscreen_frames == decoded);
    CHECK(fullscreen_marker[0] == 1 && fullscreen_marker[fullscreen_frames - 1] == 10);
    CHECK(now_us >= 333330 && now_us < 500000);
    CHECK(!s->used && !s->frames_buffered);
    finish();
}
static void timed_stall(void)
{
    if (decoded == 2) now_us += 250000;
    during_decode = timed_stall;
}
static void test_timed_stall_catches_up(void)
{
    vp_session_t *s = prepared_timed_cfr(); during_decode = timed_stall;
    run_task(0);
    CHECK(s->state == VIDEO_STATE_STOPPED && s->frames_dropped == 5);
    CHECK(decoded + s->frames_dropped == 10);
    CHECK(fullscreen_marker[0] == 1 && fullscreen_marker[fullscreen_frames - 1] == 10);
    /* FreeRTOS waits round down to whole milliseconds. */
    CHECK(now_us >= 333000 && now_us <= 333330);
    CHECK(!s->used && !s->frames_buffered);
    finish();
}
static void test_timed_decode_error_remains_fatal(void)
{
    vp_session_t *s = prepared_timed_cfr(); fail_decode = true;
    run_task(0);
    CHECK(s->state == VIDEO_STATE_ERROR && !fullscreen_frames && !s->started);
    finish();
}
static void test_timed_truncated_prebuffer(void)
{
    vp_session_t *s = prepared_timed();
    frame(s, 1); frame(s, 2); s->eof = true;
    run_task(0);
    CHECK(s->state == VIDEO_STATE_ERROR && !decoded && now_us == 0);
    CHECK(!video_player_first_frame_presented());
    finish();
}
static void stop_decoder(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_DECODER_DONE && s->quit && refresh.paused);
    CHECK((s->ev->bits & EV_WAKE_READER) && (s->ev->bits & EV_WAKE_DECODER));
    on_block = NULL; run_task(0); actions++;
}
static void test_fullscreen_stop(void)
{
    vp_session_t *s = start(VIDEO_MODE_FULLSCREEN, "/__obd_host_no_file__/video.obv");
    CHECK(video_player_pause() == ESP_OK); on_block = stop_decoder;
    CHECK(video_player_stop() == ESP_OK);
    CHECK(!decoded && !refresh.paused && invalidations == 1 && actions == 1 && s->refs == 1);
    finish();
}
static void test_task_create_failure(void)
{
    fail_task_attempt = 2;
    video_config_t cfg = { .mode = VIDEO_MODE_COMPOSITE, .source = VIDEO_SRC_SD,
                          .path = "/__obd_host_no_file__/video.obv", .video_view = &view };
    CHECK(video_player_start(&cfg) == ESP_ERR_NO_MEM);
    CHECK(!s_cur && task_count == 1 && tasks[0].deleted);
    CHECK(!live_allocations && !decoded && task_delete_count == 1);
}
static void test_path_limit(void)
{
    _Static_assert(VIDEO_PLAYER_PATH_MAX >= 153u, "theme video paths must fit");
    char path[VIDEO_PLAYER_PATH_MAX + 1u];
    memset(path, 'a', sizeof(path));
    path[sizeof(path) - 1u] = '\0';
    video_config_t cfg = { .mode = VIDEO_MODE_COMPOSITE, .source = VIDEO_SRC_SD,
                          .path = path, .video_view = &view };
    CHECK(video_player_start(&cfg) == ESP_ERR_INVALID_ARG);
    CHECK(!s_cur && !task_count && !live_allocations);
}

static int decoded_pending(vp_session_t *s)
{
    vp_out_t out = {0}; int b = -1;
    static const uint8_t jpeg[] = { 11, 11, 11, 11 };
    CHECK(media_jpeg_open(false, &out.j) == ESP_OK);
    CHECK(comp_decode(s, &out, jpeg, sizeof(jpeg), &b) == ESP_OK);
    media_jpeg_close(out.j);
    CHECK(b == 0 && s->pending < 0 && s->front < 0);
    s->started = true; s->state = VIDEO_STATE_PLAYING;
    return b;
}
static void test_ui_event_pause_loading(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    CHECK(!s->paused && s->state == VIDEO_STATE_LOADING);
    CHECK(video_player_pause() == ESP_OK); paused_ui_opportunities(s);
    CHECK(!decoded && s->pending < 0);
    CHECK(video_player_resume() == ESP_OK);
    CHECK(!s->paused && s->state == VIDEO_STATE_LOADING);
    service_ui(s); CHECK(!presented); finish();
}
static void paused_pending(bool late)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    int b = decoded_pending(s);
    if (!late) comp_publish(s, b);
    CHECK(video_player_pause() == ESP_OK);
    if (late) comp_publish(s, b); /* pause between wait_until and publication */
    paused_ui_opportunities(s);
    CHECK(s->pending == b && s->front < 0 && decoded == 1 && !presented);
    CHECK(video_player_resume() == ESP_OK);
    CHECK(!s->paused && s->state == VIDEO_STATE_PLAYING);
    service_ui(s);
    CHECK(presented == 1 && shown_marker[0] == 11);
    CHECK(s->front == b && s->pending < 0 && (s->ev->bits & EV_WAKE_DECODER));
    CHECK(comp_wait_slot(s)); finish();
}
static void test_ui_event_pause_pending(void) { paused_pending(false); }
static void test_ui_event_pause_late_publish(void) { paused_pending(true); }
static void pause_during_decode(void)
{
    CHECK(decoded == 1 && test_session->pending < 0 && !presented);
    CHECK(video_player_pause() == ESP_OK); actions++;
}
static void resume_decoder_event(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER && s->paused && decoded == 1 && actions == 1);
    paused_ui_opportunities(s);
    CHECK(video_player_resume() == ESP_OK && !s->paused);
    on_block = NULL; actions++;
}
static void test_ui_event_pause_decode(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); frame(s, 12); s->eof = true;
    during_decode = pause_during_decode; on_block = resume_decoder_event;
    run_task(0); service_ui(s);
    CHECK(actions == 2 && decoded == 2 && presented == 1 && shown_marker[0] == 12);
    CHECK(decoded_marker[0] == 12 && decoded_marker[1] == 12);
    CHECK(decoded_at[0] == 0 && decoded_at[1] == 10000000);
    CHECK(!s->frames_dropped && !s->underruns && s->state == VIDEO_STATE_STOPPED); finish();
}
static void pause_after_publish(vp_session_t *s)
{
    CHECK(s->pending == 0 && decoded == 1 && !presented);
    CHECK(video_player_pause() == ESP_OK); actions++;
}
static void test_ui_event_pause_publish(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64); frame(s, 13); s->eof = true;
    after_publication = pause_after_publish; on_block = resume_decoder_event;
    run_task(0); service_ui(s);
    CHECK(actions == 2 && decoded == 1 && presented == 1 && shown_marker[0] == 13);
    CHECK(!s->frames_dropped && !s->underruns && s->state == VIDEO_STATE_STOPPED); finish();
}
static void test_ui_event_pause_stop(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    comp_publish(s, decoded_pending(s)); CHECK(video_player_pause() == ESP_OK);
    paused_ui_opportunities(s); finish();
    CHECK(!presented && timer_deletes == 0 && video_player_resume() == ESP_ERR_INVALID_STATE);
}
static void test_ui_event_fullscreen_control(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    CHECK(refresh.paused);
    CHECK(video_player_pause() == ESP_OK && refresh.paused);
    CHECK(video_player_resume() == ESP_OK && refresh.paused);
    s->eof = true; run_task(0); CHECK(refresh.paused && !decoded);
    finish(); CHECK(!refresh.paused && !timer_deletes && invalidations == 1);
}
/* Page loads and dirty layouts can resume the timer while video owns draw1/2.
 * Exercise the installed callback, rather than checking the paused flag alone. */
static void test_fullscreen_invalidation_guard(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    memset(draw1, 0x5a, sizeof(draw1));
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_invalidate(&screen);
        CHECK(!refresh.paused);
        refresh.timer_cb(&refresh);
        CHECK(refresh.paused && !refresh_draws);
        for (unsigned j = 0; j < sizeof(draw1); j++) CHECK(draw1[j] == 0x5a);
    }
    s->eof = true;
    run_task(0);
    /* Retain the final panel image even after the decoder has naturally ended. */
    lv_obj_invalidate(&screen);
    refresh.timer_cb(&refresh);
    CHECK(refresh.paused && !refresh_draws);
    finish();
    refresh.timer_cb(&refresh);
    CHECK(refresh_draws == 1 && draw1[0] == 0);
}
static void test_ui_event_invalid_control(void)
{
    CHECK(video_player_pause() == ESP_ERR_INVALID_STATE);
    CHECK(video_player_resume() == ESP_ERR_INVALID_STATE);
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->state = VIDEO_STATE_ERROR;
    CHECK(video_player_pause() == ESP_ERR_INVALID_STATE && !s->paused);
    CHECK(video_player_resume() == ESP_ERR_INVALID_STATE && !s->paused);
    s->state = VIDEO_STATE_LOADING;
    CHECK(video_player_pause() == ESP_OK);
    CHECK(video_player_pause() == ESP_ERR_INVALID_STATE && s->paused);
    CHECK(video_player_resume() == ESP_OK);
    CHECK(video_player_resume() == ESP_ERR_INVALID_STATE && !s->paused);
    CHECK(task_count == 2 && !timer_deletes); finish();
}

/* Execute the real boot-lifetime runtime against the real player. No copied
 * app loop: escapes are only at the RTOS wait edge after releasing lvgl_mux. */
static void run_runtime(unsigned n)
{
    CHECK(!runtime_active && !mux_held && n);
    runtime_limit = n; runtime_active = true;
    if (!setjmp(runtime_escape)) lvgl_runtime_task(&view);
    runtime_active = false;
    CHECK(!mux_held && runtime_handlers == n && runtime_waits == n);
#if CONFIG_OBD_PERF_STATS
    CHECK(perf_counts[PERF_LVGL_WAIT] == n && perf_counts[PERF_LVGL_HANDLER] == n);
#endif
}
static void next_tpms_schedule_case(void)
{
    now_us += 250000;
    switch (runtime_handlers) {
    case 1: runtime_cfg.ble_obd_mac[0] = 1; break;
    case 2: runtime_elm_connected = true; break;
    case 3: runtime_elm_connected = false; runtime_cfg.device_role = ESPNOW_ROLE_SLAVE; break;
    case 4:
        runtime_cfg.device_role = ESPNOW_ROLE_MASTER;
        runtime_cfg.ble_obd_mac[0] = 0; strcpy(runtime_cfg.ble_device_name, "ELM"); break;
    }
}
static void test_runtime_tpms_schedule(void)
{
    now_us = 250000; handler_delay = 2;
    check_tpms_schedule = true; at_handler = next_tpms_schedule_case;
    run_runtime(5);
    CHECK(tpms_service_calls == 5);
}
static void select_delay(void)
{
    static const uint32_t delays[] = { 0, 1, 2, 10, 500, UINT32_MAX };
    CHECK(runtime_handlers <= COUNT_OF(delays)); handler_delay = delays[runtime_handlers - 1];
}
static void test_runtime_timer_deadlines(void)
{
    at_handler = select_delay; run_runtime(6);
    static const TickType_t expected[] = { 2, 2, 2, 10, 500, 500 };
    CHECK(!memcmp(expected, runtime_ticks, sizeof(expected)));
    CHECK(now_us == 1016000 && timeout_returns == 6 && !event_returns && !wake_calls);
    CHECK(!live_allocations && !presented && !timer_deletes);
}
static void observe_first_frame(void)
{ CHECK(presented == 1 && shown_marker[0] == 11 && test_session->pending < 0 && refresh.ready); }
static void test_runtime_before_registration(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    comp_publish(s, decoded_pending(s));
    CHECK(!wake_calls && !notify_bits); /* no registered task yet */
    unsigned allocs = live_allocations;
    at_handler = observe_first_frame; run_runtime(2);
    CHECK(presented == 1 && s->pending < 0 && s->front == 0);
    CHECK(!event_returns && timeout_returns == 2 && live_allocations == allocs);
    finish();
}
static void publish_for_runtime(void)
{
    before_notify_wait = NULL; notify_block = NULL;
    comp_publish(test_session, decoded_pending(test_session)); actions++;
}
static void runtime_publication(bool gap)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    if (gap) before_notify_wait = publish_for_runtime; else notify_block = publish_for_runtime;
    run_runtime(2);
    CHECK(actions == 1 && wake_calls == 1 && event_returns == 1 && timeout_returns == 1);
    CHECK(now_us == 500000 && presented == 1 && shown_marker[0] == 11 && s->pending < 0);
    CHECK((s->ev->bits & EV_WAKE_DECODER) && !notify_bits); finish();
}
static void test_runtime_publish_gap(void) { runtime_publication(true); }
static void test_runtime_publish_blocked(void) { runtime_publication(false); }
static void coalesced_wakes(void)
{
    before_notify_wait = NULL;
    for (unsigned i = 0; i < 10000; i++) lvgl_runtime_wake();
}
static void test_runtime_coalescing(void)
{
    before_notify_wait = coalesced_wakes; run_runtime(2);
    CHECK(wake_calls == 10000 && event_returns == 1 && timeout_returns == 1 && !notify_bits);
    CHECK(!presented && !live_allocations && now_us == 500000);
}
static void resume_for_runtime(void)
{
    CHECK(test_session->paused && test_session->pending >= 0 && !presented);
    notify_block = NULL; CHECK(video_player_resume() == ESP_OK); actions++;
}
static void test_runtime_resume_pending(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    comp_publish(s, decoded_pending(s)); CHECK(video_player_pause() == ESP_OK);
    notify_block = resume_for_runtime; run_runtime(2);
    CHECK(actions == 1 && wake_calls == 1 && event_returns == 1 && timeout_returns == 1);
    CHECK(presented == 1 && s->pending < 0 && !s->paused); finish();
}
static void pause_publish_for_runtime(void)
{
    before_notify_wait = NULL;
    int b = decoded_pending(test_session);
    CHECK(video_player_pause() == ESP_OK); comp_publish(test_session, b); actions++;
}
static void resume_from_handler(void)
{
    if (runtime_handlers == 2) {
        CHECK(!presented && test_session->pending == 0 && test_session->paused);
        CHECK(video_player_resume() == ESP_OK); actions++;
    }
}
static void test_runtime_late_paused_publish(void)
{
    prepared(VIDEO_MODE_COMPOSITE, 64);
    before_notify_wait = pause_publish_for_runtime; at_handler = resume_from_handler;
    run_runtime(3);
    CHECK(actions == 2 && wake_calls == 2 && event_returns == 2 && timeout_returns == 1);
    CHECK(presented == 1 && shown_marker[0] == 11); finish();
}
static void test_runtime_paused_idle(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    comp_publish(s, decoded_pending(s)); CHECK(video_player_pause() == ESP_OK);
    run_runtime(3);
    CHECK(!wake_calls && !event_returns && timeout_returns == 3 && now_us == 1500000);
    CHECK(!presented && s->pending == 0 && s->front < 0); finish();
}
static void stop_publish_for_runtime(void)
{
    before_notify_wait = NULL;
    int b = decoded_pending(test_session);
    CHECK(video_player_stop() == ESP_OK && test_session->quit && !s_cur);
    comp_publish(test_session, b); /* worker reference retains retired buffers */
    CHECK(!test_session->lock.depth); actions++;
}
static void test_runtime_stop_late_notification(void)
{
    prepared(VIDEO_MODE_COMPOSITE, 64);
    before_notify_wait = stop_publish_for_runtime; run_runtime(2);
    CHECK(actions == 1 && wake_calls == 1 && event_returns == 1 && timeout_returns == 1);
    CHECK(!presented && !s_cur); finish();
}
static vp_session_t *retired;
static int retired_buffer;
static void replace_publish_for_runtime(void)
{
    before_notify_wait = NULL;
    comp_publish(retired, retired_buffer);
    CHECK(retired->quit && !retired->lock.depth);
    int b = decoded_pending(test_session);
    ((uint16_t *)test_session->fb[b])[0] = 22;
    comp_publish(test_session, b); actions++;
}
static void test_runtime_replaced_session(void)
{
    retired = prepared(VIDEO_MODE_COMPOSITE, 64);
    uint32_t retired_id = video_player_session_id();
    retired_buffer = decoded_pending(retired); session_ref(retired);
    CHECK(video_player_stop() == ESP_OK);
    CHECK(video_player_session_id() == 0);
    CHECK(tasks[0].deleted && !tasks[1].deleted);
    run_task(1); CHECK(retired->refs == 1);
    memset(tasks, 0, sizeof(tasks)); task_count = task_delete_count = create_attempts = 0;
    test_session = NULL;
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    CHECK(video_player_session_id() != retired_id);
    before_notify_wait = replace_publish_for_runtime; run_runtime(2);
    CHECK(actions == 1 && wake_calls == 2 && event_returns == 1 && timeout_returns == 1);
    CHECK(presented == 1 && shown_marker[0] == 22 && s->pending < 0);
    CHECK(retired->pending == retired_buffer && retired->front < 0);
    session_unref(retired); retired = NULL; finish();
}
static void test_runtime_fullscreen_guard(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_FULLSCREEN, 64);
    s->pending = 0; /* never a composite frame; ui_step must ignore this session */
    before_notify_wait = coalesced_wakes; run_runtime(2);
    CHECK(refresh.paused && !presented && s->pending == 0 && !decoded);
    CHECK(wake_calls == 10000 && event_returns == 1 && timeout_returns == 1);
    on_block = stop_decoder; CHECK(video_player_stop() == ESP_OK);
    finish(); CHECK(!refresh.paused && invalidations == 1);
}

/* Real SD indexed reads: forward, backward, A -> B -> A, pause/resume. */
static void value_reader_action(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_READER && !s->eof && !s->reader_err);
    if (actions == 5) {
        CHECK(s->paused && !s->used);
        CHECK(video_player_set_position(.5) == ESP_OK);
        CHECK(video_player_resume() == ESP_OK);
        actions++;
        return;
    }
    const unsigned expected[] = {0, 2, 1, 0, 0, 99, 1};
    uint32_t size, rec, index; const uint8_t *jpg;
    CHECK(actions < COUNT_OF(expected) && ring_peek(s, &size, &jpg, &rec));
    memcpy(&index, jpg - VP_REC_HDR, sizeof(index));
    CHECK(index == expected[actions] && size == 4 && jpg[0] == index + 1);
    ring_release(s, rec);
    switch (actions++) {
    case 0: CHECK(video_player_set_position(2) == ESP_OK); break;
    case 1: CHECK(video_player_set_position(.5) == ESP_OK); break;
    case 2: CHECK(video_player_set_position(-1) == ESP_OK); break;
    case 3:
        CHECK(video_player_set_position(1) == ESP_OK);
        CHECK(video_player_set_position(0) == ESP_OK); /* same frame, new revision */
        break;
    case 4: CHECK(video_player_pause() == ESP_OK); break;
    case 6:
        s->quit = true;
        xEventGroupSetBits(s->ev, EV_WAKE_READER | EV_WAKE_DECODER);
        break;
    default: CHECK(false);
    }
}
static void test_value_reader_seek(void)
{
    make_file();
    video_config_t cfg = { .mode = VIDEO_MODE_COMPOSITE, .source = VIDEO_SRC_SD,
        .path = file_path, .video_view = &view, .value_driven = true };
    CHECK(video_player_start(&cfg) == ESP_OK);
    vp_session_t *s = test_session = s_cur;
    on_block = value_reader_action;
    run_task(1);
    CHECK(actions == 7 && !s->reader_err && !s->eof && !timed_waits);
    CHECK(video_player_set_position(NAN) == ESP_ERR_INVALID_STATE);
    finish();
}
static void test_value_frame_wakes(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->value_driven = true; s->ev->bits = 0;
    uint32_t revision = s->position_revision;
    for (unsigned i=0; i<1000; i++) {
        CHECK(video_player_set_position((i % 10) * .001) == ESP_OK);
        CHECK(s->position_revision == revision && !s->ev->bits);
    }
    CHECK(video_player_set_position(.5) == ESP_OK);
    CHECK(s->position_revision == revision + 1 &&
          (s->ev->bits & (EV_WAKE_READER | EV_WAKE_DECODER)) == (EV_WAKE_READER | EV_WAKE_DECODER));
    s->ev->bits = 0;
    CHECK(video_player_set_position(.501) == ESP_OK);
    CHECK(s->position_revision == revision + 1 && !s->ev->bits);
    CHECK(video_player_set_position(0) == ESP_OK); // A -> B -> A still requires a new read
    CHECK(s->position_revision == revision + 2 && s->ev->bits);
    finish();
}
static void value_decode_moving(void)
{
    CHECK(video_player_set_position(1) == ESP_OK);
    frame(test_session, 2); /* new request arrives during an expensive decode */
}
static void value_decoder_action(vp_session_t *s, EventBits_t bits)
{
    CHECK(bits == EV_WAKE_DECODER);
    if (s->pending >= 0) { service_ui(s); return; }
    switch (actions++) {
    case 0:
        CHECK(presented == 2 && shown_marker[0] == 0 && shown_marker[1] == 2);
        CHECK(video_player_set_position(.5) == ESP_OK); frame(s, 1); break;
    case 1:
        CHECK(presented == 3 && shown_marker[2] == 1);
        CHECK(video_player_pause() == ESP_OK); break;
    case 2:
        CHECK(s->paused && presented == 3);
        CHECK(video_player_set_position(0) == ESP_OK);
        CHECK(video_player_resume() == ESP_OK); frame(s, 0); break;
    case 3:
        CHECK(presented == 4 && shown_marker[3] == 0);
        s->quit = true; xEventGroupSetBits(s->ev, EV_WAKE_DECODER); break;
    default: CHECK(false);
    }
}
static void test_value_decoder_latest(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->value_driven = true;
    frame(s, 2); /* obsolete request must not decode */
    frame(s, 0);
    during_decode = value_decode_moving; on_block = value_decoder_action;
    run_task(0);
    CHECK(decoded == 4 && presented == 4 && actions == 4 && !timed_waits);
    CHECK(decoded_marker[0] == 0 && decoded_marker[1] == 2 && decoded_marker[2] == 1 && decoded_marker[3] == 0);
    finish();
}
static void test_value_decoder_error(void)
{
    vp_session_t *s = prepared(VIDEO_MODE_COMPOSITE, 64);
    s->value_driven = true; fail_decode = true; frame(s, 0);
    run_task(0);
    CHECK(s->state == VIDEO_STATE_ERROR && decoded == 1 && !presented && !timed_waits);
    finish();
}

/* Exercise read-ahead beyond one DMA block, frame-padding seeks within the
 * cache, a backwards loop seek, and EOF. These are real POSIX file reads. */
static void test_sd_bulk_read_ahead(void)
{
    size_t len = VP_SD_BUFFER_BYTES * 2u + 83u;
    uint8_t *expected = malloc(len), *actual = malloc(len), *buf = malloc(VP_SD_BUFFER_BYTES);
    CHECK(expected && actual && buf);
    for (size_t i = 0; i < len; i++) expected[i] = (uint8_t)(i * 37u + i / 251u);
    char path[] = "/tmp/obd-sd-stream-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0); CHECK(unlink(path) == 0);
    CHECK(write(fd, expected, len) == (ssize_t)len); CHECK(lseek(fd, 0, SEEK_SET) == 0);
    vp_sd_stream_t f = { .fd = fd, .buf = buf, .cap = VP_SD_BUFFER_BYTES };
    CHECK(sd_stream_read(&f, actual, 37) == 37 && !memcmp(expected, actual, 37));
    off_t physical = lseek(fd, 0, SEEK_CUR);
    CHECK(physical == VP_SD_BUFFER_BYTES);
    CHECK(sd_stream_seek(&f, 40)); /* skip frame padding without dropping read-ahead */
    CHECK(lseek(fd, 0, SEEK_CUR) == physical);
    CHECK(sd_stream_read(&f, actual, len - 40) == len - 40);
    CHECK(!memcmp(expected + 40, actual, len - 40));
    CHECK(sd_stream_read(&f, actual, 1) == 0);
    CHECK(sd_stream_seek(&f, 12));
    CHECK(sd_stream_read(&f, actual, len - 12) == len - 12);
    CHECK(!memcmp(expected + 12, actual, len - 12));
    CHECK(sd_stream_seek(&f, len + 100)); CHECK(sd_stream_read(&f, actual, 1) == 0);
    CHECK(close(fd) == 0); free(expected); free(actual); free(buf);
}

typedef struct { const char *name; void (*fn)(void); } test_t;
int main(void)
{
    const test_t tests[] = {
#define TEST(f) { #f, f }
        TEST(test_sd_bulk_read_ahead), TEST(test_reader_gate_handoff_and_stop),
        TEST(test_full_reader_release), TEST(test_full_reader_gap), TEST(test_full_reader_stop),
        TEST(test_composite_stop_waits_for_decoder),
        TEST(test_paused_resume), TEST(test_paused_gap), TEST(test_paused_stop),
        TEST(test_header_real_reader), TEST(test_header_reader_error), TEST(test_header_stop),
        TEST(test_header_publication), TEST(test_header_gap),
        TEST(test_watermark_commit), TEST(test_watermark_commit_gap), TEST(test_slow_reader_resumes_each_frame),
        TEST(test_watermark_data), TEST(test_watermark_eof), TEST(test_watermark_error), TEST(test_watermark_stop),
        TEST(test_watermark_full), TEST(test_ui_handoff), TEST(test_ui_gap), TEST(test_obv3_frame_formats), TEST(test_ui_wait_rechecks_deadline),
        TEST(test_ui_pause), TEST(test_ui_stop), TEST(test_paced_frames),
        TEST(test_near_budget_pacing), TEST(test_native_pacing), TEST(test_target_fps_pacing),
        TEST(test_theme_ring_size_and_budget), TEST(test_slow_theme_keeps_playing), TEST(test_slow_pacing_recovers),
        TEST(test_deadline_pause), TEST(test_deadline_stop),
        TEST(test_fullscreen_finish), TEST(test_fullscreen_decode_ahead), TEST(test_fullscreen_strip_pingpong), TEST(test_fullscreen_decode_pause), TEST(test_first_frame_waits_for_dma), TEST(test_fullscreen_stop), TEST(test_task_create_failure),
        TEST(test_timed_fullscreen_finish), TEST(test_timed_truncated_prebuffer),
        TEST(test_timed_slow_decode_completes), TEST(test_timed_stall_catches_up),
        TEST(test_timed_decode_error_remains_fatal),
        TEST(test_path_limit), TEST(test_value_reader_seek), TEST(test_value_frame_wakes), TEST(test_value_decoder_latest), TEST(test_value_decoder_error),
        TEST(test_ui_event_pause_loading), TEST(test_ui_event_pause_pending), TEST(test_ui_event_pause_late_publish),
        TEST(test_ui_event_pause_decode), TEST(test_ui_event_pause_publish), TEST(test_ui_event_pause_stop),
        TEST(test_ui_event_fullscreen_control), TEST(test_fullscreen_invalidation_guard), TEST(test_ui_event_invalid_control),
        TEST(test_runtime_tpms_schedule), TEST(test_runtime_timer_deadlines), TEST(test_runtime_before_registration),
        TEST(test_runtime_publish_gap), TEST(test_runtime_publish_blocked), TEST(test_runtime_coalescing),
        TEST(test_runtime_resume_pending), TEST(test_runtime_late_paused_publish), TEST(test_runtime_paused_idle),
        TEST(test_runtime_stop_late_notification), TEST(test_runtime_replaced_session), TEST(test_runtime_fullscreen_guard)
#undef TEST
    };
    for (unsigned i = 0; i < COUNT_OF(tests); i++) {
        fflush(NULL); pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) { tests[i].fn(); exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "FAIL %s (status=%d)\n", tests[i].name, status); return 1;
        }
        printf("PASS %s\n", tests[i].name);
    }
    printf("%zu real video-player scenarios passed\n", COUNT_OF(tests)); return 0;
}
