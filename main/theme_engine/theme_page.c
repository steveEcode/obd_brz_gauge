// ============================================================
//  theme_page.c — theme page build, data bindings and rules
//  (contracts/theme-editor-contract.md A4-A6, C3-C5; REFACTOR_PLAN.md
//  P4 / 5.7).
//
//  Build: parse the layout, pair it with the sidecar page (overlay +
//  rules), convert the images the page needs into the PSRAM cache
//  (pointers and rule targets first, then decorative images bottom to
//  top within the per-page budget), then create the LVGL objects in z
//  order. Per-page state (items, compiled rules, change windows) lives
//  in one PSRAM block freed on the screen's LV_EVENT_DELETE.
//
//  Tick: one signal_store snapshot → bindings (only changed outputs
//  touch LVGL) → rules on the same snapshot → diff against the applied
//  look. Only the isolated image-preparation phase may run on a worker;
//  widget creation, publication, ticks and deletion run under the LVGL mutex.
// ============================================================

#include "app_obd_dsp/perf_stats.h"
#include "theme_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "rules/tr_parse.h"
#include "rules/tr_eval.h"
#include "app_obd_dsp/signal_store.h"
#include "app_obd_dsp/signal_delivery.h"
#include "app_obd_dsp/storage_util.h"
#include "media/video_elem.h"

_Static_assert(SIG_SLOT_COUNT <= 64, "change mask / used mask are 64-bit");

#define TE_VAR_MAX     16          // image variants per element (asset x tint)
#define TE_ITEM_NONE   0xFFFFu
#define TE_LABEL_MAX   128         // formatted label text

enum { TE_CLS_POINTER = 0, TE_CLS_TARGET, TE_CLS_DECOR, TE_CLS_NONE };
enum { TE_POL_REQUIRED = 0, TE_POL_BUDGET, TE_POL_DECOR };
enum { TE_ACQ_OK = 0, TE_ACQ_EMPTY, TE_ACQ_BUDGET, TE_ACQ_NOMEM, TE_ACQ_ERR };

typedef struct {
    const char *name;         // asset name (layout or program string)
    int16_t  asset;           // image index, -1 = video key only
    uint8_t  has_tint;
    uint32_t tint;            // 0xRRGGBB, baked
} te_vkey_t;

typedef struct {
    te_vkey_t    k;
    tic_entry_t *e;           // pinned, NULL = not available
    char        *vkey;        // video key (vid_/video_ items), owned
} te_var_t;

typedef struct {
    uint32_t c1, c2;          // arc indicator / bar fg / label text; arc track / bar bg
    uint8_t  has_tint;
    uint8_t  hidden;
    uint8_t  opacity;
    uint32_t tint;
    uint8_t  tint_opa;
    const char *text;         // static labels
    const char *asset;        // image / pointer / video
} te_look_t;

typedef struct {
    const tl_elem_t *el;
    uint8_t  kind;            // tr_kind_t
    uint8_t  video;
    uint8_t  target;          // some rule action targets it
    uint8_t  bound;           // data_source present
    uint8_t  have_last;       // label/render input cache valid
    uint8_t  last_sweep_on;   // last range binding used the sweep value
    uint8_t  rt_on;           // runtime image recolor active
    uint8_t  transform;       // label: zoom / rotation style set
    int16_t  slot;            // signal slot, -1 = unknown key / unbound
    int16_t  zoom;            // label transform_zoom (256 = none)
    lv_obj_t *obj;            // arc / bar bg / label / image (poster) / pointer
    lv_obj_t *fg;             // bar foreground
    lv_obj_t *vbox;           // video container
    lv_obj_t *vid;            // video_elem object
    theme_overlay_t *ov;
    const lv_font_t *font;
    double   lo, hi;          // display range
    double   last_raw;
    double   last_sweep;      // sweep value used by the last rendered binding
    int32_t  last_px;         // arc value / bar px / pointer angle
    int32_t  piv_x, piv_y;    // pointer pivot in the R(w) x R(h) box
    uint32_t rt_tint;
    uint8_t  rt_tint_opa;
    te_var_t *vars;
    uint8_t  n_vars;
    uint8_t  cur;             // shown variant
    const char *vid_base_key; // id without the vid_/video_ prefix
    te_look_t base, applied;
} te_item_t;

struct te_live_page {
    te_live_page_t *next;
    lv_obj_t     *scr;
    int           page_idx;
    tl_layout_t  *layout;
    tr_program_t *prog;
    te_item_t    *items;
    uint16_t      n_items;
    uint16_t     *binding_items; // bound non-image item indexes
    uint16_t      n_binding_items;
    bool          bindings_seeded;
    uint16_t     *tgt_item;   // [prog->n_targets] → item, TE_ITEM_NONE
    uint64_t      used_mask;  // slots read each tick
    int64_t       build_ms;
    size_t        cost;       // converted image bytes this page pins
    bool          was_suppressed;
    bool          was_sweep;       // transition-only sweep diagnostics
    double        values[SIG_SLOT_COUNT];
    double        prev[SIG_SLOT_COUNT];      // last valid value (NaN = none)
    int64_t       chg_until[SIG_SLOT_COUNT]; // change window end (ms)
};

typedef struct {
    te_ctx_t   *c;
    const char *page;
    size_t      budget;
    size_t      cost;
} te_build_t;

// ---------------- small helpers ----------------

static inline int32_t te_min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t te_max32(int32_t a, int32_t b) { return a > b ? a : b; }

static bool te_streq(const char *a, const char *b)
{
    if (a == b) return true;
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static uint32_t te_mix_color(uint32_t from, uint32_t to, double progress)
{
    if (progress <= 0.0) return from;
    if (progress >= 1.0) return to;
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        double a = (double)((from >> shift) & 0xFFu);
        double b = (double)((to >> shift) & 0xFFu);
        uint32_t c = (uint32_t)(a + (b - a) * progress + 0.5);
        out |= c << shift;
    }
    return out;
}

static int64_t te_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// R(deg * 10) normalized to [0, 3600); NaN / inf → 0.
static int16_t te_angle10(double deg)
{
    double a = tl_round(deg * 10.0);
    if (!isfinite(a)) return 0;
    a = fmod(a, 3600.0);
    if (a < 0) a += 3600.0;
    return (int16_t)a;
}

// ((R(deg) % 360) + 360) % 360; NaN / inf → 0.
static uint16_t te_deg360(double deg)
{
    double a = tl_round(deg);
    if (!isfinite(a)) return 0;
    a = fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return (uint16_t)a;
}

static void te_set_hidden(lv_obj_t *o, bool hidden)
{
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void te_plain(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static int te_resolve(const char *key, void *ctx)
{
    (void)ctx;
    int s = signal_store_slot_by_key(key);
    return (s >= 0 && s < SIG_SLOT_COUNT) ? s : -1;
}

static int te_class(const tl_elem_t *el, bool target)
{
    if (el->kind == TR_KIND_POINTER) return TE_CLS_POINTER;
    // Keep video posters available as fallbacks while a layer is hidden,
    // unavailable, or waiting for its first decoded frame.
    if (el->kind == TR_KIND_IMAGE) return (target || el->video) ? TE_CLS_TARGET : TE_CLS_DECOR;
    return TE_CLS_NONE;
}

// Rule target index of layout element idx, -1 when no action targets it.
static int te_target_of(const tr_program_t *prog, const uint16_t *rel_el, uint16_t idx)
{
    if (!prog || !rel_el) return -1;
    for (uint16_t t = 0; t < prog->n_targets; t++) {
        uint16_t e = prog->targets[t].elem;
        if (e != TR_ELEM_NONE && rel_el[e] == idx) return t;
    }
    return -1;
}

// ---------------- page preparation ----------------

void te_prep_free(te_prep_t *p)
{
    if (!p) return;
    tl_alloc_t a = TE_ALLOC_INIT;
    if (p->layout) tl_free(p->layout, &a);
    if (p->prog) tr_program_free(p->prog);
    te_free(p->scratch);
    te_free(p->rel);
    te_free(p->rel_el);
    te_free(p->is_target);
    memset(p, 0, sizeof(*p));
}

bool te_layout_has_video(const tl_layout_t *l)
{
    for (uint16_t i = 0; l && i < l->n; i++) {
        if (l->el[i].video) return true;
    }
    return false;
}

// Bin elements the sidecar knows under the same id and kind (first wins).
static void te_prep_rel(te_prep_t *o)
{
    const tl_layout_t *l = o->layout;
    cJSON *em = cJSON_ParseWithLength(o->sc->elements, o->sc->elements_len);
    for (uint16_t i = 0; cJSON_IsObject(em) && i < l->n; i++) {
        const tl_elem_t *el = &l->el[i];
        if (!el->id[0]) continue;
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(em, el->id);
        if (!cJSON_IsString(k) || tr_kind_from_str(k->valuestring) != (tr_kind_t)el->kind) continue;
        bool dup = false;
        for (uint16_t j = 0; j < o->n_rel && !dup; j++) dup = strcmp(o->rel[j].id, el->id) == 0;
        if (dup) continue;
        o->rel[o->n_rel].id = el->id;
        o->rel[o->n_rel].kind = (tr_kind_t)el->kind;
        o->rel_el[o->n_rel++] = i;
    }
    cJSON_Delete(em);
}

static void te_prep_rules(te_prep_t *o, const char *page_id)
{
    if (!o->sc->rules || !o->sc->rules_len) return;
    cJSON *arr = cJSON_ParseWithLength(o->sc->rules, o->sc->rules_len);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return;
    }
    cJSON *page = cJSON_CreateObject();
    if (!page) {
        cJSON_Delete(arr);
        return;
    }
    cJSON_AddItemToObject(page, "rules", arr);
    tr_compile_cfg_t cfg = {TE_TR_ALLOC, te_resolve, NULL, o->rel, o->n_rel};
    tr_err_t e = tr_compile(page, &cfg, &o->prog, &o->diag);
    cJSON_Delete(page);
    if (e != TR_OK) {
        ESP_LOGW(TE_TAG, "page %s: rules not compiled (%d)", page_id, (int)e);
        o->prog = NULL;
        return;
    }
    for (uint16_t t = 0; t < o->prog->n_targets; t++) {
        uint16_t el = o->prog->targets[t].elem;
        if (el != TR_ELEM_NONE && el < o->n_rel) o->is_target[o->rel_el[el]] = 1;
    }
}

esp_err_t te_prep_page(int page_idx, bool with_sidecar, te_prep_t *o)
{
    memset(o, 0, sizeof(*o));
    te_ctx_t *c = g_te;
    if (!c || !c->custom || page_idx < 0 || page_idx >= c->n_pages) return ESP_ERR_INVALID_ARG;
    const te_page_t *pg = &c->pages[page_idx];

    char *scratch = te_malloc(TL_ITEM_MAX);
    if (!scratch) return ESP_ERR_NO_MEM;
    char *layout_data = te_malloc(pg->size);
    if (!layout_data) { te_free(scratch); return ESP_ERR_NO_MEM; }
    esp_err_t err = te_read(pg->offset, layout_data, pg->size);
    if (err != ESP_OK) { te_free(layout_data); te_free(scratch); return err; }
    tl_defaults_t d = te_layout_defaults();
    tl_alloc_t a = TE_ALLOC_INIT;
    o->layout = tl_parse(layout_data, pg->size, &d, &a, scratch, TL_ITEM_MAX);
    te_free(layout_data);
    te_free(scratch);
    if (!o->layout) {
        ESP_LOGE(TE_TAG, "page %s: layout is not a JSON object (or out of memory)", pg->id);
        return ESP_FAIL;
    }
    uint16_t n = o->layout->n;
    o->is_target = te_calloc(n, 1);
    o->rel = te_calloc(n, sizeof(tr_element_t));
    o->rel_el = te_calloc(n, sizeof(uint16_t));
    if (!o->is_target || !o->rel || !o->rel_el) {
        te_prep_free(o);
        return ESP_ERR_NO_MEM;
    }
    if (!with_sidecar || !c->sc_json) return ESP_OK;

    size_t cap = 2 * (c->sc_len + 64);
    o->scratch = te_malloc(sizeof(tsc_page_t) + cap);
    if (!o->scratch) {
        te_prep_free(o);
        return ESP_ERR_NO_MEM;
    }
    tsc_page_t *sp = (tsc_page_t *)(void *)o->scratch;
    if (!tsc_find_page(c->sc_json, c->sc_len, pg->id, 0, o->scratch + sizeof(tsc_page_t), cap, sp)) {
        te_free(o->scratch);
        o->scratch = NULL;
        return ESP_OK;   // the sidecar has no such page: bin-only rendering
    }
    o->sc = sp;
    te_prep_rel(o);
    te_prep_rules(o, pg->id);
    return ESP_OK;
}

// ---------------- image variants / PSRAM plan ----------------

// Variants an element may show: base asset, rule assets, then every
// rule tint over each asset (images and pointers). Video items: the
// poster plus one entry per rule asset name (key resolved later).
static uint8_t te_collect_vars(const tr_program_t *prog, int t, const tl_elem_t *el, te_vkey_t *out)
{
    uint8_t n = 0;
    const bool video = el->video;
    int base = te_image_find(el->asset);
    if (base >= 0) {
        out[n].name = el->asset;
        out[n].asset = (int16_t)base;
        out[n].has_tint = 0;
        out[n].tint = 0;
        n++;
    } else if (!video) {
        return 0;
    }
    if (t < 0 || !prog) return n;

    uint32_t tints[TE_VAR_MAX];
    uint8_t n_tints = 0;
    for (uint8_t r = 0; r < prog->n_rules; r++) {
        const tr_rule_t *rule = &prog->rules[r];
        for (uint8_t a = 0; a < rule->n_actions; a++) {
            const tr_action_t *act = &rule->actions[a];
            if (act->target != (uint16_t)t) continue;
            if ((act->props & TR_PROP_ASSET) && act->asset && act->asset[0] && n < TE_VAR_MAX) {
                int ai = video ? -1 : te_image_find(act->asset);
                bool dup = !video && ai < 0;
                for (uint8_t j = 0; j < n && !dup; j++) {
                    dup = video ? (out[j].asset < 0 && strcmp(out[j].name, act->asset) == 0)
                                : out[j].asset == ai;
                }
                if (!dup) {
                    out[n].name = act->asset;
                    out[n].asset = (int16_t)ai;
                    out[n].has_tint = 0;
                    out[n].tint = 0;
                    n++;
                }
            }
            if ((act->props & TR_PROP_TINT) && !video && n_tints < TE_VAR_MAX) {
                bool dup = false;
                for (uint8_t j = 0; j < n_tints && !dup; j++) dup = tints[j] == act->color;
                if (!dup) tints[n_tints++] = act->color;
            }
        }
    }
    const uint8_t n_assets = n;
    for (uint8_t j = 0; j < n_tints; j++) {
        for (uint8_t a = 0; a < n_assets && n < TE_VAR_MAX; a++) {
            out[n] = out[a];
            out[n].has_tint = 1;
            out[n].tint = tints[j];
            n++;
        }
    }
    return n;
}

// Cache variant (geometry) of one key; false when nothing can be shown.
static bool te_make_variant(const tl_elem_t *el, const te_image_t *im, const te_vkey_t *k,
                            tic_variant_t *v)
{
    memset(v, 0, sizeof(*v));
    v->has_tint = k->has_tint;
    v->tint = k->has_tint ? k->tint : 0;
    if (el->kind == TR_KIND_POINTER) {
        int32_t dw = tl_px(el->width), dh = tl_px(el->height);
        if (dw < 1 || dh < 1 || dw > TPX_MAX_DIM || dh > TPX_MAX_DIM) return false;
        v->kind = TIC_KIND_POINTER;
        v->dw = (uint16_t)dw;
        v->dh = (uint16_t)dh;
        v->piv_x = tl_px(el->width * el->pivot_x / 100.0);
        v->piv_y = tl_px(el->height * el->pivot_y / 100.0);
        return true;
    }
    int32_t bw = te_min32(tl_px(el->width), im->w);
    int32_t bh = te_min32(tl_px(el->height), im->h);
    if (bw <= 0 || bh <= 0) return false;
    bool rotated = te_angle10(el->rotation) != 0;
    v->kind = TIC_KIND_IMAGE;
    v->crop = rotated ? TPX_CROP_RB : TPX_CROP_ALL;
    v->round_dc = rotated ? 0 : 1;
    v->win_w = bw;
    v->win_h = bh;
    if (!rotated) {
        v->scr_x = tl_px(el->x);
        v->scr_y = tl_px(el->y);
    }
    return true;
}

// Upper-bound estimate of the PSRAM a key costs (no pixel scan).
static size_t te_est_cost(const tl_elem_t *el, const te_image_t *im, const te_vkey_t *k)
{
    tic_variant_t v;
    if (!te_make_variant(el, im, k, &v)) return 0;
    if (v.kind == TIC_KIND_POINTER) {
        if (im->fmt == TPX_FMT_RGB565 && v.dw == im->w && v.dh == im->h)
            return (size_t)v.dw * v.dh * 2u;
        return (size_t)v.dw * v.dh * 3u;
    }
    size_t px = (size_t)v.win_w * (size_t)v.win_h;
    if (im->fmt == TPX_FMT_RGBA8888) return px * 3u;
    return px * 2u;
}

size_t te_estimate_page(const te_prep_t *p, te_skip_fn skip_cb, void *ctx)
{
    te_ctx_t *c = g_te;
    const tl_layout_t *l = p ? p->layout : NULL;
    if (!c || !l) return 0;
    const size_t budget = te_layout_has_video(l) ? TIC_BUDGET_WITH_VIDEO : TIC_BUDGET_NO_VIDEO;
    size_t used = 0, req = 0;
    te_vkey_t k[TE_VAR_MAX];
    for (int pass = TE_CLS_POINTER; pass <= TE_CLS_DECOR; pass++) {
        for (uint16_t i = 0; i < l->n; i++) {
            const tl_elem_t *el = &l->el[i];
            if (te_class(el, p->is_target && p->is_target[i]) != pass) continue;
            int t = te_target_of(p->prog, p->rel_el, i);
            uint8_t n = te_collect_vars(p->prog, t, el, k);
            for (uint8_t j = 0; j < n; j++) {
                if (k[j].asset < 0) continue;
                size_t cost = te_est_cost(el, &c->images[k[j].asset], &k[j]);
                if (pass == TE_CLS_DECOR) {
                    if (used + cost > budget) {
                        if (skip_cb) skip_cb(el->id, cost, ctx);
                    } else {
                        used += cost;
                    }
                } else if (k[j].has_tint && el->kind == TR_KIND_IMAGE) {
                    if (cost <= TE_TINT_BAKE_MAX && used + cost <= budget) used += cost;
                } else {
                    used += cost;
                    req += cost;
                }
            }
        }
    }
    return req;
}

static void te_evict_all(te_ctx_t *c)
{
    tic_make_room(&c->cache, 0, c->cache.pinned);
}

// Pinned cache entry for one key (render on miss) under policy pol.
static tic_entry_t *te_acquire(te_build_t *b, const tl_elem_t *el, const te_vkey_t *k, int pol,
                               int *status)
{
    te_ctx_t *c = b->c;
    const te_image_t *im = &c->images[k->asset];
    tic_variant_t v;
    *status = TE_ACQ_OK;
    if (!te_make_variant(el, im, k, &v)) {
        *status = TE_ACQ_EMPTY;
        return NULL;
    }
    tic_entry_t *e = tic_lookup(&c->cache, c->manifest_crc, (uint16_t)k->asset, &v);
    if (e) {
        if (pol != TE_POL_REQUIRED && b->cost + e->cost > b->budget) {
            tic_release(&c->cache, e);
            *status = TE_ACQ_BUDGET;
            return NULL;
        }
        b->cost += e->cost;
        return e;
    }

    tpx_alloc_t a = TE_ALLOC_INIT;
    uint8_t *input = te_malloc(im->size);
    if (!input) {
        te_evict_all(c);
        input = te_malloc(im->size);
    }
    if (!input) { *status = TE_ACQ_NOMEM; return NULL; }
    int64_t read_start = esp_timer_get_time();
    if (te_read_from(c, im->offset, input, im->size) != ESP_OK) {
        te_free(input);
        *status = TE_ACQ_ERR;
        return NULL;
    }
    int64_t read_us = esp_timer_get_time() - read_start;
    tpx_src_t src = {input, im->size, (tpx_fmt_t)im->fmt, im->w, im->h};
    uint8_t *rs = NULL;
    tpx_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    tpx_err_t r;
    if (v.kind == TIC_KIND_POINTER) {
        // Identity avoids resampling; the cache still owns an SD pixel copy.
        if (src.w != v.dw || src.h != v.dh) {
            r = tpx_resample(&src, v.dw, v.dh, &a, &rs);
            if (r == TPX_ERR_NOMEM) {
                te_evict_all(c);
                r = tpx_resample(&src, v.dw, v.dh, &a, &rs);
            }
            if (r != TPX_OK) {
                te_free(input);
                *status = r == TPX_ERR_NOMEM ? TE_ACQ_NOMEM : TE_ACQ_ERR;
                return NULL;
            }
            src.data = rs;
            src.size = (size_t)v.dw * v.dh * 4u;
            src.fmt = TPX_FMT_RGBA8888;
            src.w = v.dw;
            src.h = v.dh;
        }
        tpx_opts_t o = {0, 0, v.dw, v.dh, false, 0, 0, TPX_CROP_ALL};
        r = tpx_plan(&src, &o, &plan);
        if (r == TPX_OK && plan.cf != TPX_CF_NONE) {
            // keep the pivot inside the bitmap so LVGL rotates around a real pixel
            int32_t px = te_max32(0, te_min32(v.piv_x, v.dw - 1));
            int32_t py = te_max32(0, te_min32(v.piv_y, v.dh - 1));
            int32_t x0 = te_min32(plan.ox, px), y0 = te_min32(plan.oy, py);
            int32_t x1 = te_max32(plan.ox + plan.w, px + 1), y1 = te_max32(plan.oy + plan.h, py + 1);
            tpx_opts_t o2 = {x0, y0, x1 - x0, y1 - y0, false, 0, 0, TPX_CROP_NONE};
            r = tpx_plan(&src, &o2, &plan);
        }
    } else {
        tpx_opts_t o = {0, 0, v.win_w, v.win_h, v.round_dc != 0, v.scr_x, v.scr_y, v.crop};
        r = tpx_plan(&src, &o, &plan);
    }
    if (r != TPX_OK) {
        te_free(rs);
        te_free(input);
        *status = TE_ACQ_ERR;
        return NULL;
    }
    plan.zero_copy = false;   // cache owns RAM; the temporary SD input is released
    size_t cost = (plan.cf == TPX_CF_NONE || plan.zero_copy) ? 0 : plan.size;
    if (pol != TE_POL_REQUIRED && b->cost + cost > b->budget) {
        te_free(rs);
        te_free(input);
        *status = TE_ACQ_BUDGET;
        return NULL;
    }
    if (pol == TE_POL_DECOR && cost &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < cost + TE_PSRAM_RESERVE) {
        te_free(rs);
        te_free(input);
        *status = TE_ACQ_BUDGET;
        return NULL;
    }
    tic_make_room(&c->cache, cost, c->cache.pinned + b->budget);

    tpx_img_t img;
    r = tpx_render(&src, &plan, &a, &img);
    if (r == TPX_ERR_NOMEM) {
        te_evict_all(c);
        r = tpx_render(&src, &plan, &a, &img);
    }
    te_free(rs);   // only scaled pointers have a temporary RGBA bitmap
    te_free(input);
    if (r != TPX_OK) {
        *status = r == TPX_ERR_NOMEM ? TE_ACQ_NOMEM : TE_ACQ_ERR;
        return NULL;
    }
    if (v.has_tint && img.owned) {
        tpx_recolor(img.owned, (size_t)plan.w * plan.h, v.tint, TR_RECOLOR_OPA,
                    LV_COLOR_MIX_ROUND_OFS);
    }
    int64_t convert_us = esp_timer_get_time() - read_start - read_us;
    if (read_us >= 100000 || convert_us >= 100000) {
        ESP_LOGI(TE_TAG, "image %d (%ux%u, %u bytes): SD read %lld ms, convert %lld ms",
                 k->asset, (unsigned)im->w, (unsigned)im->h, (unsigned)im->size,
                 (long long)(read_us / 1000), (long long)(convert_us / 1000));
    }
    e = tic_insert(&c->cache, c->manifest_crc, (uint16_t)k->asset, &v, &img);
    if (!e) {
        *status = TE_ACQ_NOMEM;
        return NULL;
    }
    b->cost += e->cost;
    return e;
}

// ---------------- LVGL helpers ----------------

static const lv_font_t *te_montserrat(int size)
{
    switch (size) {
#if LV_FONT_MONTSERRAT_12
    case 12: return &lv_font_montserrat_12;
#endif
#if LV_FONT_MONTSERRAT_14
    case 14: return &lv_font_montserrat_14;
#endif
#if LV_FONT_MONTSERRAT_16
    case 16: return &lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_26
    case 26: return &lv_font_montserrat_26;
#endif
#if LV_FONT_MONTSERRAT_32
    case 32: return &lv_font_montserrat_32;
#endif
#if LV_FONT_MONTSERRAT_48
    case 48: return &lv_font_montserrat_48;
#endif
    default: return NULL;
    }
}

// font_asset when it loads; else built-in Montserrat 12/14/16/26/32/48
// (exact, else nearest lower, else smallest); above 48 the largest one
// plus transform_zoom = R(size * 256 / 48).
static const lv_font_t *te_label_font(const tl_elem_t *el, int16_t *zoom)
{
    static const uint8_t k_sizes[] = {12, 14, 16, 26, 32, 48};
    *zoom = 256;
    if (el->font_asset) {
        const lv_font_t *f = te_font_get(el->font_asset);
        if (f) return f;
    }
    double size = isfinite(el->font_size) ? el->font_size : 16.0;
    int r = (int)fmax(0.0, fmin(4096.0, tl_round(size)));
    const lv_font_t *exact = NULL, *lower = NULL, *smallest = NULL, *largest = NULL;
    int largest_sz = 0;
    for (size_t i = 0; i < sizeof(k_sizes); i++) {
        const lv_font_t *f = te_montserrat(k_sizes[i]);
        if (!f) continue;
        if (!smallest) smallest = f;
        largest = f;
        largest_sz = k_sizes[i];
        if (k_sizes[i] == r) exact = f;
        if (k_sizes[i] <= r) lower = f;
    }
    if (exact) return exact;
    if (largest && r > 48) {
        double z = tl_round(size * 256.0 / largest_sz);
        *zoom = (int16_t)fmin(z, 4096.0);
        return largest;
    }
    if (lower) return lower;
    return smallest ? smallest : LV_FONT_DEFAULT;
}

static void te_label_align(te_item_t *it)
{
    const tl_elem_t *el = it->el;
    lv_point_t sz;
    lv_txt_get_size(&sz, lv_label_get_text(it->obj), it->font, 0, 0, LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);
    int32_t w = sz.x, x, piv;
    switch (el->align) {
    case TL_ALIGN_CENTER:
        x = tl_px(el->x - w / 2.0);
        piv = w / 2;
        break;
    case TL_ALIGN_RIGHT:
        x = tl_px(el->x - w);
        piv = w;
        break;
    default:
        x = tl_px(el->x);
        piv = 0;
        break;
    }
    lv_obj_set_pos(it->obj, (lv_coord_t)x, (lv_coord_t)tl_px(el->y));
    if (it->transform) {
        lv_obj_set_style_transform_pivot_x(it->obj, (lv_coord_t)piv, 0);
        lv_obj_set_style_transform_pivot_y(it->obj, 0, 0);
    }
}

static void te_label_set(te_item_t *it, const char *txt)
{
    const char *cur = lv_label_get_text(it->obj);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(it->obj, txt);
    te_label_align(it);
}

static void te_apply_vis(te_item_t *it)
{
    bool hid = it->applied.hidden != 0;
    if (it->obj) {
        bool h = hid;
        if (!hid && !it->video && it->n_vars &&
            (it->kind == TR_KIND_IMAGE || it->kind == TR_KIND_POINTER)) {
            const tic_entry_t *e = it->vars[it->cur].e;
            if (!e || e->empty) h = true;
        }
        te_set_hidden(it->obj, h);
    }
    te_set_hidden(it->vbox, hid);
}

// Show variant k of an image / pointer item.
static void te_img_show(te_item_t *it, uint8_t k)
{
    const tl_elem_t *el = it->el;
    tic_entry_t *e = it->vars[k].e;
    it->cur = k;
    if (!e || e->empty) return;
    lv_obj_t *o = it->obj;
    lv_img_set_src(o, &e->dsc);   // resets the pivot to the center
    lv_obj_set_pos(o, (lv_coord_t)(tl_px(el->x) + e->ox), (lv_coord_t)(tl_px(el->y) + e->oy));
    lv_obj_set_size(o, e->dsc.header.w, e->dsc.header.h);
    if (it->kind == TR_KIND_POINTER) {
        lv_img_set_pivot(o, (lv_coord_t)(it->piv_x - e->ox), (lv_coord_t)(it->piv_y - e->oy));
        if (it->last_px != INT32_MIN) lv_img_set_angle(o, (int16_t)it->last_px);
    } else {
        int16_t ang = te_angle10(el->rotation);
        lv_img_set_pivot(o, (lv_coord_t)-e->ox, (lv_coord_t)-e->oy);   // top-left of the source
        lv_img_set_angle(o, ang);
    }
}

// Pick the variant for (asset, tint): exact baked one, else the asset
// without tint plus runtime recolor, else the base.
static void te_img_select(te_item_t *it, const te_look_t *d)
{
    int want = d->asset ? te_image_find(d->asset) : it->vars[0].k.asset;
    if (want < 0) want = it->vars[0].k.asset;
    int k = -1;
    bool rt = false;
    for (uint8_t i = 0; i < it->n_vars && k < 0 && d->tint_opa >= TR_RECOLOR_OPA; i++) {
        const te_var_t *v = &it->vars[i];
        if (v->e && v->k.asset == want && v->k.has_tint == d->has_tint &&
            (!d->has_tint || v->k.tint == d->tint)) {
            k = i;
        }
    }
    if (k < 0) {
        for (uint8_t i = 0; i < it->n_vars && k < 0; i++) {
            const te_var_t *v = &it->vars[i];
            if (v->e && v->k.asset == want && !v->k.has_tint) k = i;
        }
        if (k < 0) k = 0;
        rt = d->has_tint != 0;
    }
    if ((uint8_t)k != it->cur) te_img_show(it, (uint8_t)k);
    if (rt != (it->rt_on != 0) ||
        (rt && (d->tint != it->rt_tint || d->tint_opa != it->rt_tint_opa))) {
        lv_obj_set_style_img_recolor(it->obj, lv_color_hex(d->tint), 0);
        lv_obj_set_style_img_recolor_opa(it->obj, rt ? d->tint_opa : LV_OPA_TRANSP, 0);
        it->rt_on = rt;
        it->rt_tint = d->tint;
        it->rt_tint_opa = d->tint_opa;
    }
}

static lv_opa_t te_opa(uint8_t percent)
{
    return percent >= 100 ? LV_OPA_COVER : (lv_opa_t)((percent * LV_OPA_COVER + 50) / 100);
}

static void te_video_look(te_item_t *it, const te_look_t *d, const te_look_t *a)
{
    if (it->obj && (d->has_tint != a->has_tint || d->tint != a->tint || d->tint_opa != a->tint_opa)) {
        lv_obj_set_style_img_recolor(it->obj, lv_color_hex(d->tint), 0);
        lv_obj_set_style_img_recolor_opa(it->obj, d->has_tint ? d->tint_opa : LV_OPA_TRANSP, 0);
    }
    if (d->opacity != a->opacity) {
        if (it->obj) lv_obj_set_style_img_opa(it->obj, te_opa(d->opacity), 0);
        if (it->vid) video_elem_set_opa(it->vid, te_opa(d->opacity));
    }
    if (!it->vid) return;
    if (!te_streq(d->asset, a->asset)) {
        const char *key = it->vid_base_key;
        for (uint8_t i = 0; d->asset && i < it->n_vars; i++) {
            if (it->vars[i].vkey && strcmp(it->vars[i].k.name, d->asset) == 0) {
                key = it->vars[i].vkey;
                break;
            }
        }
        video_elem_set_key(it->vid, key);
    }
}

// Apply the desired look of a rule target, touching only what changed.
static void te_apply_look(te_item_t *it, const te_look_t *d)
{
    te_look_t *a = &it->applied;
    uint8_t cur = it->cur;
    switch (it->kind) {
    case TR_KIND_ARC:
        if (d->c1 != a->c1) lv_obj_set_style_arc_color(it->obj, lv_color_hex(d->c1), LV_PART_INDICATOR);
        if (d->c2 != a->c2) lv_obj_set_style_arc_color(it->obj, lv_color_hex(d->c2), LV_PART_MAIN);
        break;
    case TR_KIND_BAR:
        if (d->c1 != a->c1) lv_obj_set_style_bg_color(it->fg, lv_color_hex(d->c1), 0);
        if (d->c2 != a->c2) lv_obj_set_style_bg_color(it->obj, lv_color_hex(d->c2), 0);
        break;
    case TR_KIND_LABEL:
        if (d->c1 != a->c1) lv_obj_set_style_text_color(it->obj, lv_color_hex(d->c1), 0);
        if (!it->bound && !te_streq(d->text, a->text)) te_label_set(it, d->text ? d->text : "");
        break;
    case TR_KIND_IMAGE:
    case TR_KIND_POINTER:
        if (it->video) {
            te_video_look(it, d, a);
        } else if (it->obj && (!te_streq(d->asset, a->asset) || d->has_tint != a->has_tint ||
                               (d->has_tint && (d->tint != a->tint || d->tint_opa != a->tint_opa)))) {
            te_img_select(it, d);
        }
        if (!it->video && it->obj && d->opacity != a->opacity)
            lv_obj_set_style_img_opa(it->obj, te_opa(d->opacity), 0);
        break;
    default:
        break;
    }
    bool vis = d->hidden != a->hidden || cur != it->cur;
    *a = *d;
    if (vis) te_apply_vis(it);
    // Eligibility must see the updated container visibility before choosing
    // the next video (especially when restoring a previously hidden layer).
    if (vis && it->vid) video_elem_set_paused(it->vid, d->hidden != 0);
}

// ---------------- bindings ----------------

static void te_update_binding(te_item_t *it, const double *values, bool sweep_on, double sweep)
{
    const tl_elem_t *el = it->el;
    double raw = it->slot >= 0 ? values[it->slot] : NAN;

    if (el->video_value && it->vid) {
        /* Missing data holds the previous frame; creation starts at frame zero. */
        if (!sweep_on && !isfinite(raw)) return;
        double v = sweep_on ? el->range_min + sweep * (el->range_max - el->range_min) : raw;
        v = fmax(el->range_min, fmin(el->range_max, v));
        if (el->video_step > 0 && v < el->range_max) {
            v = el->range_min + floor((v - el->range_min) / el->video_step + 0.5) * el->video_step;
        }
        double pos = fmax(0.0, fmin(1.0, (v - el->range_min) / (el->range_max - el->range_min)));
        if (!it->have_last || pos != it->last_raw) video_elem_set_position(it->vid, pos);
        it->last_raw = pos;
        it->have_last = 1;
        return;
    }

    if (it->kind == TR_KIND_LABEL) {
        if (!it->bound) return;
        if (it->have_last && (raw == it->last_raw || (isnan(raw) && isnan(it->last_raw)))) return;
        char buf[TE_LABEL_MAX];
        theme_ov_label_text(it->ov, el->format, el->divisor, raw, buf, sizeof(buf));
        te_label_set(it, buf);
        it->last_raw = raw;
        it->have_last = 1;
        return;
    }

    const bool sw = sweep_on && it->bound;
    const bool same_raw = raw == it->last_raw || (isnan(raw) && isnan(it->last_raw));
    // Gauge pages tick faster than most OBD signals change. Avoid repeating
    // overlay transforms, range mapping and angle/size math when the input is
    // unchanged; sweep updates still flow because their ratio is part of the key.
    const bool same_input = sw
        ? (it->last_sweep_on && sweep == it->last_sweep)
        : (!it->last_sweep_on && same_raw);
    if (it->have_last && same_input) return;
    it->last_raw = raw;
    it->last_sweep_on = sw ? 1 : 0;
    if (sw) it->last_sweep = sweep;
    it->have_last = 1;

    double v = sw ? it->lo + sweep * (it->hi - it->lo)
                  : (isnan(raw) ? it->lo : theme_ov_transform(it->ov, raw));
    if (it->kind == TR_KIND_POINTER) {
        double deg = theme_ov_pointer_angle(v, it->lo, it->hi, el->angle_min, el->angle_max);
        if (isnan(deg)) deg = el->angle_min;
        int32_t a10 = te_angle10(deg + el->rotation);
        if (a10 != it->last_px) {
            it->last_px = a10;
            lv_img_set_angle(it->obj, (int16_t)a10);
        }
        return;
    }

    double p = sw ? sweep : theme_ov_progress(v, it->lo, it->hi);
    if (it->kind == TR_KIND_ARC) {
        // Arc values use a 0..10000 scale, not LVGL pixel coordinates.
        int32_t val = isnan(p) ? 0 : (int32_t)tl_round(10000.0 * fmax(0.0, fmin(1.0, p)));
        if (val != it->last_px) {
            it->last_px = val;
            lv_arc_set_value(it->obj, (int16_t)val);
        }
    } else if (it->kind == TR_KIND_BAR) {
        // contract A5: fg = R(w * p) (R(h * p) bottom-up when vertical)
        bool vert = it->ov && it->ov->vertical;
        int32_t px = isnan(p) ? 0 : tl_px((vert ? el->height : el->width) * p);
        if (px != it->last_px) {
            it->last_px = px;
            if (px <= 0) {
                lv_obj_add_flag(it->fg, LV_OBJ_FLAG_HIDDEN);
            } else {
                if (vert) {
                    lv_obj_set_size(it->fg, (lv_coord_t)tl_px(el->width), (lv_coord_t)px);
                    lv_obj_set_pos(it->fg, 0, (lv_coord_t)(tl_px(el->height) - px));
                } else {
                    lv_obj_set_size(it->fg, (lv_coord_t)px, (lv_coord_t)tl_px(el->height));
                }
                lv_obj_clear_flag(it->fg, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ---------------- tick ----------------

static void te_apply_targets(te_live_page_t *p, const tr_result_t *res, bool sup)
{
    for (uint16_t t = 0; t < p->prog->n_targets; t++) {
        uint16_t ii = p->tgt_item[t];
        if (ii == TE_ITEM_NONE) continue;
        te_item_t *it = &p->items[ii];
        if (!it->obj && !it->vbox) continue;
        te_look_t d = it->base;
        const tr_override_t *o = (!sup && res && t < res->n_targets) ? &res->ovr[t] : NULL;
        if (o && o->touched) {
            if (o->props & TR_PROP_COLOR) d.c1 = o->color;
            if (o->props & TR_PROP_BG_COLOR) d.c2 = o->color;
            if (o->props & TR_PROP_TINT) {
                d.has_tint = 1;
                d.tint = o->color;
                d.tint_opa = o->animated_color
                    ? (uint8_t)(o->animation_progress * TR_RECOLOR_OPA + 0.5)
                    : TR_RECOLOR_OPA;
            }
            if (o->animated_color) {
                double f = o->animation_progress;
                if (o->props & TR_PROP_COLOR) d.c1 = te_mix_color(it->base.c1, o->color, f);
                if (o->props & TR_PROP_BG_COLOR) d.c2 = te_mix_color(it->base.c2, o->color, f);
            }
            if (o->props & TR_PROP_TEXT) d.text = o->text ? o->text : "";
            if (o->props & TR_PROP_VISIBLE) d.hidden = o->visible ? 0 : 1;
            if (o->props & TR_PROP_ASSET) d.asset = o->asset;
            if (o->props & TR_PROP_OPACITY) {
                d.opacity = (uint8_t)(it->base.opacity +
                    ((double)o->opacity - it->base.opacity) * o->opacity_progress + 0.5);
            }
        }
        te_apply_look(it, &d);
    }
}

static bool te_rules_animating(const tr_result_t *res, const tr_program_t *prog)
{
    if (!prog) return false;
    if (prog->resolved_exit_mask) return true;
    if (!res) return false;
    for (uint8_t i = 0; i < res->n_active; i++) {
        const tr_rule_t *rule = &prog->rules[res->active[i]];
        for (uint8_t a = 0; a < rule->n_actions; a++) {
            if (rule->actions[a].animation.mode &&
                ((rule->actions[a].props & TR_PROP_ANY_COLOR) ||
                 ((rule->actions[a].props & TR_PROP_OPACITY) && rule->actions[a].animation.mode != 3))) return true;
        }
    }
    return false;
}

static void te_sweep_rule_inputs(double ratio, uint16_t rpm, double *values,
                                 uint64_t *changed)
{
    const int32_t internal[SIG_OBD_COUNT] = {
        [SIG_OBD_RPM] = rpm,
        [SIG_OBD_SPEED] = (int32_t)(999.0 * ratio),
        [SIG_OBD_BOOST] = (int32_t)(200.0 * ratio),
        [SIG_OBD_COOLANT_TEMP] = (int32_t)(120.0 * ratio),
        [SIG_OBD_OIL_PRESSURE] = (int32_t)(1000.0 * ratio),
        [SIG_OBD_GEAR] = (int32_t)(6.0 * ratio + 0.5),
        [SIG_OBD_BATTERY_VOLTAGE] = (int32_t)(12000.0 + 2400.0 * ratio),
        [SIG_OBD_OIL_TEMP] = (int32_t)(120.0 * ratio),
        [SIG_OBD_AFR] = (int32_t)(800.0 + 1400.0 * ratio),
        [SIG_OBD_THROTTLE] = (int32_t)(100.0 * ratio),
        [SIG_OBD_INTAKE_TEMP] = (int32_t)(120.0 * ratio),
        [SIG_OBD_LOAD] = (int32_t)(100.0 * ratio),
    };

    for (int slot = SIG_OBD_FIRST; slot < SIG_OBD_FIRST + SIG_OBD_COUNT; slot++) {
        values[slot] = signal_delivery_value(slot, internal[slot]);
        *changed |= UINT64_C(1) << slot;
    }
}

static void te_page_update(te_ctx_t *c, te_live_page_t *p)
{
    const int64_t now = te_now_ms();
    uint64_t changed = 0;
    (void)signal_store_get_delivered_mask(p->used_mask, p->values);
    uint64_t pending = p->used_mask;
    while (pending) {
        const unsigned s = (unsigned)__builtin_ctzll(pending);
        const uint64_t bit = UINT64_C(1) << s;
        pending &= pending - 1u;
        if (s >= SIG_SLOT_COUNT) continue;

        const double v = p->values[s];  // getter writes NAN for every requested invalid slot
        if (!isnan(v)) {
            if (!isnan(p->prev[s]) && p->prev[s] != v) p->chg_until[s] = now + TE_CHANGE_WINDOW_MS;
            p->prev[s] = v;
        }
        if (p->chg_until[s] > now) changed |= bit;
    }

    const bool sweep_on = c->sweep_on;
    const double sweep = c->sweep;
    const double *binding_values = p->values;
    double sweep_values[SIG_SLOT_COUNT];
    if (sweep_on) {
        memcpy(sweep_values, p->values, sizeof(sweep_values));
        te_sweep_rule_inputs(sweep, c->sweep_rpm, sweep_values, &changed);
        binding_values = sweep_values;
    }
    if (!p->bindings_seeded || !p->binding_items) {
        // The first pass also initializes unbound arcs, bars and pointers.
        for (uint16_t i = 0; i < p->n_items; i++) {
            te_item_t *it = &p->items[i];
            if ((!it->obj && !it->vid) || (it->kind == TR_KIND_IMAGE && !it->el->video_value)) continue;
            te_update_binding(it, binding_values, sweep_on, sweep);
        }
    } else {
        for (uint16_t i = 0; i < p->n_binding_items; i++) {
            te_item_t *it = &p->items[p->binding_items[i]];
            if (it->obj || it->vid) te_update_binding(it, binding_values, sweep_on, sweep);
        }
    }
    p->bindings_seeded = true;

    const bool sup = c->suppressed;
    if (p->prog) {
        // Labels and rules share delivered sweep values; live samples stay intact.
        tr_inputs_t in = {binding_values, SIG_SLOT_COUNT, 0, changed, (double)(now - p->build_ms)};
        const bool had_resolved = p->prog->have_resolved;
        const uint32_t mask = p->prog->resolved_mask;
        const uint32_t exit_mask = p->prog->resolved_exit_mask;
        const tr_result_t *res = tr_eval(p->prog, &in);
        if (sweep_on && (!p->was_sweep || mask != p->prog->resolved_mask || sup != p->was_suppressed)) {
            ESP_LOGI(TE_TAG, "sweep rules: rpm=%u active=0x%08lx targets=%u suppressed=%u",
                     (unsigned)c->sweep_rpm, (unsigned long)p->prog->resolved_mask,
                     (unsigned)p->prog->n_targets, (unsigned)sup);
        }
        // Evaluation still advances conditions, latches and debounce each tick.
        // Bindings do not change rule appearance; stable output needs no target pass.
        if (!had_resolved || mask != p->prog->resolved_mask || exit_mask != p->prog->resolved_exit_mask ||
            sup != p->was_suppressed ||
            te_rules_animating(res, p->prog))
            te_apply_targets(p, res, sup);
    }
    if (p->was_suppressed && !sup) {
        // system alarm over: the caller restored its own background, put the layout's back
        lv_obj_set_style_bg_color(p->scr, lv_color_hex(p->layout->bg_color), 0);
        lv_obj_set_style_bg_opa(p->scr, LV_OPA_COVER, 0);
    }
    if (p->was_sweep && !sweep_on)
        ESP_LOGI(TE_TAG, "sweep ended: rule inputs restored to live signals");
    p->was_sweep = sweep_on;
    p->was_suppressed = sup;
}

#if CONFIG_OBD_PERF_STATS
static void te_tick_run(void);

void theme_tick(void)
{
    PERF_BEGIN(perf_theme_start);
    te_tick_run();
    PERF_END(PERF_THEME_TICK, perf_theme_start, 1);
}

static void te_tick_run(void)
#else
void theme_tick(void)
#endif
{
    te_ctx_t *c = g_te;
    if (!c || !c->n_live) return;
    if (!te_trylock()) return;   // sidecar pairing in progress: skip this frame
    lv_obj_t *act = lv_scr_act();
    te_live_page_t *p = c->active;
    if (!p || p->scr != act) {
        // A screen transition can change lv_scr_act() without going through
        // page creation. Refresh the cache once, then keep the hot path to a
        // pointer comparison for the steady state.
        p = NULL;
        for (te_live_page_t *candidate = c->live; candidate; candidate = candidate->next) {
            if (candidate->scr == act) {
                p = candidate;
                break;
            }
        }
        c->active = p;
    }
    if (p) te_page_update(c, p);
    te_unlock();
}

bool theme_page_obd_demand_mask(lv_obj_t *screen, uint16_t *mask)
{
    te_ctx_t *c = g_te;
    uint16_t result = 0;

    if (!mask) return false;
    if (!c) {
        *mask = 0;
        return true;
    }
    if (!te_trylock()) return false;
    if (screen) {
        for (te_live_page_t *p = c->live; p; p = p->next) {
            if (p->scr == screen) {
                result = (uint16_t)(p->used_mask & ((UINT64_C(1) << SIG_OBD_COUNT) - 1u));
                break;
            }
        }
    }
    te_unlock();
    *mask = result;
    return true;
}

void theme_set_sweep(float ratio, uint16_t rpm)
{
    te_ctx_t *c = g_te;
    if (!c) return;
    c->sweep_rpm = rpm;
    if (!(ratio >= 0.0f)) {   // < 0 or NaN
        c->sweep_on = false;
        return;
    }
    c->sweep = ratio > 1.0f ? 1.0f : ratio;
    c->sweep_on = true;
}

void theme_set_rules_suppressed(bool suppressed)
{
    te_ctx_t *c = g_te;
    if (c) c->suppressed = suppressed;
}

// ---------------- page lifetime ----------------

static void te_live_free(te_ctx_t *c, te_live_page_t *p)
{
    for (uint16_t i = 0; p->items && i < p->n_items; i++) {
        te_item_t *it = &p->items[i];
        for (uint8_t j = 0; j < it->n_vars; j++) {
            if (it->vars[j].e) tic_release(&c->cache, it->vars[j].e);
            te_free(it->vars[j].vkey);
        }
        te_free(it->vars);
        if (it->ov) {
            theme_ov_alloc_t a = TE_ALLOC_INIT;
            theme_overlay_free(it->ov, &a);
        }
    }
    te_free(p->items);
    te_free(p->binding_items);
    te_free(p->tgt_item);
    if (p->prog) tr_program_free(p->prog);
    if (p->layout) {
        tl_alloc_t a = TE_ALLOC_INIT;
        tl_free(p->layout, &a);
    }
    te_free(p);
}

void te_page_on_destroy(te_live_page_t *p)
{
    te_ctx_t *c = g_te;
    if (!c || !p) return;
    te_lock();
    if (c->active == p) c->active = NULL;
    for (te_live_page_t **l = &c->live; *l; l = &(*l)->next) {
        if (*l == p) {
            *l = p->next;
            if (c->n_live) c->n_live--;
            break;
        }
    }
    // Children (images, labels, video) are deleted right after this event;
    // LVGL does not draw or refresh styles while deleting, so entries and
    // fonts may go away here.
    te_live_free(c, p);
    if (!c->n_live && !c->n_preparing && c->release_pending) te_release_locked();
    te_unlock();
}

static void te_on_delete(lv_event_t *e)
{
    te_live_page_t *p = lv_event_get_user_data(e);
    if (p && lv_event_get_target(e) == p->scr) te_page_on_destroy(p);
}

// ---------------- page build ----------------

static void te_items_init(te_ctx_t *c, te_live_page_t *p, const te_prep_t *prep)
{
    const tl_layout_t *l = p->layout;
    cJSON *ovj = NULL;
    if (prep->sc && prep->sc->overlay && prep->sc->overlay_len) {
        ovj = cJSON_ParseWithLength(prep->sc->overlay, prep->sc->overlay_len);
    }
    for (uint16_t i = 0; i < l->n; i++) {
        const tl_elem_t *el = &l->el[i];
        te_item_t *it = &p->items[i];
        it->el = el;
        it->base.opacity = (uint8_t)(el->opacity + 0.5);
        it->kind = el->kind;
        it->video = el->video;
        it->target = prep->is_target[i];
        it->slot = -1;
        it->zoom = 256;
        it->last_px = INT32_MIN;
        it->last_raw = NAN;

        // overlay: only for the element the sidecar pairs (same id and kind, first wins)
        bool paired = false;
        for (uint16_t j = 0; j < prep->n_rel && !paired; j++) paired = prep->rel_el[j] == i;
        const cJSON *entry = paired ? cJSON_GetObjectItemCaseSensitive(ovj, el->id) : NULL;
        if (cJSON_IsObject(entry)) {
            theme_ov_alloc_t a = TE_ALLOC_INIT;
            theme_overlay_t *ov = NULL;
            if (theme_overlay_parse(entry, &a, &ov)) {
                if (ov->kind && ov->kind != el->kind) theme_overlay_free(ov, &a);
                else it->ov = ov;
            }
        }

        if (el->data_source && (el->kind != TR_KIND_IMAGE || el->video_value)) {
            it->bound = 1;
            it->slot = (int16_t)te_resolve(el->data_source, NULL);
            if (it->slot >= 0) p->used_mask |= 1ull << it->slot;
            else ESP_LOGW(TE_TAG, "page %s: '%s' unknown data_source '%s' (shows no data)",
                          c->pages[p->page_idx].id, el->id, el->data_source);
        }
        theme_ov_range(it->ov, el->range_min, el->range_max, &it->lo, &it->hi);
        if (el->kind == TR_KIND_POINTER) {
            it->piv_x = tl_px(el->width * el->pivot_x / 100.0);
            it->piv_y = tl_px(el->height * el->pivot_y / 100.0);
        }

        te_look_t *b = &it->base;
        b->c1 = el->color;
        b->c2 = el->bg_color;
        b->text = el->text ? el->text : "";
        b->asset = el->asset;
        it->applied = *b;
    }
    cJSON_Delete(ovj);

    // rule sources read each tick
    const tr_program_t *prog = p->prog;
    for (uint8_t r = 0; prog && r < prog->n_rules; r++) {
        const tr_rule_t *rule = &prog->rules[r];
        if (rule->change_src < SIG_SLOT_COUNT) p->used_mask |= 1ull << rule->change_src;
        for (uint8_t k = 0; k < rule->n_conds; k++) {
            if (rule->conds[k].src < SIG_SLOT_COUNT) p->used_mask |= 1ull << rule->conds[k].src;
            if (rule->conds[k].has_cmp && rule->conds[k].cmp_src < SIG_SLOT_COUNT) {
                p->used_mask |= 1ull << rule->conds[k].cmp_src;
            }
        }
    }
}

// Video keys for set.asset: asset_files fileName without directory / extension.
static void te_video_keys(te_ctx_t *c, te_item_t *it, char *scratch)
{
    for (uint8_t j = 0; j < it->n_vars; j++) {
        te_var_t *v = &it->vars[j];
        if (v->k.asset >= 0 || !scratch || !c->sc_json) continue;
        char file[128];
        if (!tsc_asset_file(c->sc_json, c->sc_len, v->k.name, scratch, TSC_ITEM_MAX, file,
                            sizeof(file))) {
            continue;
        }
        char *base = strrchr(file, '/');
        base = base ? base + 1 : file;
        char *dot = strrchr(base, '.');
        if (dot && dot != base) *dot = '\0';
        if (base[0]) v->vkey = te_strdup(base);
    }
}

static bool te_vars_init(te_ctx_t *c, te_live_page_t *p, const uint16_t *rel_el)
{
    char *scratch = NULL;
    te_vkey_t k[TE_VAR_MAX];
    for (uint16_t i = 0; i < p->n_items; i++) {
        te_item_t *it = &p->items[i];
        if (it->kind != TR_KIND_IMAGE && it->kind != TR_KIND_POINTER) continue;
        int t = it->target ? te_target_of(p->prog, rel_el, i) : -1;
        uint8_t n = te_collect_vars(p->prog, t, it->el, k);
        if (!n) {
            ESP_LOGW(TE_TAG, "page %s: '%s' asset '%s' not in theme.bin, skipped",
                     c->pages[p->page_idx].id, it->el->id, it->el->asset ? it->el->asset : "");
            continue;
        }
        // image tint variants above the bake cap fall back to runtime recolor
        uint8_t m = 0;
        for (uint8_t j = 0; j < n; j++) {
            if (it->kind == TR_KIND_IMAGE && k[j].has_tint && k[j].asset >= 0 &&
                te_est_cost(it->el, &c->images[k[j].asset], &k[j]) > TE_TINT_BAKE_MAX) {
                continue;
            }
            k[m++] = k[j];
        }
        it->vars = te_calloc(m, sizeof(te_var_t));
        if (!it->vars) {
            te_free(scratch);
            return false;
        }
        for (uint8_t j = 0; j < m; j++) it->vars[j].k = k[j];
        it->n_vars = m;
        if (it->video && m > (k[0].asset >= 0 ? 1 : 0)) {
            if (!scratch) scratch = te_malloc(TSC_ITEM_MAX);
            te_video_keys(c, it, scratch);
        }
    }
    te_free(scratch);
    return true;
}

static void te_item_drop_vars(te_ctx_t *c, te_build_t *b, te_item_t *it)
{
    for (uint8_t j = 0; j < it->n_vars; j++) {
        tic_entry_t *e = it->vars[j].e;
        if (!e) continue;
        b->cost -= e->cost;
        tic_release(&c->cache, e);
        it->vars[j].e = NULL;
    }
}

static const char *te_acq_reason(int st)
{
    switch (st) {
    case TE_ACQ_EMPTY: return "empty box";
    case TE_ACQ_BUDGET: return "over the page budget / PSRAM reserve";
    case TE_ACQ_NOMEM: return "out of PSRAM";
    default: return "conversion failed";
    }
}

// Convert images: pointers, then rule targets, then decorative images
// bottom to top (the top-most ones are skipped first when over budget).
static bool te_images_plan(te_ctx_t *c, te_live_page_t *p, te_build_t *b)
{
    bool ok = true;
    for (int pass = TE_CLS_POINTER; pass <= TE_CLS_DECOR; pass++) {
        for (uint16_t i = 0; i < p->n_items; i++) {
            te_item_t *it = &p->items[i];
            if (!it->n_vars || te_class(it->el, it->target) != pass) continue;
            int base_st = TE_ACQ_OK;
            for (uint8_t j = 0; j < it->n_vars; j++) {
                te_var_t *v = &it->vars[j];
                if (v->k.asset < 0) continue;
                int pol = pass == TE_CLS_DECOR ? TE_POL_DECOR
                        : (v->k.has_tint && it->kind == TR_KIND_IMAGE) ? TE_POL_BUDGET
                        : TE_POL_REQUIRED;
                int st;
                v->e = te_acquire(b, it->el, &v->k, pol, &st);
                if (j == 0) base_st = st;
                if (j == 0 && !v->e) break;
            }
            if (it->vars[0].k.asset >= 0 && !it->vars[0].e) {
                if (base_st == TE_ACQ_ERR || base_st == TE_ACQ_NOMEM) ok = false;
                if (base_st == TE_ACQ_EMPTY) {
                    ESP_LOGI(TE_TAG, "page %s: '%s' has an empty box, skipped", b->page, it->el->id);
                } else {
                    ESP_LOGW(TE_TAG, "page %s: '%s' skipped: %s", b->page, it->el->id,
                             te_acq_reason(base_st));
                }
                te_item_drop_vars(c, b, it);
            }
        }
    }
    return ok;
}

static void te_create_arc(lv_obj_t *scr, te_item_t *it)
{
    const tl_elem_t *el = it->el;
    lv_obj_t *o = lv_arc_create(scr);
    it->obj = o;
    lv_obj_remove_style(o, NULL, LV_PART_KNOB);
    te_plain(o);
    int32_t w = tl_px(el->width);
    lv_obj_set_size(o, (lv_coord_t)w, (lv_coord_t)w);
    lv_obj_set_pos(o, (lv_coord_t)tl_px(el->x),
                   (lv_coord_t)tl_px(el->y + (el->height - el->width) / 2.0));
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_coord_t lw = (lv_coord_t)tl_px(el->line_width);
    lv_obj_set_style_arc_width(o, lw, LV_PART_MAIN);
    lv_obj_set_style_arc_width(o, lw, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(o, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(o, false, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(o, lv_color_hex(el->bg_color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(o, (lv_opa_t)tl_round(el->bg_opacity * 255.0 / 100.0), LV_PART_MAIN);
    lv_obj_set_style_arc_color(o, lv_color_hex(el->color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(o, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_arc_set_rotation(o, 0);
    lv_arc_set_bg_angles(o, te_deg360(el->start_angle), te_deg360(el->end_angle));
    lv_arc_set_range(o, 0, 10000);
    lv_arc_set_value(o, 0);
    it->last_px = 0;
}

static void te_create_bar(lv_obj_t *scr, te_item_t *it)
{
    const tl_elem_t *el = it->el;
    lv_coord_t radius = (lv_coord_t)tl_px(el->radius);
    lv_obj_t *bg = lv_obj_create(scr);
    it->obj = bg;
    lv_obj_remove_style_all(bg);
    te_plain(bg);
    lv_obj_set_pos(bg, (lv_coord_t)tl_px(el->x), (lv_coord_t)tl_px(el->y));
    lv_obj_set_size(bg, (lv_coord_t)tl_px(el->width), (lv_coord_t)tl_px(el->height));
    lv_obj_set_style_bg_opa(bg, (lv_opa_t)tl_round(el->bg_opacity * 255.0 / 100.0), 0);
    lv_obj_set_style_bg_color(bg, lv_color_hex(el->bg_color), 0);
    lv_obj_set_style_radius(bg, radius, 0);

    lv_obj_t *fg = lv_obj_create(bg);
    it->fg = fg;
    lv_obj_remove_style_all(fg);
    te_plain(fg);
    lv_obj_set_pos(fg, 0, 0);
    lv_obj_set_size(fg, 0, (lv_coord_t)tl_px(el->height));
    lv_obj_set_style_bg_opa(fg, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fg, lv_color_hex(el->color), 0);
    lv_obj_set_style_radius(fg, radius, 0);
    lv_obj_add_flag(fg, LV_OBJ_FLAG_HIDDEN);
    it->last_px = 0;

    if (it->ov && it->ov->has_rotation && te_angle10(it->ov->rotation)) {
        lv_obj_set_style_transform_angle(bg, te_angle10(it->ov->rotation), 0);
        lv_obj_set_style_transform_pivot_x(bg, 0, 0);
        lv_obj_set_style_transform_pivot_y(bg, 0, 0);
    }
}

static void te_create_label(lv_obj_t *scr, te_item_t *it)
{
    const tl_elem_t *el = it->el;
    lv_obj_t *o = lv_label_create(scr);
    it->obj = o;
    te_plain(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(o, 0, 0);
    it->font = te_label_font(el, &it->zoom);
    lv_obj_set_style_text_font(o, it->font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(el->color), 0);
    int16_t rot = (it->ov && it->ov->has_rotation) ? te_angle10(it->ov->rotation) : 0;
    if (it->zoom != 256 || rot) {
        it->transform = 1;
        if (it->zoom != 256) lv_obj_set_style_transform_zoom(o, it->zoom, 0);
        if (rot) lv_obj_set_style_transform_angle(o, rot, 0);
    }
    lv_label_set_text(o, it->bound ? "" : it->base.text);
    te_label_align(it);
}

static void te_create_image(lv_obj_t *scr, te_item_t *it)
{
    if (!it->n_vars || !it->vars[0].e) return;
    // decorative images with nothing visible need no object
    if (it->vars[0].e->empty && (!it->target || it->video)) return;
    lv_obj_t *o = lv_img_create(scr);
    it->obj = o;
    te_plain(o);
    lv_obj_set_style_img_opa(o, te_opa(it->base.opacity), 0);
    te_img_show(it, 0);
    te_apply_vis(it);
}

static void te_create_video(lv_obj_t *scr, te_item_t *it, const char *theme_key)
{
    const tl_elem_t *el = it->el;
    const char *key = NULL;
    if (!tl_is_video_id(el->id, &key) || !key || !key[0]) return;
    int32_t w = tl_px(el->width), h = tl_px(el->height);
    if (w <= 0 || h <= 0) return;
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    te_plain(box);
    lv_obj_set_pos(box, (lv_coord_t)tl_px(el->x), (lv_coord_t)tl_px(el->y));
    lv_obj_set_size(box, (lv_coord_t)w, (lv_coord_t)h);
    lv_obj_t *vid = el->video_value
        ? video_elem_create_value(box, theme_key, key, 0, 0, (lv_coord_t)w, (lv_coord_t)h)
        : video_elem_create(box, theme_key, key, 0, 0, (lv_coord_t)w, (lv_coord_t)h);
    if (!vid) {
        lv_obj_del(box);   // no .obv on the card: the poster stays
        return;
    }
    it->vbox = box;
    it->vid = vid;
    it->vid_base_key = key;
    video_elem_set_opa(vid, te_opa(it->base.opacity));
}

static te_live_page_t *te_prepare(te_ctx_t *c, int idx, te_prep_t *prep)
{
    const char *page_id = c->pages[idx].id;
    te_live_page_t *p = te_calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->page_idx = idx;
    p->layout = prep->layout;
    prep->layout = NULL;
    p->prog = prep->prog;
    prep->prog = NULL;
    const tl_layout_t *l = p->layout;
    p->n_items = l->n;
    p->items = te_calloc(l->n, sizeof(te_item_t));
    if (!p->items) goto fail;
    if (p->prog) {
        p->tgt_item = te_calloc(p->prog->n_targets, sizeof(uint16_t));
        if (!p->tgt_item) goto fail;
        for (uint16_t t = 0; t < p->prog->n_targets; t++) {
            uint16_t e = p->prog->targets[t].elem;
            p->tgt_item[t] = (e != TR_ELEM_NONE && e < prep->n_rel) ? prep->rel_el[e] : TE_ITEM_NONE;
        }
    }
    for (int s = 0; s < SIG_SLOT_COUNT; s++) {
        p->values[s] = NAN;
        p->prev[s] = NAN;
    }

    if (l->unknown_types) {
        ESP_LOGW(TE_TAG, "page %s: %u element(s) of unknown type skipped (first: '%s')", page_id,
                 (unsigned)l->unknown_types, l->first_unknown);
    }
    if (l->invalid || l->over_limit || l->bindings_dropped) {
        ESP_LOGW(TE_TAG, "page %s: %u invalid, %u over the %d-element limit, %u bindings dropped",
                 page_id, (unsigned)l->invalid, (unsigned)l->over_limit, TL_MAX_ELEMENTS,
                 (unsigned)l->bindings_dropped);
    }

    te_items_init(c, p, prep);
    if (!te_vars_init(c, p, prep->rel_el)) goto fail;

    // After the first page update, only bound non-image elements can change.
    // Keep their indexes compact so static decoration and unbound widgets do
    // not get revisited on every UI tick. Allocation failure keeps the full
    // scan fallback and does not make page creation fail.
    uint16_t binding_count = 0;
    for (uint16_t i = 0; i < p->n_items; i++) {
        const te_item_t *it = &p->items[i];
        if (it->bound) binding_count++;
    }
    if (binding_count) {
        p->binding_items = te_calloc(binding_count, sizeof(*p->binding_items));
        if (p->binding_items) {
            p->n_binding_items = binding_count;
            uint16_t n = 0;
            for (uint16_t i = 0; i < p->n_items; i++) {
                const te_item_t *it = &p->items[i];
                if (it->bound) p->binding_items[n++] = i;
            }
        }
    }

    return p;

fail:
    ESP_LOGE(TE_TAG, "page %s: out of memory during preparation", page_id);
    te_live_free(c, p);
    return NULL;
}

static lv_obj_t *te_publish(te_ctx_t *c, te_live_page_t *p)
{
    const char *page_id = c->pages[p->page_idx].id;
    const tl_layout_t *l = p->layout;
    const bool has_video = te_layout_has_video(l);
    const size_t budget = has_video ? TIC_BUDGET_WITH_VIDEO : TIC_BUDGET_NO_VIDEO;
    char theme_key[STORAGE_THEME_KEY_LEN];
    storage_theme_key(c->id, theme_key, sizeof(theme_key));

    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) goto fail;
    p->scr = scr;
    te_plain(scr);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(l->bg_color), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    for (uint16_t i = 0; i < p->n_items; i++) {
        te_item_t *it = &p->items[i];
        switch (it->kind) {
        case TR_KIND_ARC: te_create_arc(scr, it); break;
        case TR_KIND_BAR: te_create_bar(scr, it); break;
        case TR_KIND_LABEL: te_create_label(scr, it); break;
        case TR_KIND_POINTER: te_create_image(scr, it); break;
        case TR_KIND_IMAGE:
            te_create_image(scr, it);
            if (it->video) te_create_video(scr, it, theme_key);
            break;
        default: break;
        }
    }

    // change windows start from the current values; elapsed starts now
    p->build_ms = te_now_ms();
    // Seed change windows from one coherent snapshot instead of taking the
    // signal-store lock once per used slot while constructing the page.
    signal_store_get_delivered_mask(p->used_mask, p->values);
    for (int s = 0; s < SIG_SLOT_COUNT; s++) {
        if ((p->used_mask >> s) & 1u) p->prev[s] = p->values[s];
    }
    if (p->prog) tr_reset(p->prog);
    p->was_suppressed = c->suppressed;
    te_page_update(c, p);

    lv_obj_add_event_cb(scr, te_on_delete, LV_EVENT_DELETE, p);
    p->next = c->live;
    c->live = p;
    c->n_live++;
    if (lv_scr_act() == scr) c->active = p;

    tic_trim(&c->cache, c->cache.pinned + (has_video ? 0 : budget / 2));
    ESP_LOGI(TE_TAG, "page %s: %u elements, %u rules, %u targets, images %uK of %uK budget (cache %uK, PSRAM free %uK)",
             page_id, (unsigned)l->n, p->prog ? (unsigned)p->prog->n_rules : 0u,
             p->prog ? (unsigned)p->prog->n_targets : 0u,
             (unsigned)(p->cost / 1024), (unsigned)(budget / 1024),
             (unsigned)(c->cache.used / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return scr;

fail:
    ESP_LOGE(TE_TAG, "page %s: out of memory", page_id);
    if (p->scr) lv_obj_del(p->scr);
    te_live_free(c, p);
    return NULL;
}

static lv_obj_t *te_build(te_ctx_t *c, int idx, te_prep_t *prep)
{
    te_live_page_t *p = te_prepare(c, idx, prep);
    if (!p) return NULL;
    te_build_t b = {c, c->pages[idx].id,
                   te_layout_has_video(p->layout) ? TIC_BUDGET_WITH_VIDEO : TIC_BUDGET_NO_VIDEO, 0};
    te_images_plan(c, p, &b);
    p->cost = b.cost;
    return te_publish(c, p);
}

struct theme_page_prepared {
    te_ctx_t source;          // immutable image metadata/path + worker-owned cache
    te_live_page_t *page;     // owns layout, rules, overlays and image variants
    char page_id[TE_ID_CAP];
    bool ran;
    bool images_ok;
};

theme_page_prepared_t *theme_page_prepare_begin(const char *page_id)
{
    te_ctx_t *c = g_te;
    if (!c || !page_id) return NULL;
    te_lock();
    theme_page_prepared_t *job = NULL;
    int idx = c->custom && !c->release_pending ? te_page_find(page_id) : -1;
    if (idx >= 0) {
        te_prep_t prep;
        esp_err_t err = te_prep_page(idx, true, &prep);
        if (err == ESP_OK) {
            job = te_calloc(1, sizeof(*job));
            if (job) {
                job->page = te_prepare(c, idx, &prep);
                if (!job->page) { te_free(job); job = NULL; }
            }
        }
        te_prep_free(&prep);
    }
    if (job) {
        te_ctx_t *src = &job->source;
        src->manifest_crc = c->manifest_crc;
        src->load_generation = c->load_generation;
        src->file_size = c->file_size;
        src->map = c->map;  // host fixture only; firmware always reads SD
        memcpy(src->sd_path, c->sd_path, sizeof(src->sd_path));
        for (uint8_t i = 0; i < c->n_images; i++) {
            src->images[i] = c->images[i];
            src->images[i].name = NULL; // no borrowed metadata in the worker
        }
        src->cache.unpublished = true;
        snprintf(job->page_id, sizeof(job->page_id), "%s", page_id);
        c->n_preparing++;
    }
    te_unlock();
    return job;
}

void theme_page_prepare_run(theme_page_prepared_t *job)
{
    if (!job || job->ran) return;
    te_live_page_t *p = job->page;
    te_build_t b = {&job->source, job->page_id,
                   te_layout_has_video(p->layout) ? TIC_BUDGET_WITH_VIDEO : TIC_BUDGET_NO_VIDEO, 0};
    job->images_ok = te_images_plan(&job->source, p, &b);
    p->cost = b.cost;
    job->ran = true;
}

lv_obj_t *theme_page_prepare_finish(theme_page_prepared_t *job)
{
    if (!job) return NULL;
    te_ctx_t *c = g_te;
    lv_obj_t *scr = NULL;
    if (c) te_lock();
    if (job->ran && job->images_ok && c && c->custom && !c->release_pending &&
        c->load_generation == job->source.load_generation &&
        c->manifest_crc == job->source.manifest_crc) {
        tic_adopt(&c->cache, &job->source.cache);
        scr = te_publish(c, job->page);
    } else {
        te_live_free(&job->source, job->page);
    }
    tic_clear(&job->source.cache);
    if (c) {
        if (c->n_preparing) c->n_preparing--;
        if (!c->n_live && !c->n_preparing && c->release_pending) te_release_locked();
    }
    if (c) te_unlock();
    te_free(job);
    return scr;
}

lv_obj_t *theme_create_page(const char *page_id)
{
    te_ctx_t *c = g_te;
    if (!c || !page_id) return NULL;
    te_lock();
    lv_obj_t *scr = NULL;
    int idx = c->custom ? te_page_find(page_id) : -1;
    if (idx < 0) {
        ESP_LOGW(TE_TAG, "page '%s' not in the current theme", page_id);
    } else if (c->release_pending) {
        ESP_LOGW(TE_TAG, "page '%s': theme resources are being released, not built", page_id);
    } else {
        te_prep_t prep;
        esp_err_t err = te_prep_page(idx, true, &prep);
        if (err == ESP_OK) scr = te_build(c, idx, &prep);
        else ESP_LOGE(TE_TAG, "page '%s': %s", page_id, esp_err_to_name(err));
        te_prep_free(&prep);
    }
    te_unlock();
    return scr;
}
