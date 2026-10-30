#include "vp_profile.h"
#include "vp_internal.h"

#include <string.h>

#define ALIGN4(x) (((x) + 3u) & ~(size_t)3u)

static const cJSON *jget(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *it = jget(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static long jint(const cJSON *obj, const char *key, long dflt)
{
    long v;
    return vp_int_json_int(jget(obj, key), -1e9, 1e9, &v) ? v : dflt;
}

static float jnum(const cJSON *obj, const char *key, float dflt)
{
    double v;
    return vp_int_json_num(jget(obj, key), -1e9, 1e9, &v) ? (float)v : dflt;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static int str_index(const char *s, const char *const *names, int dflt)
{
    int i;
    if (!s) return dflt;
    for (i = 0; names[i]; i++) {
        if (!strcmp(s, names[i])) return i;
    }
    return dflt;
}

static const char *const k_rates[] = { "round", "every", "slow", "on_demand", NULL };
static const char *const k_filters[] = { "none", "temp_hyst", "ema", NULL };
static const char *const k_derive[] = { "none", "ratio", NULL };
static const char *const k_modes[] = { "none", "probe", "continuous", NULL };

typedef struct {
    size_t n_init, n_req, n_can, n_sig, n_src, n_code, n_lut, n_lut_entry;
} counts_t;

static void count_tables(const cJSON *root, counts_t *k)
{
    const cJSON *signals = jget(root, "signals");
    const cJSON *luts = jget(root, "luts");
    const cJSON *init = jget(jget(root, "bus"), "init");
    const cJSON *s;
    int si = 0, sj = 0;

    memset(k, 0, sizeof(*k));
    k->n_init = cJSON_IsArray(init) ? (size_t)cJSON_GetArraySize(init) : 0;
    cJSON_ArrayForEach(s, signals) {
        if (!cJSON_IsTrue(jget(s, "disabled"))) k->n_sig++;
    }
    while ((s = vp_int_next_src(signals, &si, &sj, NULL)) != NULL) {
        k->n_src++;
        if (vp_int_src_kind(s) == VP_SRC_CAN) k->n_can++;
        k->n_code += (size_t)cJSON_GetArraySize(jget(s, "rpn"));
    }
    k->n_req = vp_int_count_requests(signals, vp_int_bus_hdr(root, NULL));
    if (cJSON_IsArray(luts)) {
        const cJSON *t;
        k->n_lut = (size_t)cJSON_GetArraySize(luts);
        cJSON_ArrayForEach(t, luts) {
            k->n_lut_entry += (size_t)cJSON_GetArraySize(t);
        }
    }
}

static const vp_sig_def_t *registry_def(const vp_validate_cfg_t *cfg, const char *key)
{
    size_t i;
    for (i = 0; cfg && cfg->registry && i < cfg->n_registry; i++) {
        if (cfg->registry[i].key && !strcmp(cfg->registry[i].key, key)) return &cfg->registry[i];
    }
    return NULL;
}

static void fill_header(vp_profile_t *p, const cJSON *root)
{
    const cJSON *bus = jget(root, "bus");
    const cJSON *dt = jget(root, "drivetrain");
    const cJSON *mon = jget(root, "monitor");
    const cJSON *elm = jget(jget(root, "requires"), "elm");
    const cJSON *it;
    bool functional = false;

    p->magic = VP_PROFILE_MAGIC;
    p->version = VP_PROFILE_VERSION;
    p->header_size = (uint16_t)sizeof(vp_profile_t);
    copy_str(p->id, sizeof(p->id), jstr(root, "id"));
    copy_str(p->name, sizeof(p->name), jstr(root, "name"));
    p->rev = (uint16_t)jint(root, "rev", 1);

    cJSON_ArrayForEach(it, elm) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATMA")) p->requires |= VP_REQ_ATMA;
        if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATCEA")) p->requires |= VP_REQ_ATCEA;
    }

    p->proto = (uint8_t)jint(bus, "proto", VP_DEFAULT_PROTO);
    p->hdr = vp_int_bus_hdr(root, &functional);
    p->hdr_functional = functional ? 1 : 0;
    p->atst = (uint8_t)jint(bus, "atst", VP_DEFAULT_ATST);
    p->gap_ms = (uint16_t)jint(bus, "gap_ms", VP_DEFAULT_GAP_MS);
    copy_str(p->check_connection, sizeof(p->check_connection),
             jstr(bus, "check_connection") ? jstr(bus, "check_connection") : "0100");
    copy_str(p->tester_present, sizeof(p->tester_present), jstr(bus, "tester_present"));
    p->tester_present_every_ms = (uint16_t)jint(bus, "tester_present_every_ms", 0);

    p->final_drive = jnum(dt, "fd", 0.0f);
    p->tire_m = jnum(dt, "tire_m", 0.0f);
    p->gear_tol = jnum(dt, "tol", 0.0f);
    p->speed_scale = jnum(dt, "speed_scale", 1.0f);
    cJSON_ArrayForEach(it, jget(dt, "gears")) {
        if (p->gear_count < VP_MAX_GEARS && cJSON_IsNumber(it)) {
            p->gears[p->gear_count++] = (float)it->valuedouble;
        }
    }

    if (mon) {
        int mode = str_index(jstr(mon, "mode"), k_modes, 0);
        p->mon_mode = (uint8_t)(mode == 1 ? VP_MON_PROBE : (mode == 2 ? VP_MON_CONTINUOUS : VP_MON_NONE));
        p->mon_every_ms = (uint16_t)jint(mon, "every_ms", 0);
        p->mon_window_ms = (uint16_t)jint(mon, "window_ms", 0);
        p->mon_stale_ms = (uint16_t)jint(mon, "stale_ms", 0);
    }
    p->min_req_ms = VP_MIN_REQ_INTERVAL_MS;
}

typedef struct {
    vp_profile_t       *p;
    vp_at_cmd_t        *init;
    vp_request_t       *req;
    vp_can_rule_t      *can;
    vp_signal_t        *sig;
    vp_source_t        *src;
    vp_rpn_insn_t      *code;
    vp_rpn_lut_t       *lut;
    vp_rpn_lut_entry_t *lut_entry;
    size_t n_req, n_can, n_src, n_code, n_lut_entry, n_veh;
} fill_t;

static bool fill_luts(fill_t *f, const cJSON *luts)
{
    const cJSON *t;
    size_t ti = 0;

    cJSON_ArrayForEach(t, luts) {
        const cJSON *e;
        f->lut[ti].first = (uint16_t)f->n_lut_entry;
        f->lut[ti].count = (uint8_t)cJSON_GetArraySize(t);
        cJSON_ArrayForEach(e, t) {
            vp_rpn_lut_entry_t *le = &f->lut_entry[f->n_lut_entry++];
            int ne = cJSON_GetArraySize(e);
            if (ne == 2) {
                le->lo = le->hi = (float)cJSON_GetArrayItem(e, 0)->valuedouble;
                le->out = (float)cJSON_GetArrayItem(e, 1)->valuedouble;
            } else if (ne == 3) {
                le->lo = (float)cJSON_GetArrayItem(e, 0)->valuedouble;
                le->hi = (float)cJSON_GetArrayItem(e, 1)->valuedouble;
                le->out = (float)cJSON_GetArrayItem(e, 2)->valuedouble;
            } else {
                return false;
            }
        }
        ti++;
    }
    return true;
}

static bool fill_source(fill_t *f, const cJSON *root, const cJSON *s, uint16_t sig_idx)
{
    vp_source_t *o = &f->src[f->n_src];
    vp_rpn_raw_t x;
    uint16_t data_len = 0;
    bool has_x = vp_int_src_xfield(s, &x, &data_len);
    int kind = vp_int_src_kind(s);
    const cJSON *hold = jget(s, "hold");
    size_t n = 0;
    double lo, hi;

    o->kind = (uint8_t)kind;
    o->sig = sig_idx;
    o->len = (uint8_t)data_len;
    o->fail_after = (uint8_t)jint(s, "fail_after", VP_DEFAULT_FAIL_AFTER);
    if (has_x) {
        o->raw_start = x.start;
        o->raw_bits = x.bits;
        o->raw_flags = x.flags;
    }

    if (kind == VP_SRC_REQ) {
        vp_request_t key;
        size_t i;
        if (!vp_int_req_key(s, f->p->hdr, &key)) return false;
        for (i = 0; i < f->n_req; i++) {
            if (memcmp(&f->req[i], &key, sizeof(key)) == 0) break;
        }
        if (i == f->n_req) {
            if (f->n_req >= f->p->n_req) return false;
            f->req[f->n_req++] = key;
        }
        o->ref = (uint16_t)i;
        if (has_x) {
            o->flags |= VP_SRC_F_FROM_END;
            o->byte_from_end = (uint8_t)data_len;
        }
    } else if (kind == VP_SRC_CAN) {
        vp_can_rule_t *r;
        if (f->n_can >= f->p->n_can) return false;
        r = &f->can[f->n_can];
        if (!vp_parse_can_id(jstr(s, "can"), &r->can_id)) return false;
        r->src = (uint16_t)f->n_src;
        r->sig = sig_idx;
        o->ref = (uint16_t)f->n_can++;
    } else {
        return false;
    }

    if (vp_int_json_pair(jget(s, "valid"), &lo, &hi)) {
        o->flags |= VP_SRC_F_VALID;
        o->valid_lo = (float)lo;
        o->valid_hi = (float)hi;
    }
    if (cJSON_IsObject(hold)) {
        o->flags |= VP_SRC_F_HOLD;
        o->hold_delta = jnum(hold, "delta", 0.0f);
        o->hold_max = (uint8_t)jint(hold, "max_hold", 0);
        o->hold_gap_ms = (uint16_t)jint(hold, "gap_ms", 0);
    }

    if (f->n_code > f->p->n_code) return false;
    if (vp_rpn_from_json(jget(s, "rpn"), has_x ? &x : NULL, &f->code[f->n_code],
                         f->p->n_code - f->n_code, &n, NULL) != VP_RPN_OK) {
        return false;
    }
    {
        vp_rpn_verify_cfg_t vc = { data_len, f->p->n_lut };
        if (vp_rpn_verify(&f->code[f->n_code], n, &vc, NULL) != VP_RPN_OK) return false;
    }
    o->code_off = (uint16_t)f->n_code;
    o->code_len = (uint8_t)n;
    f->n_code += n;
    f->n_src++;
    (void)root;
    return true;
}

static bool fill_signal(fill_t *f, const cJSON *root, const cJSON *js, uint16_t idx,
                        const vp_validate_cfg_t *cfg)
{
    vp_signal_t *o = &f->sig[idx];
    const char *id = jstr(js, "id");
    const cJSON *post = jget(js, "post");
    const cJSON *src = jget(js, "src");
    const cJSON *it;
    double lo, hi;

    if (!id) return false;
    copy_str(o->key, sizeof(o->key), id);
    copy_str(o->name, sizeof(o->name), jstr(js, "name"));
    copy_str(o->unit, sizeof(o->unit), jstr(js, "unit"));
    if (!strncmp(id, "veh.", 4)) {
        o->num_id = (uint16_t)(VP_VEH_NUM_ID_BASE + f->n_veh++);
        o->flags |= VP_SIG_F_VEH;
    } else {
        const vp_sig_def_t *d = registry_def(cfg, id);
        o->num_id = d ? d->num_id : 0;
        /* resolve the default unit so the engine never guesses */
        if (!o->unit[0] && d && d->unit_default) copy_str(o->unit, sizeof(o->unit), d->unit_default);
    }
    o->rate = (uint8_t)str_index(jstr(js, "rate"), k_rates, VP_RATE_ROUND);
    if (o->rate == VP_RATE_SLOW) o->slow_div = (uint8_t)jint(js, "div", VP_DEFAULT_SLOW_DIV);
    o->filter = (uint8_t)str_index(jstr(js, "filter"), k_filters, VP_FILTER_NONE);
    o->derive = (uint8_t)str_index(jstr(js, "derive"), k_derive, VP_DERIVE_NONE);
    o->fallback = (uint8_t)str_index(jstr(js, "fallback"), k_derive, VP_DERIVE_NONE);
    o->period_ms = (uint16_t)jint(js, "period_ms", 0);
    if (vp_int_json_pair(jget(js, "valid"), &lo, &hi)) {
        o->flags |= VP_SIG_F_VALID;
        o->valid_lo = (float)lo;
        o->valid_hi = (float)hi;
    }
    if (cJSON_IsObject(post)) {
        if (vp_int_json_pair(jget(post, "clamp"), &lo, &hi)) {
            o->flags |= VP_SIG_F_CLAMP;
            o->clamp_lo = (float)lo;
            o->clamp_hi = (float)hi;
        }
        if (jget(post, "zero_le")) {
            o->flags |= VP_SIG_F_ZERO_LE;
            o->zero_le = jnum(post, "zero_le", 0.0f);
        }
        if (jget(post, "zero_if_rpm_lt")) {
            o->flags |= VP_SIG_F_ZERO_RPM;
            o->zero_if_rpm_lt = (uint16_t)jint(post, "zero_if_rpm_lt", 0);
        }
    }

    o->src_first = (uint16_t)f->n_src;
    cJSON_ArrayForEach(it, src) {
        if (!fill_source(f, root, it, idx)) return false;
        o->src_count++;
    }
    return true;
}

static vp_compile_err_t compile_resolved(const cJSON *root, const vp_validate_cfg_t *cfg,
                            const vp_allocator_t *alloc, vp_profile_t **out,
                            vp_issues_t *issues)
{
    counts_t k;
    size_t off, total;
    uint8_t *blob;
    fill_t f;
    const cJSON *it;
    uint16_t idx = 0;
    vp_profile_t *p;

    if (!root || !alloc || !alloc->alloc || !out) return VP_COMPILE_ERR_ARG;
    *out = NULL;
    if (vp_int_validate_resolved(root, cfg, issues) != 0) return VP_COMPILE_ERR_INVALID;

    count_tables(root, &k);
    off = sizeof(vp_profile_t);
    memset(&f, 0, sizeof(f));

#define VP__PLACE(field, n, type)                    \
    do {                                             \
        off = ALIGN4(off);                           \
        field = off;                                 \
        off += (n) * sizeof(type);                   \
    } while (0)

    {
        size_t o_init, o_req, o_can, o_sig, o_src, o_code, o_lut, o_lute;
        VP__PLACE(o_init, k.n_init, vp_at_cmd_t);
        VP__PLACE(o_req, k.n_req, vp_request_t);
        VP__PLACE(o_can, k.n_can, vp_can_rule_t);
        VP__PLACE(o_sig, k.n_sig, vp_signal_t);
        VP__PLACE(o_src, k.n_src, vp_source_t);
        VP__PLACE(o_code, k.n_code, vp_rpn_insn_t);
        VP__PLACE(o_lut, k.n_lut, vp_rpn_lut_t);
        VP__PLACE(o_lute, k.n_lut_entry, vp_rpn_lut_entry_t);
        total = ALIGN4(off);
        if (total > 0xFFFFu * 4u) return VP_COMPILE_ERR_INTERNAL;

        blob = (uint8_t *)alloc->alloc(alloc->ctx, total);
        if (!blob) return VP_COMPILE_ERR_NOMEM;
        memset(blob, 0, total);
        p = (vp_profile_t *)(void *)blob;
        p->total_size = (uint32_t)total;
        p->off_init = (uint32_t)o_init;
        p->off_req = (uint32_t)o_req;
        p->off_can = (uint32_t)o_can;
        p->off_sig = (uint32_t)o_sig;
        p->off_src = (uint32_t)o_src;
        p->off_code = (uint32_t)o_code;
        p->off_lut = (uint32_t)o_lut;
        p->off_lut_entry = (uint32_t)o_lute;
    }
#undef VP__PLACE

    p->n_init = (uint16_t)k.n_init;
    p->n_req = (uint16_t)k.n_req;
    p->n_can = (uint16_t)k.n_can;
    p->n_sig = (uint16_t)k.n_sig;
    p->n_src = (uint16_t)k.n_src;
    p->n_code = (uint16_t)k.n_code;
    p->n_lut = (uint16_t)k.n_lut;
    p->n_lut_entry = (uint16_t)k.n_lut_entry;
    fill_header(p, root);

    f.p = p;
    f.init = (vp_at_cmd_t *)(void *)(blob + p->off_init);
    f.req = (vp_request_t *)(void *)(blob + p->off_req);
    f.can = (vp_can_rule_t *)(void *)(blob + p->off_can);
    f.sig = (vp_signal_t *)(void *)(blob + p->off_sig);
    f.src = (vp_source_t *)(void *)(blob + p->off_src);
    f.code = (vp_rpn_insn_t *)(void *)(blob + p->off_code);
    f.lut = (vp_rpn_lut_t *)(void *)(blob + p->off_lut);
    f.lut_entry = (vp_rpn_lut_entry_t *)(void *)(blob + p->off_lut_entry);

    {
        size_t i = 0;
        cJSON_ArrayForEach(it, jget(jget(root, "bus"), "init")) {
            if (!vp_at_check(it->valuestring, VP_AT_CTX_INIT, f.init[i].cmd, sizeof(f.init[i].cmd), NULL)) {
                goto internal;
            }
            i++;
        }
    }
    if (!fill_luts(&f, jget(root, "luts"))) goto internal;
    cJSON_ArrayForEach(it, jget(root, "signals")) {
        if (cJSON_IsTrue(jget(it, "disabled"))) continue;
        if (!fill_signal(&f, root, it, idx, cfg)) goto internal;
        idx++;
    }
    if (f.n_req != k.n_req || f.n_can != k.n_can || f.n_src != k.n_src ||
        f.n_code != k.n_code || f.n_lut_entry != k.n_lut_entry) {
        goto internal;
    }

    p->crc32 = 0;
    p->crc32 = vp_crc32(0, blob, total);
    if (!vp_profile_check(blob, total)) goto internal;
    *out = p;
    return VP_COMPILE_OK;

internal:
    if (issues) {
        vp_issue_add(issues, "E_SCHEMA", VP_SEV_ERROR, "$", "internal compiler error");
    }
    if (alloc->free) alloc->free(alloc->ctx, blob);
    return VP_COMPILE_ERR_INTERNAL;
}

vp_compile_err_t vp_compile(const cJSON *root, const vp_validate_cfg_t *cfg,
                            const vp_allocator_t *alloc, vp_profile_t **out,
                            vp_issues_t *issues)
{
    cJSON *owned = NULL;
    const cJSON *resolved;
    vp_compile_err_t err;
    if (!root || !alloc || !alloc->alloc || !out) return VP_COMPILE_ERR_ARG;
    *out = NULL;
    resolved = vp_int_resolve_defaults(root, &owned, issues);
    if (!resolved) return VP_COMPILE_ERR_NOMEM;
    err = compile_resolved(resolved, cfg, alloc, out, issues);
    cJSON_Delete(owned);
    return err;
}

vp_compile_err_t vp_compile_json(const char *text, size_t len, const vp_validate_cfg_t *cfg,
                                 const vp_allocator_t *alloc, vp_profile_t **out,
                                 vp_issues_t *issues)
{
    size_t max = (cfg && cfg->max_json_bytes) ? cfg->max_json_bytes : VP_MAX_JSON_BYTES;
    cJSON *root;
    vp_compile_err_t e;

    if (!text || !out) return VP_COMPILE_ERR_ARG;
    *out = NULL;
    if (len > max) {
        vp_issue_add(issues, "E_SIZE", VP_SEV_ERROR, "$", "%u bytes, max %u", (unsigned)len, (unsigned)max);
        return VP_COMPILE_ERR_INVALID;
    }
    root = cJSON_ParseWithLength(text, len);
    if (!root) {
        vp_issue_add(issues, "E_SCHEMA", VP_SEV_ERROR, "$", "not valid JSON");
        return VP_COMPILE_ERR_PARSE;
    }
    e = vp_compile(root, cfg, alloc, out, issues);
    cJSON_Delete(root);
    return e;
}

void vp_profile_free(vp_profile_t *p, const vp_allocator_t *alloc)
{
    if (p && alloc && alloc->free) alloc->free(alloc->ctx, p);
}

/* ------------------------------------------------------------------ */
/* blob checks and helpers                                             */
/* ------------------------------------------------------------------ */

static bool table_ok(const vp_profile_t *p, uint32_t off, uint32_t n, size_t esz, uint32_t max_n)
{
    if (n > max_n) return false;
    if (off % 4u != 0 || off < sizeof(vp_profile_t)) return false;
    return (uint64_t)off + (uint64_t)n * esz <= p->total_size;
}

static bool cstr_ok(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

static bool request_ok(const vp_request_t *r)
{
    vp_hdr_class_t hc = vp_classify_tx_id(r->hdr);
    uint8_t side;
    uint8_t i;

    if (r->before_count > VP_MAX_REQ_COMMANDS || r->after_count > VP_MAX_REQ_COMMANDS ||
        (r->rsv & (uint8_t)~VP_REQ_RSV_HDR_EXPLICIT) != 0) return false;
    for (side = 0; side < 2; side++) {
        const vp_at_cmd_t *cmds = side == 0 ? r->before : r->after;
        uint8_t n = side == 0 ? r->before_count : r->after_count;
        for (i = 0; i < n; i++) {
            const char *why = NULL;
            if (!cstr_ok(cmds[i].cmd, sizeof(cmds[i].cmd)) ||
                !vp_at_check(cmds[i].cmd, VP_AT_CTX_REQUEST, NULL, 0, &why)) {
                (void)why;
                return false;
            }
        }
    }

    if (r->tx_len == 0 || r->tx_len > 8 || r->expect_len == 0 || r->expect_len > 8) return false;
    if (hc == VP_HDR_INVALID) return false;
    if (((r->flags & VP_REQ_F_FUNC) != 0) != (hc == VP_HDR_FUNCTIONAL)) return false;
    if (r->flags & VP_REQ_F_RX) {
        if (r->rx & VP_ID_29BIT) {
            if (((r->rx & VP_ID_MASK) & 0xFFFFFF00u) != 0x18DAF100u) return false;
        } else if (r->rx < 0x600u || r->rx > 0x7FFu || r->rx == 0x7DFu ||
                   (r->rx >= 0x7E0u && r->rx <= 0x7E7u)) {
            return false;
        }
    }
    if (r->flags & VP_REQ_F_RAW) {
        vp_rawframe_t fr[2];
        size_t nf = vp_rawframe_parse(r->tx, r->tx_len, fr), j;
        bool found = false;
        if (nf == 0 || (r->flags & VP_REQ_F_CEA)) return false;
        for (j = 0; j < nf; j++) {
            uint8_t svc = r->tx[fr[j].off];
            if (!vp_service_allowed(svc) || !vp_service_len_ok(svc, fr[j].len)) return false;
            if (svc == r->service) found = true;
        }
        if (!found) return false;
    } else {
        if (r->tx[0] != r->service || !vp_service_len_ok(r->service, r->tx_len)) return false;
    }
    if (!vp_service_allowed(r->service)) return false;
    if (hc == VP_HDR_FUNCTIONAL && !vp_service_functional_ok(r->service)) return false;
    return true;
}

bool vp_profile_check(const void *blob, size_t len)
{
    const vp_profile_t *p = (const vp_profile_t *)blob;
    const vp_request_t *req;
    const vp_can_rule_t *can;
    const vp_signal_t *sig;
    const vp_source_t *src;
    const vp_rpn_insn_t *code;
    const vp_rpn_lut_t *lut;
    const vp_at_cmd_t *init;
    uint32_t crc;
    static const uint8_t zero4[4] = { 0, 0, 0, 0 };
    uint32_t i;

    if (!blob || len < sizeof(vp_profile_t) || ((uintptr_t)blob & 3u)) return false;
    if (p->magic != VP_PROFILE_MAGIC || p->version != VP_PROFILE_VERSION) return false;
    if (p->header_size != sizeof(vp_profile_t)) return false;
    if (p->total_size < sizeof(vp_profile_t) || p->total_size > len) return false;

    crc = vp_crc32(0, blob, offsetof(vp_profile_t, crc32));
    crc = vp_crc32(crc, zero4, sizeof(zero4));
    crc = vp_crc32(crc, (const uint8_t *)blob + offsetof(vp_profile_t, crc32) + 4,
                   p->total_size - offsetof(vp_profile_t, crc32) - 4);
    if (crc != p->crc32) return false;

    if (!cstr_ok(p->id, sizeof(p->id)) || !cstr_ok(p->name, sizeof(p->name))) return false;
    if (p->gear_count > VP_MAX_GEARS || p->proto > 12 || p->mon_mode > VP_MON_CONTINUOUS) return false;
    if (vp_classify_tx_id(p->hdr) == VP_HDR_INVALID) return false;

    if (!table_ok(p, p->off_init, p->n_init, sizeof(vp_at_cmd_t), VP_MAX_INIT_CMDS) ||
        !table_ok(p, p->off_req, p->n_req, sizeof(vp_request_t), VP_MAX_REQUESTS) ||
        !table_ok(p, p->off_can, p->n_can, sizeof(vp_can_rule_t), VP_MAX_CAN_RULES) ||
        !table_ok(p, p->off_sig, p->n_sig, sizeof(vp_signal_t), VP_MAX_SIGNALS) ||
        !table_ok(p, p->off_src, p->n_src, sizeof(vp_source_t), VP_MAX_SOURCES) ||
        !table_ok(p, p->off_code, p->n_code, sizeof(vp_rpn_insn_t), VP_MAX_SOURCES * VP_RPN_MAX_INSNS) ||
        !table_ok(p, p->off_lut, p->n_lut, sizeof(vp_rpn_lut_t), VP_MAX_LUTS) ||
        !table_ok(p, p->off_lut_entry, p->n_lut_entry, sizeof(vp_rpn_lut_entry_t),
                  VP_MAX_LUTS * VP_MAX_LUT_ENTRIES)) {
        return false;
    }

    init = vp_profile_init(p);
    req = vp_profile_requests(p);
    can = vp_profile_can_rules(p);
    sig = vp_profile_signals(p);
    src = vp_profile_sources(p);
    code = vp_profile_code(p);
    lut = vp_profile_luts(p);

    for (i = 0; i < p->n_init; i++) {
        if (!cstr_ok(init[i].cmd, sizeof(init[i].cmd)) ||
            !vp_at_check(init[i].cmd, VP_AT_CTX_INIT, NULL, 0, NULL) ||
            (vp_kwp_init_header(init[i].cmd) && p->proto != 4 && p->proto != 5)) {
            return false;
        }
    }
    for (i = 0; i < p->n_req; i++) {
        if (!request_ok(&req[i])) return false;
    }
    for (i = 0; i < p->n_lut; i++) {
        if (lut[i].count == 0 || lut[i].count > VP_MAX_LUT_ENTRIES ||
            (uint32_t)lut[i].first + lut[i].count > p->n_lut_entry) {
            return false;
        }
    }
    for (i = 0; i < p->n_sig; i++) {
        const vp_signal_t *s = &sig[i];
        if (!cstr_ok(s->key, sizeof(s->key)) || !cstr_ok(s->name, sizeof(s->name)) ||
            !cstr_ok(s->unit, sizeof(s->unit))) {
            return false;
        }
        if (s->src_count > VP_MAX_SRC_PER_SIGNAL || (uint32_t)s->src_first + s->src_count > p->n_src) {
            return false;
        }
        if (s->rate > VP_RATE_ON_DEMAND || s->filter > VP_FILTER_EMA ||
            s->derive > VP_DERIVE_RATIO || s->fallback > VP_DERIVE_RATIO) {
            return false;
        }
    }
    for (i = 0; i < p->n_src; i++) {
        const vp_source_t *s = &src[i];
        vp_rpn_verify_cfg_t vc;
        if (s->sig >= p->n_sig) return false;
        if (i < sig[s->sig].src_first || i >= (uint32_t)sig[s->sig].src_first + sig[s->sig].src_count) {
            return false;
        }
        if (s->kind == VP_SRC_REQ) {
            if (s->ref >= p->n_req) return false;
        } else if (s->kind == VP_SRC_CAN) {
            if (s->ref >= p->n_can || can[s->ref].src != i) return false;
        } else {
            return false;
        }
        if ((uint32_t)s->code_off + s->code_len > p->n_code) return false;
        if (s->len > VP_MAX_DATA_LEN) return false;
        vc.data_len = s->len;
        vc.n_luts = p->n_lut;
        if (vp_rpn_verify(&code[s->code_off], s->code_len, &vc, NULL) != VP_RPN_OK) return false;
    }
    for (i = 0; i < p->n_can; i++) {
        if (can[i].src >= p->n_src || can[i].sig >= p->n_sig || src[can[i].src].kind != VP_SRC_CAN) {
            return false;
        }
    }
    return true;
}

int vp_profile_find_signal(const vp_profile_t *p, const char *key)
{
    const vp_signal_t *sig;
    uint16_t i;

    if (!p || !key) return -1;
    sig = vp_profile_signals(p);
    for (i = 0; i < p->n_sig; i++) {
        if (!strcmp(sig[i].key, key)) return (int)i;
    }
    return -1;
}

vp_rpn_err_t vp_profile_eval_source(const vp_profile_t *p, uint16_t si,
                                    const uint8_t *data, size_t len, float *out)
{
    const vp_source_t *s;
    vp_rpn_env_t env;
    vp_rpn_err_t e;
    float v;

    if (!p || !out || si >= p->n_src) return VP_RPN_ERR_ARG;
    s = &vp_profile_sources(p)[si];
    if (s->flags & VP_SRC_F_FROM_END) {
        if (!data || len < s->byte_from_end) return VP_RPN_ERR_DATA;
        data += len - s->byte_from_end;
        len = s->byte_from_end;
    }
    env = vp_profile_rpn_env(p);
    e = vp_rpn_eval(&vp_profile_code(p)[s->code_off], s->code_len, data, len, &env, &v);
    if (e != VP_RPN_OK) return e;
    if ((s->flags & VP_SRC_F_VALID) && (v < s->valid_lo || v > s->valid_hi)) return VP_RPN_ERR_RANGE;
    *out = v;
    return VP_RPN_OK;
}
