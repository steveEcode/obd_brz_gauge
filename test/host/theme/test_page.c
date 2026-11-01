/* Real theme_page.c + real LVGL object state. Platform time, signal snapshots
 * and video I/O are controlled; this does not emulate LCD DMA or video decode.
 * Include the source to exercise private update/binding/appearance functions
 * without exposing a test API in firmware. */
#include <time.h>
#include "media/video_elem_order.h"
#include "th_util.h"
#include "../../../main/theme_engine/theme_page.c"

te_ctx_t *g_te;
static int64_t clock_ms;
static double samples[SIG_SLOT_COUNT];
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
uint64_t signal_store_get_delivered_mask(uint64_t mask, double *out)
{
    uint64_t valid = 0;
    for (int i = 0; i < SIG_SLOT_COUNT; i++) if ((mask >> i) & 1u) {
        out[i] = samples[i];
        if (!isnan(out[i])) valid |= UINT64_C(1) << i;
    }
    return valid;
}
int signal_store_slot_by_key(const char *s)
{
    if (s && !strcmp(s, "obd.rpm")) return 0;
    if (s && !strcmp(s, "obd.speed")) return 1;
    return -1;
}
#ifndef TE_PAGE_ALLOC_CUSTOM
void *te_alloc_cb(size_t n, void *ctx) { (void)ctx; return malloc(n); }
void *te_calloc_cb(size_t n, size_t z, void *ctx) { (void)ctx; return calloc(n, z); }
void te_free_cb(void *p, void *ctx) { (void)ctx; free(p); }
#endif
char *te_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
void te_lock(void) {}
void te_unlock(void) {}
bool te_trylock(void) { return true; }
static bool te_read_fail;
esp_err_t te_read(uint32_t offset, void *dst, uint32_t size)
{
    return te_read_from(g_te, offset, dst, size);
}
esp_err_t te_read_from(te_ctx_t *c, uint32_t offset, void *dst, uint32_t size)
{
    if (te_read_fail || !c || !c->map || (uint64_t)offset + size > c->file_size)
        return ESP_FAIL;
    memcpy(dst, c->map + offset, size);
    return ESP_OK;
}
int te_image_find(const char *s)
{
    if (!s) return -1;
    return !strcmp(s, "base") ? 0 : !strcmp(s, "hot") ? 1 : !strcmp(s, "empty") ? 2 : -1;
}
int te_page_find(const char *s)
{
    for (uint8_t i = 0; g_te && s && i < g_te->n_pages; i++)
        if (!strcmp(g_te->pages[i].id, s)) return i;
    return -1;
}
static lv_font_t custom_font;
lv_font_t *te_font_get(const char *s)
{ return s && !strcmp(s, "custom_font") ? &custom_font : NULL; }
tl_defaults_t te_layout_defaults(void) { return (tl_defaults_t){0}; }
void te_release_locked(void) { tic_clear(&g_te->cache); }
void storage_theme_key(const char *s, char *out, size_t n) { snprintf(out, n, "%s", s); }

typedef struct { const char *key; bool paused; unsigned switches, pauses; lv_opa_t opacity; double position; unsigned positions; } video_state_t;
static void video_delete(lv_event_t *e) { free(lv_obj_get_user_data(lv_event_get_target(e))); }
lv_obj_t *video_elem_create(lv_obj_t *parent, const char *theme, const char *key,
                            lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    (void)theme;
    lv_obj_t *obj = lv_obj_create(parent);
    video_state_t *v = calloc(1, sizeof(*v));
    v->key = key;
    lv_obj_set_user_data(obj, v);
    lv_obj_add_event_cb(obj, video_delete, LV_EVENT_DELETE, NULL);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    return obj;
}
lv_obj_t *video_elem_create_value(lv_obj_t *parent, const char *theme, const char *key,
                                  lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{ return video_elem_create(parent, theme, key, x, y, w, h); }
void video_elem_set_position(lv_obj_t *obj, double p)
{ video_state_t *v = lv_obj_get_user_data(obj); v->position = p; v->positions++; }
void video_elem_set_paused(lv_obj_t *obj, bool paused)
{
    video_state_t *v = lv_obj_get_user_data(obj);
    v->paused = paused; v->pauses++;
}
void video_elem_set_opa(lv_obj_t *obj, lv_opa_t opacity)
{
    video_state_t *v = lv_obj_get_user_data(obj);
    v->opacity = opacity;
    lv_obj_set_style_img_opa(obj, opacity, LV_PART_MAIN);
}
bool video_elem_set_key(lv_obj_t *obj, const char *key)
{
    video_state_t *v = lv_obj_get_user_data(obj);
    v->key = key; v->switches++;
    return true;
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *data)
{ (void)area; (void)data; lv_disp_flush_ready(drv); }
static int resolve(const char *key, void *ctx)
{ (void)ctx; return signal_store_slot_by_key(key); }

#define ITEMS 7
static const char *const ids[ITEMS] = {"arc", "bar", "value", "text", "pointer", "image", "vid_base"};
static const uint8_t kinds[ITEMS] = {TR_KIND_ARC, TR_KIND_BAR, TR_KIND_LABEL, TR_KIND_LABEL,
                                    TR_KIND_POINTER, TR_KIND_IMAGE, TR_KIND_IMAGE};
typedef struct {
    te_ctx_t ctx;
    te_live_page_t page;
    tl_layout_t layout;
    tl_elem_t elements[ITEMS];
    te_item_t items[ITEMS];
    uint16_t targets[ITEMS];
    te_var_t variants[ITEMS][3];
    tic_entry_t entries[3];
    uint16_t pixels[2][4];
} fixture_t;

static const char rules[] =
"{\"rules\":["
"{\"id\":\"hot\",\"debounceMs\":32,\"when\":{\"source\":\"obd.rpm\",\"op\":\">\",\"value\":50,\"hysteresis\":5},\"apply\":["
"{\"target\":\"arc\",\"set\":{\"color\":\"#ff0000\"}},"
"{\"target\":\"bar\",\"set\":{\"color\":\"#ff0000\"}},"
"{\"target\":\"value\",\"set\":{\"text\":\"ignored\",\"color\":\"#ff0000\"}},"
"{\"target\":\"text\",\"set\":{\"text\":\"HOT\",\"color\":\"#ff0000\"}},"
"{\"target\":\"pointer\",\"set\":{\"asset\":\"hot\",\"color\":\"#ff0000\"}},"
"{\"target\":\"image\",\"set\":{\"asset\":\"hot\",\"color\":\"#ff0000\",\"opacity\":100},"
"\"animation\":{\"mode\":\"fade\",\"durationMs\":1000}},"
"{\"target\":\"vid_base\",\"set\":{\"asset\":\"hot\",\"visible\":false}}]},"
"{\"id\":\"blink\",\"priority\":2,\"trigger\":{\"type\":\"timer\",\"intervalMs\":500},\"apply\":["
"{\"target\":\"text\",\"set\":{\"visible\":false}}]},"
"{\"id\":\"change\",\"priority\":3,\"trigger\":{\"type\":\"change\",\"source\":\"obd.rpm\"},\"apply\":["
"{\"target\":\"bar\",\"set\":{\"color\":\"#ffffff\"}}]},"
"{\"id\":\"empty\",\"priority\":4,\"when\":{\"source\":\"obd.speed\",\"op\":\"<\",\"value\":20},\"apply\":["
"{\"target\":\"image\",\"set\":{\"asset\":\"empty\"}}]}]}";

static void init_fixture(fixture_t *f, bool suppressed)
{
    memset(f, 0, sizeof(*f));
    g_te = &f->ctx;
    f->ctx.suppressed = suppressed;
    f->layout.bg_color = 0x123456;
    f->layout.n = ITEMS; f->layout.el = f->elements;
    te_live_page_t *p = &f->page;
    p->layout = &f->layout; p->items = f->items; p->n_items = ITEMS;
    p->tgt_item = f->targets; p->used_mask = 3; p->was_suppressed = suppressed;
    p->scr = lv_obj_create(NULL);
    tr_element_t elements[ITEMS];
    for (unsigned i = 0; i < 3; i++) {
        tic_entry_t *e = &f->entries[i];
        e->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        e->dsc.header.w = e->dsc.header.h = 2;
        e->dsc.data_size = sizeof(f->pixels[0]);
        e->dsc.data = (const uint8_t *)f->pixels[i % 2];
        e->empty = i == 2;
    }
    for (unsigned i = 0; i < ITEMS; i++) {
        tl_elem_t *el = &f->elements[i];
        te_item_t *it = &f->items[i];
        elements[i] = (tr_element_t){ids[i], kinds[i]};
        el->id = ids[i]; el->kind = kinds[i]; el->width = 100; el->height = 12;
        el->start_angle = 0; el->end_angle = 270; el->angle_min = 0; el->angle_max = 270;
        el->color = 0xeeeeee; el->bg_color = 0x222222; el->line_width = 2;
        el->bg_opacity = 100;
        el->opacity = i == 5 ? 40 : i == 6 ? 35 : 100;
        el->text = "BASE"; el->asset = "base"; el->format = "%.0f"; el->divisor = 1;
        it->el = el; it->kind = kinds[i]; it->lo = 0; it->hi = 100;
        it->slot = i < 3 || i == 4 ? 0 : -1; it->bound = it->slot >= 0;
        it->last_px = INT32_MIN;
        it->base = (te_look_t){.c1=el->color, .c2=el->bg_color, .text=el->text, .asset=el->asset,
                               .opacity=(uint8_t)el->opacity};
        it->applied = it->base;
        if (i >= 4) {
            it->vars = f->variants[i]; it->n_vars = 3;
            for (unsigned k = 0; k < 3; k++) {
                it->vars[k].k.name = k == 0 ? "base" : k == 1 ? "hot" : "empty";
                it->vars[k].k.asset = (int16_t)k;
                it->vars[k].e = &f->entries[k];
                it->vars[k].vkey = (char *)it->vars[k].k.name;
            }
        }
        switch (it->kind) {
        case TR_KIND_ARC: te_create_arc(p->scr, it); break;
        case TR_KIND_BAR: te_create_bar(p->scr, it); break;
        case TR_KIND_LABEL: te_create_label(p->scr, it); break;
        default: te_create_image(p->scr, it); break;
        }
        if (i == 6) { it->video = 1; te_create_video(p->scr, it, "test"); }
    }
    TH_CHECK(lv_obj_get_style_img_opa(f->items[5].obj, LV_PART_MAIN) == te_opa(40));
    TH_CHECK(((video_state_t *)lv_obj_get_user_data(f->items[6].vid))->opacity == te_opa(35));
    tr_compile_cfg_t cfg = {.resolve_source=resolve, .elements=elements, .n_elements=ITEMS};
    TH_CHECK(tr_compile_json(rules, strlen(rules), &cfg, &p->prog, NULL) == TR_OK);
    if (!p->prog) abort();
    TH_CHECK(p->prog->n_rules == 4 && p->prog->n_targets == ITEMS);
    TH_CHECK(p->prog->rules[1].trigger == TR_TRIG_TIMER);
    TH_CHECK(p->prog->rules[2].trigger == TR_TRIG_CHANGE);
    for (unsigned t = 0; t < p->prog->n_targets; t++) p->tgt_item[t] = p->prog->targets[t].elem;
    for (unsigned i = 0; i < SIG_SLOT_COUNT; i++) p->prev[i] = NAN;
}
static void destroy_fixture(fixture_t *f)
{
    lv_obj_del(f->page.scr);
    tr_program_free(f->page.prog);
}
static bool color_same(lv_color_t a, lv_color_t b) { return a.full == b.full; }
static void compare(fixture_t *a, fixture_t *b)
{
    TH_CHECK(a->page.prog->resolved_mask == b->page.prog->resolved_mask);
    TH_CHECK(!memcmp(&a->page.prog->state, &b->page.prog->state, sizeof(tr_state_t)));
    TH_CHECK(color_same(lv_obj_get_style_bg_color(a->page.scr, 0), lv_obj_get_style_bg_color(b->page.scr, 0)));
    lv_obj_update_layout(a->page.scr); lv_obj_update_layout(b->page.scr);
    for (unsigned i = 0; i < ITEMS; i++) {
        te_item_t *x = &a->items[i], *y = &b->items[i];
        TH_CHECK(x->applied.c1 == y->applied.c1 && x->applied.c2 == y->applied.c2);
        TH_CHECK(x->applied.hidden == y->applied.hidden && x->applied.has_tint == y->applied.has_tint);
        TH_CHECK(x->applied.opacity == y->applied.opacity);
        TH_CHECK(x->applied.tint == y->applied.tint);
        TH_CHECK(te_streq(x->applied.asset, y->applied.asset) && te_streq(x->applied.text, y->applied.text));
        TH_CHECK(x->last_px == y->last_px && x->cur == y->cur);
        TH_CHECK(lv_obj_has_flag(x->obj, LV_OBJ_FLAG_HIDDEN) == lv_obj_has_flag(y->obj, LV_OBJ_FLAG_HIDDEN));
        switch (x->kind) {
        case TR_KIND_ARC:
            TH_CHECK(lv_arc_get_value(x->obj) == lv_arc_get_value(y->obj));
            TH_CHECK(color_same(lv_obj_get_style_arc_color(x->obj, LV_PART_INDICATOR), lv_obj_get_style_arc_color(y->obj, LV_PART_INDICATOR)));
            TH_CHECK(color_same(lv_obj_get_style_arc_color(x->obj, LV_PART_MAIN), lv_obj_get_style_arc_color(y->obj, LV_PART_MAIN)));
            break;
        case TR_KIND_BAR:
            TH_CHECK(lv_obj_has_flag(x->fg, LV_OBJ_FLAG_HIDDEN) == lv_obj_has_flag(y->fg, LV_OBJ_FLAG_HIDDEN));
            TH_CHECK(lv_obj_get_width(x->fg) == lv_obj_get_width(y->fg));
            TH_CHECK(color_same(lv_obj_get_style_bg_color(x->fg, 0), lv_obj_get_style_bg_color(y->fg, 0)));
            TH_CHECK(color_same(lv_obj_get_style_bg_color(x->obj, 0), lv_obj_get_style_bg_color(y->obj, 0)));
            break;
        case TR_KIND_LABEL:
            TH_CHECK(te_streq(lv_label_get_text(x->obj), lv_label_get_text(y->obj)));
            TH_CHECK(color_same(lv_obj_get_style_text_color(x->obj, 0), lv_obj_get_style_text_color(y->obj, 0)));
            break;
        default:
            TH_CHECK(lv_img_get_angle(x->obj) == lv_img_get_angle(y->obj));
            TH_CHECK(color_same(lv_obj_get_style_img_recolor(x->obj, 0), lv_obj_get_style_img_recolor(y->obj, 0)));
            TH_CHECK(lv_obj_get_style_img_recolor_opa(x->obj, 0) == lv_obj_get_style_img_recolor_opa(y->obj, 0));
            TH_CHECK(lv_obj_get_style_img_opa(x->obj, 0) == lv_obj_get_style_img_opa(y->obj, 0));
            if (x->vid) {
                video_state_t *vx = lv_obj_get_user_data(x->vid), *vy = lv_obj_get_user_data(y->vid);
                TH_CHECK(vx->paused == vy->paused && te_streq(vx->key, vy->key));
                TH_CHECK(vx->opacity == vy->opacity);
                TH_CHECK(vx->switches == vy->switches && vx->pauses == vy->pauses);
                TH_CHECK(lv_obj_has_flag(x->vbox, LV_OBJ_FLAG_HIDDEN) == lv_obj_has_flag(y->vbox, LV_OBJ_FLAG_HIDDEN));
            }
        }
    }
}
static void update_pair(fixture_t *a, fixture_t *b)
{
    g_te = &a->ctx; te_page_update(&a->ctx, &a->page);
    g_te = &b->ctx; te_page_update(&b->ctx, &b->page);
    // Reference: always assemble/apply current looks, as the former page loop
    // did. Evaluator and binding code are identical in both pages.
    te_apply_targets(&b->page, &b->page.prog->result, b->ctx.suppressed);
    compare(a, b);
}
static void test_transitions(void)
{
    fixture_t a, b;
    clock_ms = 0; samples[0] = 0; samples[1] = 30;
    init_fixture(&a, false); init_fixture(&b, false);
    update_pair(&a, &b);
    samples[0] = 80; clock_ms = 16; update_pair(&a, &b);
    TH_CHECK(!strcmp(lv_label_get_text(a.items[3].obj), "BASE"));
    clock_ms = 48; update_pair(&a, &b);
    TH_CHECK(!strcmp(lv_label_get_text(a.items[3].obj), "HOT"));
    TH_CHECK(!strcmp(lv_label_get_text(a.items[2].obj), "80"));
    TH_CHECK(a.items[4].cur == 1);
    TH_CHECK(a.items[4].applied.has_tint && a.items[4].rt_on);
    TH_CHECK(lv_obj_get_style_img_recolor_opa(a.items[4].obj, 0) == TR_RECOLOR_OPA);
    TH_CHECK(lv_obj_get_style_img_opa(a.items[5].obj, 0) == te_opa(40));
    clock_ms = 64; update_pair(&a, &b);
    TH_CHECK(lv_obj_get_style_img_opa(a.items[5].obj, 0) == te_opa(41));
    TH_CHECK(((video_state_t *)lv_obj_get_user_data(a.items[6].vid))->paused);
    TH_CHECK(lv_obj_has_flag(a.items[6].obj, LV_OBJ_FLAG_HIDDEN));
    TH_CHECK(lv_obj_has_flag(a.items[6].vbox, LV_OBJ_FLAG_HIDDEN));
    a.ctx.suppressed = b.ctx.suppressed = true;
    clock_ms = 64; update_pair(&a, &b);
    TH_CHECK(!strcmp(lv_label_get_text(a.items[3].obj), "BASE"));
    TH_CHECK(!((video_state_t *)lv_obj_get_user_data(a.items[6].vid))->paused);
    // Binding changes while rules stay active and suppressed.
    samples[0] = 90; clock_ms = 80; update_pair(&a, &b);
    TH_CHECK(!strcmp(lv_label_get_text(a.items[2].obj), "90"));
    lv_obj_set_style_bg_color(a.page.scr, lv_color_hex(0xff0000), 0);
    lv_obj_set_style_bg_color(b.page.scr, lv_color_hex(0xff0000), 0);
    a.ctx.suppressed = b.ctx.suppressed = false;
    clock_ms = 96; update_pair(&a, &b);
    TH_CHECK(color_same(lv_obj_get_style_bg_color(a.page.scr, 0), lv_color_hex(a.layout.bg_color)));
    samples[1] = 0; clock_ms = 112; update_pair(&a, &b);
    TH_CHECK(lv_obj_has_flag(a.items[5].obj, LV_OBJ_FLAG_HIDDEN));
    samples[1] = 30; clock_ms = 128; update_pair(&a, &b);
    TH_CHECK(!lv_obj_has_flag(a.items[5].obj, LV_OBJ_FLAG_HIDDEN));
    samples[0] = 0; clock_ms = 144; update_pair(&a, &b);
    TH_CHECK(!lv_obj_has_flag(a.items[6].obj, LV_OBJ_FLAG_HIDDEN));
    TH_CHECK(!lv_obj_has_flag(a.items[6].vbox, LV_OBJ_FLAG_HIDDEN));
    TH_CHECK(a.items[4].cur == 0);
    TH_CHECK(!strcmp(lv_label_get_text(a.items[3].obj), "BASE"));
    clock_ms = 644; update_pair(&a, &b);
    TH_CHECK(a.items[5].applied.opacity < 100 && a.items[5].applied.opacity > 40);
    clock_ms = 1144; update_pair(&a, &b);
    TH_CHECK(a.items[5].applied.opacity == 40);
    // Change-window expiry and timer phase changes need evaluation even when
    // there are no new signal samples.
    clock_ms = 425; update_pair(&a, &b);
    TH_CHECK(a.items[1].applied.c1 == a.items[1].base.c1);
    TH_CHECK(!lv_obj_has_flag(a.items[3].obj, LV_OBJ_FLAG_HIDDEN));
    clock_ms = 500; update_pair(&a, &b);
    TH_CHECK(lv_obj_has_flag(a.items[3].obj, LV_OBJ_FLAG_HIDDEN));
    // Initial render while suppressed and reset of a live program.
    tr_reset(a.page.prog); tr_reset(b.page.prog);
    a.ctx.suppressed = b.ctx.suppressed = true;
    clock_ms = 516; update_pair(&a, &b);
    destroy_fixture(&a); destroy_fixture(&b);
}

// Bound text must use the same simulated samples as rules, with either a
// custom font or the built-in font, and return to unchanged live data afterward.
static void test_sweep_label_values(void)
{
    for (int slot = SIG_OBD_FIRST; slot < SIG_OBD_FIRST + SIG_OBD_COUNT; slot++) {
        fixture_t f;
        samples[0] = samples[1] = NAN;
        samples[slot] = NAN;
        init_fixture(&f, true);
        g_te = &f.ctx;
        te_item_t *it = &f.items[2];
        it->slot = slot;
        f.page.used_mask |= UINT64_C(1) << slot;
        custom_font = *LV_FONT_DEFAULT;
        f.elements[2].font_asset = "custom_font";
        lv_obj_del(it->obj);
        te_create_label(f.page.scr, it);
        TH_CHECK(lv_obj_get_style_text_font(it->obj, 0) == &custom_font);
        te_page_update(&f.ctx, &f.page);
        for (unsigned step = 0; step <= 4; step++) {
            double expected[SIG_SLOT_COUNT] = {0};
            uint64_t changed = 0;
            theme_set_sweep((float)step / 4, (uint16_t)(step * 2000));
            te_sweep_rule_inputs((double)step / 4, (uint16_t)(step * 2000), expected, &changed);
            te_page_update(&f.ctx, &f.page);
            char text[TE_LABEL_MAX];
            theme_ov_label_text(it->ov, "%.0f", 1, expected[slot], text, sizeof(text));
            TH_CHECK(!strcmp(lv_label_get_text(it->obj), text));
            TH_CHECK(isnan(samples[slot]));
            TH_CHECK(!strcmp(lv_label_get_text(f.items[3].obj), "BASE"));
        }
        samples[slot] = 42;
        theme_set_sweep(-1, 0);
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(!strcmp(lv_label_get_text(it->obj), "42"));
        // Repeat with a valid but unchanged live sample to catch stale caches.
        theme_set_sweep(0.5f, 4000);
        te_page_update(&f.ctx, &f.page);
        theme_set_sweep(-1, 0);
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(!strcmp(lv_label_get_text(it->obj), "42"));
        destroy_fixture(&f);
        g_te = NULL;
    }
}

static void test_video_layer_order(void)
{
    tl_elem_t video = {.kind = TR_KIND_IMAGE, .video = 1};
    tl_elem_t decoration = {.kind = TR_KIND_IMAGE};
    TH_CHECK(te_class(&video, false) == TE_CLS_TARGET);
    TH_CHECK(te_class(&decoration, false) == TE_CLS_DECOR);
    TH_CHECK(te_class(&decoration, true) == TE_CLS_TARGET);

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *lower_wrapper = lv_obj_create(screen);
    lv_obj_t *lower_video = lv_obj_create(lower_wrapper);
    lv_obj_t *upper_wrapper = lv_obj_create(screen);
    lv_obj_t *upper_video = lv_obj_create(upper_wrapper);
    TH_CHECK(lv_obj_get_index(lower_video) == lv_obj_get_index(upper_video));
    uint32_t lower_order = video_elem_layer_order(lower_video);
    uint32_t upper_order = video_elem_layer_order(upper_video);
    TH_CHECK(lower_order < upper_order);
    video_elem_layer_choice_t choice = {0};
    TH_CHECK(video_elem_layer_consider(&choice, true, lower_order));
    TH_CHECK(!video_elem_layer_consider(&choice, false, upper_order));
    TH_CHECK(choice.order == lower_order);
    TH_CHECK(video_elem_layer_consider(&choice, true, upper_order));
    TH_CHECK(choice.order == upper_order);
    lv_obj_del(screen);
}
static uint32_t rng = UINT32_C(0x51fe9123);
static uint32_t next_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void test_sweep_rule_inputs(void)
{
    double values[SIG_SLOT_COUNT] = {0};
    uint64_t changed = 0;
    te_sweep_rule_inputs(1.0, 8000, values, &changed);
    TH_CHECK(changed == ((UINT64_C(1) << SIG_OBD_COUNT) - 1));
    TH_CHECK(values[SIG_OBD_RPM] == 8000);
    TH_CHECK(values[SIG_OBD_SPEED] == 999);
    TH_CHECK(values[SIG_OBD_BOOST] == 200);
    TH_CHECK(values[SIG_OBD_COOLANT_TEMP] == 120);
    TH_CHECK(values[SIG_OBD_OIL_PRESSURE] == 145);
    TH_CHECK(values[SIG_OBD_GEAR] == 6);
    TH_CHECK(values[SIG_OBD_BATTERY_VOLTAGE] == 144);
    TH_CHECK(values[SIG_OBD_OIL_TEMP] == 120);
    TH_CHECK(values[SIG_OBD_AFR] == 2200);
    TH_CHECK(values[SIG_OBD_THROTTLE] == 100);
    TH_CHECK(values[SIG_OBD_INTAKE_TEMP] == 120);
    TH_CHECK(values[SIG_OBD_LOAD] == 100);
}
// A threshold during sweep must recolor only the filled portion, even when
// the rule stays active across later ticks or animates the color.
static void test_sweep_color_preserves_track(void)
{
    const char *const animation[] = {
        "", ",\"animation\":{\"mode\":\"fade\",\"durationMs\":1000}",
        ",\"animation\":{\"mode\":\"keyframes\",\"durationMs\":1000}",
        ",\"animation\":{\"mode\":\"strobe\",\"durationMs\":1000,\"frequencyHz\":4}"
    };
    for (unsigned mode = 0; mode < sizeof(animation) / sizeof(animation[0]); mode++) {
        fixture_t f;
        clock_ms = 0; samples[0] = samples[1] = 0;
        init_fixture(&f, false);
        tr_program_free(f.page.prog);
        const tr_element_t elements[] = {{"arc", TR_KIND_ARC}, {"bar", TR_KIND_BAR}};
        const tr_compile_cfg_t cfg = {.resolve_source=resolve, .elements=elements, .n_elements=2};
        char json[768];
        int n = snprintf(json, sizeof(json),
            "{\"rules\":[{\"id\":\"rpm_color\",\"when\":{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":3000},"
            "\"apply\":[{\"target\":\"arc\",\"set\":{\"color\":\"#ff3158\"}%s},"
            "{\"target\":\"bar\",\"set\":{\"color\":\"#ff3158\"}%s}]}]}",
            animation[mode], animation[mode]);
        TH_CHECK(n > 0 && (size_t)n < sizeof(json));
        TH_CHECK(tr_compile_json(json, (size_t)n, &cfg, &f.page.prog, NULL) == TR_OK);
        if (!f.page.prog) abort();
        for (unsigned t = 0; t < f.page.prog->n_targets; t++) f.targets[t] = f.page.prog->targets[t].elem;
        // Below threshold, then crossing at 40%, followed by a still-active
        // tick and full scale. The actual OBD sample remains zero throughout.
        const int percent[] = {25, 40, 65, 100};
        for (unsigned tick = 0; tick < sizeof(percent) / sizeof(percent[0]); tick++) {
            clock_ms = (int64_t)tick * 500;
            theme_set_sweep((float)percent[tick] / 100.0f, (uint16_t)(percent[tick] * 80));
            te_page_update(&f.ctx, &f.page);
            TH_CHECK(lv_arc_get_value(f.items[0].obj) == percent[tick] * 100);
            lv_obj_update_layout(f.page.scr);
            TH_CHECK(lv_obj_get_width(f.items[1].fg) == percent[tick]);
            TH_CHECK(color_same(lv_obj_get_style_arc_color(f.items[0].obj, LV_PART_MAIN), lv_color_hex(0x222222)));
            TH_CHECK(color_same(lv_obj_get_style_bg_color(f.items[1].obj, 0), lv_color_hex(0x222222)));
            for (unsigned i = 0; i < 2; i++) {
                TH_CHECK(f.items[i].applied.c2 == f.items[i].base.c2);
                const tr_override_t *o = tr_result_override(f.page.prog, (uint16_t)i);
                if (!tick) TH_CHECK(o == NULL);
                else TH_CHECK(o && o->props == TR_PROP_COLOR);
            }
            if (mode == 0 && tick > 0) {
                TH_CHECK(color_same(lv_obj_get_style_arc_color(f.items[0].obj, LV_PART_INDICATOR), lv_color_hex(0xff3158)));
                TH_CHECK(color_same(lv_obj_get_style_bg_color(f.items[1].fg, 0), lv_color_hex(0xff3158)));
            }
        }
        theme_set_sweep(-1.0f, 0);
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(f.items[0].last_px == 0);
        for (unsigned i = 0; i < 2; i++) {
            TH_CHECK(f.items[i].applied.c1 == f.items[i].base.c1);
            TH_CHECK(f.items[i].applied.c2 == f.items[i].base.c2);
        }
        TH_CHECK(lv_obj_has_flag(f.items[1].fg, LV_OBJ_FLAG_HIDDEN));
        destroy_fixture(&f);
    }
}

// Replay the real 40 ms / 25-step ramp and 23-step hold/flash sequence.
// The physical OBD sample stays zero: rules must consume the sweep RPM.
static void test_sweep_video_opacity(void)
{
    for (unsigned fade = 0; fade < 2; fade++) {
        fixture_t f;
        clock_ms = 0; samples[0] = samples[1] = 0;
        init_fixture(&f, false);
        tr_program_free(f.page.prog);
        const tr_element_t element = {"vid_base", TR_KIND_IMAGE};
        const tr_compile_cfg_t cfg = {.resolve_source=resolve, .elements=&element, .n_elements=1};
        char json[512];
        int n = snprintf(json, sizeof(json),
            "{\"rules\":[{\"id\":\"video_sweep\",\"enabled\":true,"
            "\"when\":{\"source\":\"obd.rpm\",\"op\":\">=\",\"value\":3500},"
            "\"apply\":[{\"target\":\"vid_base\",\"set\":{\"opacity\":0}%s}]}]}",
            fade ? ",\"animation\":{\"mode\":\"fade\",\"durationMs\":1000}" : "");
        TH_CHECK(n > 0 && (size_t)n < sizeof(json));
        TH_CHECK(tr_compile_json(json, (size_t)n, &cfg, &f.page.prog, NULL) == TR_OK);
        if (!f.page.prog) abort();
        f.targets[0] = 6;
        // Exercise a video without a poster as well as one with a poster.
        if (fade) { lv_obj_del(f.items[6].obj); f.items[6].obj = NULL; }
        video_state_t *v = lv_obj_get_user_data(f.items[6].vid);
        for (unsigned tick = 0; tick < 48; tick++) {
            clock_ms = tick * 40;
            float ratio = tick < 25 ? (float)tick / 25.0f : 1.0f;
            theme_set_sweep(ratio, (uint16_t)(8000.0f * ratio));
            te_page_update(&f.ctx, &f.page);
            if (tick < 11) TH_CHECK(v->opacity == te_opa(35));
            if (tick >= 36) TH_CHECK(v->opacity == LV_OPA_TRANSP);
            TH_CHECK(samples[0] == 0);
        }
        theme_set_sweep(-1.0f, 0);
        te_page_update(&f.ctx, &f.page);
        clock_ms += 1040;
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->opacity == te_opa(35));
        destroy_fixture(&f);
    }
}

static void test_value_video_binding(void)
{
    for (unsigned indexed = 0; indexed < 2; indexed++) {
        fixture_t f;
        clock_ms = 0; samples[0] = samples[1] = 0;
        init_fixture(&f, false);
        te_item_t *it = &f.items[6];
        tl_elem_t *el = &f.elements[6];
        el->video_value = 1; el->range_min = 0; el->range_max = 6000; el->video_step = 100;
        it->slot = 0; it->bound = 1;
        video_state_t *v = lv_obj_get_user_data(it->vid);
        uint16_t binding_item = 6;
        f.page.bindings_seeded = indexed;
        f.page.binding_items = indexed ? &binding_item : NULL;
        f.page.n_binding_items = indexed ? 1 : 0;
        /* A usable SD video must bind even when the poster image is missing. */
        if (indexed) { lv_obj_del(it->obj); it->obj = NULL; }
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0 && v->positions == 1);
        samples[0] = 3049; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0.5);
        unsigned before = v->positions;
        samples[0] = NAN; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0.5 && v->positions == before);
        samples[0] = 7000; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 1);
        samples[0] = 1500; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0.25);
        samples[0] = -100; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0);
        f.ctx.sweep_on = true; f.ctx.sweep = 0.75f;
        te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0.75);
        f.ctx.sweep = 1; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 1);
        f.ctx.sweep = 0.25f; te_page_update(&f.ctx, &f.page);
        TH_CHECK(v->position == 0.25);
        destroy_fixture(&f);
    }
}

static void test_replay(void)
{
    fixture_t a, b;
    clock_ms = 0; samples[0] = samples[1] = 0;
    init_fixture(&a, true); init_fixture(&b, true);
    for (unsigned tick = 0; tick < 10000; tick++) {
        clock_ms += 1 + next_random() % 32;
        if (!(tick % 11)) samples[0] = next_random() % 9 == 0 ? NAN : (double)(next_random() % 110);
        if (!(tick % 19)) samples[1] = next_random() % 7 == 0 ? NAN : (double)(next_random() % 60);
        if (!(tick % 53)) a.ctx.suppressed = b.ctx.suppressed = !a.ctx.suppressed;
        if (!(tick % 71)) a.ctx.sweep_on = b.ctx.sweep_on = !a.ctx.sweep_on;
        if (a.ctx.sweep_on) a.ctx.sweep = b.ctx.sweep = (float)(next_random() % 101) / 100;
        if (!(tick % 211)) { tr_reset(a.page.prog); tr_reset(b.page.prog); }
        update_pair(&a, &b);
    }
    destroy_fixture(&a); destroy_fixture(&b);
}

// Exercise actual staged page creation with transparent pixels. The background
// phase must remain independent of global metadata, live caches and screens.
static void test_staged_boot_page(void)
{
    const char *layout = "{\"page_id\":\"main_gauge\",\"elements\":["
        "{\"type\":\"image\",\"id\":\"picture\",\"asset\":\"base\","
        "\"x\":175,\"y\":175,\"width\":2,\"height\":2}]}";
    uint8_t pixels[16] = {255,0,0,255, 0,255,0,128, 0,0,255,0, 255,255,255,255};
    size_t size = sizeof(pixels) + strlen(layout);
    uint8_t *blob = malloc(size);
    memcpy(blob, pixels, sizeof(pixels));
    memcpy(blob + sizeof(pixels), layout, strlen(layout));

    for (int scenario = 0; scenario < 6; scenario++) {
        te_ctx_t ctx = {.custom=true, .manifest_crc=123, .load_generation=7,
                        .map=blob, .file_size=(uint32_t)size, .n_images=1, .n_pages=1};
        ctx.images[0] = (te_image_t){.name="base", .offset=0, .size=16,
                                   .w=2, .h=2, .fmt=TPX_FMT_RGBA8888};
        ctx.pages[0] = (te_page_t){.id="main_gauge", .offset=16, .size=(uint32_t)strlen(layout)};
        strcpy(ctx.id, "boot_test");
        g_te = &ctx;
        lv_obj_t *visible = lv_scr_act();
        theme_page_prepared_t *job = theme_page_prepare_begin("main_gauge");
        TH_CHECK(job != NULL);
        if (!job) continue;
        TH_CHECK(!job->page->scr && ctx.n_live == 0 && ctx.cache.count == 0);
        TH_CHECK(ctx.n_preparing == 1);
        if (scenario != 1) { // cancellation before worker starts
            // Even if metadata is unloaded, run uses its owned snapshot only.
            g_te = NULL;
            te_read_fail = scenario == 3;
            theme_page_prepare_run(job);
            te_read_fail = false;
            TH_CHECK(!job->page->scr && lv_scr_act() == visible);
            TH_CHECK(ctx.cache.count == 0 && ctx.n_live == 0);
            if (scenario != 3) {
                TH_CHECK(job->images_ok && job->source.cache.count == 1);
                TH_CHECK(job->page->items[0].vars[0].e->dsc.header.cf == LV_IMG_CF_RGB565A8);
            }
            g_te = &ctx;
        }
        if (scenario == 2) ctx.load_generation++; // another theme load
        if (scenario == 4) ctx.release_pending = true; // OTA/resource release
        if (scenario == 5) ctx.custom = false; // invalidated theme
        lv_obj_t *screen = theme_page_prepare_finish(job);
        TH_CHECK(ctx.n_preparing == 0);
        if (scenario == 0) {
            TH_CHECK(screen && ctx.n_live == 1 && ctx.cache.count == 1);
            TH_CHECK(lv_scr_act() == visible); // publish does not expose a partial screen
            TH_CHECK(ctx.cache.pinned == 12 && !ctx.cache.unpublished);
            lv_obj_t *image = lv_obj_get_child(screen, 0);
            const lv_img_dsc_t *dsc = lv_img_get_src(image);
            TH_CHECK(dsc == &ctx.cache.head->dsc);
            TH_CHECK(dsc->data[8] == 255 && dsc->data[9] == 128 && dsc->data[10] == 0);
            lv_obj_del(screen);
            TH_CHECK(ctx.n_live == 0 && ctx.cache.pinned == 0);
        } else {
            TH_CHECK(!screen && ctx.cache.count == 0 && ctx.n_live == 0);
        }
        g_te = &ctx;
        tic_clear(&ctx.cache);
    }
    g_te = NULL;
    free(blob);
}

static void test_track_opacity(void)
{
    const double percentages[] = {0, 35, 100};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *scr = lv_obj_create(NULL);
        tl_elem_t el = {.width=100, .height=100, .line_width=10, .end_angle=270,
                        .color=0xffffff, .bg_color=0x222222, .bg_opacity=percentages[i]};
        te_item_t arc = {.el=&el}, bar = {.el=&el};
        te_create_arc(scr, &arc); te_create_bar(scr, &bar);
        lv_opa_t want = (lv_opa_t)tl_round(percentages[i] * 255.0 / 100.0);
        TH_CHECK(lv_obj_get_style_arc_opa(arc.obj, LV_PART_MAIN) == want);
        TH_CHECK(lv_obj_get_style_arc_opa(arc.obj, LV_PART_INDICATOR) == LV_OPA_COVER);
        TH_CHECK(lv_obj_get_style_bg_opa(bar.obj, 0) == want);
        TH_CHECK(lv_obj_get_style_bg_opa(bar.fg, 0) == LV_OPA_COVER);
        lv_obj_del(scr);
    }
}

int main(void)
{
    lv_init();
    static lv_color_t buf[360 * 8];
    static lv_disp_draw_buf_t draw;
    static lv_disp_drv_t drv;
    lv_disp_draw_buf_init(&draw, buf, NULL, 360 * 8);
    lv_disp_drv_init(&drv); drv.hor_res = drv.ver_res = 360;
    drv.draw_buf = &draw; drv.flush_cb = flush;
    lv_disp_t *disp = lv_disp_drv_register(&drv);
    test_track_opacity();
    test_sweep_rule_inputs(); test_sweep_color_preserves_track(); test_transitions();
    test_sweep_label_values(); test_video_layer_order(); test_sweep_video_opacity(); test_replay();
    test_staged_boot_page(); test_value_video_binding();
    lv_disp_remove(disp);
    return th_report("theme_page");
}
