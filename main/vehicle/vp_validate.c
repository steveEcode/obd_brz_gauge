#include "vp_validate.h"
#include "vp_internal.h"
#include "app_obd_dsp/signal_registry.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* issue list                                                          */
/* ------------------------------------------------------------------ */

void vp_issues_init(vp_issues_t *l, vp_issue_t *storage, size_t cap)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->items = storage;
    l->cap = storage ? cap : 0;
}

static void issue_addv(vp_issues_t *l, const char *code, vp_severity_t sev,
                       const char *path, const char *fmt, va_list ap)
{
    vp_issue_t *it;

    if (!l) return;
    if (sev == VP_SEV_WARN) {
        l->n_warnings++;
    } else {
        l->n_errors++;
    }
    if (!l->items || l->count >= l->cap) return;
    it = &l->items[l->count++];
    it->code = code;
    it->severity = (uint8_t)sev;
    snprintf(it->path, sizeof(it->path), "%s", path ? path : "");
    vsnprintf(it->msg, sizeof(it->msg), fmt, ap);
}

void vp_issue_add(vp_issues_t *l, const char *code, vp_severity_t sev,
                  const char *path, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    issue_addv(l, code, sev, path, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* shared JSON helpers (also used by vp_compile.c)                     */
/* ------------------------------------------------------------------ */

static const cJSON *jget(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *jstr(const cJSON *obj, const char *key)
{
    const cJSON *it = jget(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

bool vp_int_json_int(const cJSON *it, double lo, double hi, long *out)
{
    double d;
    if (!cJSON_IsNumber(it)) return false;
    d = it->valuedouble;
    if (!isfinite(d) || d != floor(d) || d < lo || d > hi) return false;
    if (out) *out = (long)d;
    return true;
}

bool vp_int_json_num(const cJSON *it, double lo, double hi, double *out)
{
    double d;
    if (!cJSON_IsNumber(it)) return false;
    d = it->valuedouble;
    if (!isfinite(d) || d < lo || d > hi) return false;
    if (out) *out = d;
    return true;
}

bool vp_int_json_pair(const cJSON *it, double *lo, double *hi)
{
    double a, b;
    if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) != 2) return false;
    if (!vp_int_json_num(cJSON_GetArrayItem(it, 0), -VP_RPN_MAX_CONST, VP_RPN_MAX_CONST, &a)) return false;
    if (!vp_int_json_num(cJSON_GetArrayItem(it, 1), -VP_RPN_MAX_CONST, VP_RPN_MAX_CONST, &b)) return false;
    if (a > b) return false;
    if (lo) *lo = a;
    if (hi) *hi = b;
    return true;
}

uint32_t vp_int_bus_hdr(const cJSON *root, bool *functional)
{
    const char *h = jstr(jget(root, "bus"), "hdr");
    uint32_t id = VP_DEFAULT_HDR;
    vp_hdr_class_t c = VP_HDR_PHYSICAL;

    if (h) {
        uint32_t v;
        vp_hdr_class_t hc = vp_classify_tx_header(h, &v);
        if (hc != VP_HDR_INVALID) {
            id = v;
            c = hc;
        }
    }
    if (functional) *functional = (c == VP_HDR_FUNCTIONAL);
    return id;
}

int vp_int_src_kind(const cJSON *src)
{
    bool has_can = jget(src, "can") != NULL;
    bool has_req = jget(src, "req") != NULL || jget(src, "raw") != NULL;

    if (!cJSON_IsObject(src) || has_can == has_req) return 0;
    return has_can ? VP_SRC_CAN : VP_SRC_REQ;
}

bool vp_int_src_xfield(const cJSON *src, vp_rpn_raw_t *x, uint16_t *data_len)
{
    long v;

    memset(x, 0, sizeof(*x));
    *data_len = 0;
    if (vp_int_src_kind(src) == VP_SRC_CAN) {
        long bit = 0, bits = 0;
        const char *endian = jstr(src, "endian");
        *data_len = 8;
        if (!vp_int_json_int(jget(src, "bit"), 0, 63, &bit)) return false;
        if (!vp_int_json_int(jget(src, "bits"), 1, 32, &bits)) return false;
        x->start = (uint16_t)bit;
        x->bits = (uint8_t)bits;
        if (endian && !strcmp(endian, "be")) x->flags |= VP_RAW_BE;
        if (cJSON_IsTrue(jget(src, "signed"))) x->flags |= VP_RAW_SIGNED;
        return true;
    }
    if (vp_int_json_int(jget(src, "byte_from_end"), 1, VP_MAX_DATA_LEN, &v) &&
        cJSON_IsTrue(jget(src, "multiframe"))) {
        *data_len = (uint16_t)v;
        x->start = 0;
        x->bits = 8;
        return true;
    }
    if (vp_int_json_int(jget(src, "len"), 1, VP_MAX_DATA_LEN, &v)) {
        *data_len = (uint16_t)v;
    }
    return false;
}

bool vp_int_req_key(const cJSON *src, uint32_t bus_hdr, vp_request_t *k)
{
    const char *req = jstr(src, "req");
    const char *raw = jstr(src, "raw");
    const char *hdr = jstr(src, "hdr");
    const char *rx = jstr(src, "rx");
    const char *cea = jstr(src, "cea");
    const char *exp = jstr(src, "expect");
    const char *multi_mode = jstr(src, "multiframe_mode");
    uint8_t ebuf[8];
    size_t n = 0, en = 0;
    size_t off = 0, plen = 0;

    memset(k, 0, sizeof(*k));
    if (vp_int_src_kind(src) != VP_SRC_REQ) return false;
    if (exp && !vp_parse_hex_bytes(exp, false, ebuf, sizeof(ebuf), &en)) return false;

    if (raw) {
        vp_rawframe_t fr[2];
        size_t nf, i, pick = 0;
        if (!vp_parse_hex_bytes(raw, true, k->tx, sizeof(k->tx), &n)) return false;
        nf = vp_rawframe_parse(k->tx, n, fr);
        if (nf == 0) return false;
        for (i = 0; i < nf; i++) {
            uint8_t svc = k->tx[fr[i].off];
            if (en ? (ebuf[0] == (uint8_t)(svc + 0x40u)) : vp_service_allowed(svc)) {
                pick = i;
                break;
            }
        }
        off = fr[pick].off;
        plen = fr[pick].len;
        k->flags |= VP_REQ_F_RAW;
    } else if (req) {
        if (!vp_parse_hex_bytes(req, false, k->tx, sizeof(k->tx), &n)) return false;
        off = 0;
        plen = n;
    } else {
        return false;
    }
    k->tx_len = (uint8_t)n;
    k->service = k->tx[off];

    if (en) {
        memcpy(k->expect, ebuf, en);
        k->expect_len = (uint8_t)en;
    } else {
        k->expect[0] = (uint8_t)(k->service + 0x40u);
        if (plen > 1) memcpy(&k->expect[1], &k->tx[off + 1], plen - 1);
        k->expect_len = (uint8_t)plen;
    }

    if (hdr) {
        if (vp_classify_tx_header(hdr, &k->hdr) == VP_HDR_INVALID) return false;
        k->rsv |= VP_REQ_RSV_HDR_EXPLICIT;
    } else {
        k->hdr = bus_hdr;
    }
    if (vp_classify_tx_id(k->hdr) == VP_HDR_FUNCTIONAL) k->flags |= VP_REQ_F_FUNC;
    if (rx) {
        if (!vp_parse_rx_filter(rx, &k->rx)) return false;
        k->flags |= VP_REQ_F_RX;
    }
    if (cea) {
        size_t cn;
        if (!vp_parse_hex_bytes(cea, false, &k->cea, 1, &cn) || cn != 1) return false;
        k->flags |= VP_REQ_F_CEA;
    }
    if (cJSON_IsTrue(jget(src, "multiframe"))) k->flags |= VP_REQ_F_MULTI;
    if (multi_mode && !strcmp(multi_mode, "elm")) k->flags |= VP_REQ_F_MULTI_ELM;

    {
        const char *const keys[] = { "before", "after" };
        uint8_t *counts[] = { &k->before_count, &k->after_count };
        vp_at_cmd_t *dst[] = { k->before, k->after };
        size_t side;
        for (side = 0; side < 2; side++) {
            const cJSON *list = jget(src, keys[side]);
            int i;
            if (!list) continue;
            if (!cJSON_IsArray(list)) return false;
            if (cJSON_GetArraySize(list) > VP_MAX_REQ_COMMANDS) return false;
            for (i = 0; i < cJSON_GetArraySize(list); i++) {
                const cJSON *it = cJSON_GetArrayItem(list, i);
                const char *why = NULL;
                if (!cJSON_IsString(it) ||
                    !vp_at_check(it->valuestring, VP_AT_CTX_REQUEST,
                                 dst[side][i].cmd, sizeof(dst[side][i].cmd), &why)) {
                    (void)why;
                    return false;
                }
            }
            *counts[side] = (uint8_t)cJSON_GetArraySize(list);
        }
    }
    return true;
}

const cJSON *vp_int_next_src(const cJSON *signals, int *si, int *sj, const cJSON **sig)
{
    int ns = cJSON_GetArraySize(signals);

    while (*si < ns) {
        const cJSON *s = cJSON_GetArrayItem(signals, *si);
        const cJSON *src = jget(s, "src");
        if (cJSON_IsArray(src) && *sj < cJSON_GetArraySize(src)) {
            const cJSON *it = cJSON_GetArrayItem(src, *sj);
            (*sj)++;
            if (sig) *sig = s;
            return it;
        }
        (*si)++;
        *sj = 0;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* AT whitelist                                                        */
/* ------------------------------------------------------------------ */

static bool starts(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

static bool is_hex_str(const char *s, size_t n)
{
    size_t i;
    if (strlen(s) != n) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static const char *at_refusal(const char *a)
{
    if (starts(a, "PP")) return "ATPP writes adapter EEPROM";
    if (starts(a, "BRD") || starts(a, "BRT")) return "baud rate change";
    if (!strcmp(a, "Z") || starts(a, "WS") || !strcmp(a, "D")) return "adapter reset";
    if (starts(a, "LP")) return "low power mode";
    if (starts(a, "MA") || starts(a, "MR") || starts(a, "MT")) return "monitoring only via monitor block";
    if (starts(a, "SD") || starts(a, "SW") || starts(a, "WM") || starts(a, "KW") ||
        starts(a, "FI") || starts(a, "SI") || starts(a, "CSM") || starts(a, "CV") ||
        starts(a, "R0") || starts(a, "E1") || starts(a, "L1") || starts(a, "S0") ||
        starts(a, "H")) {
        return "fixed by firmware";
    }
    return "command not whitelisted";
}

static bool at_hex_width(const char *s, size_t a, size_t b)
{
    size_t n = strlen(s);
    return (n == a || n == b) && is_hex_str(s, n);
}

static bool at_hex_range(const char *s, size_t lo, size_t hi)
{
    size_t n = strlen(s);
    return n >= lo && n <= hi && !(n & 1u) && is_hex_str(s, n);
}

static bool obd_command(const char *s)
{
    uint8_t b[8];
    size_t n = 0;

    if (!s || !vp_parse_hex_bytes(s, false, b, sizeof(b), &n)) return false;
    return vp_service_allowed(b[0]) && vp_service_len_ok(b[0], n);
}

static bool tester_command(const char *s)
{
    size_t n;
    if (!s) return false;
    n = strlen(s);
    return (n == 2 || n == 4) && !strncmp(s, "3E", 2) &&
           (n == 2 || is_hex_str(s + 2, 2));
}

/* Fixed commands the firmware sends itself (link setup, vehicle check,
 * monitor window); never accepted from a profile. */
static bool engine_fixed(const char *a)
{
    static const char *const k_fixed[] = {
        "Z", "E0", "L0", "S1", "S0", "M0", "AL", "H0", "H1", "AT1", "I", "DPN", "RV", "MA", "AR", NULL
    };
    size_t i;
    for (i = 0; k_fixed[i]; i++) {
        if (!strcmp(a, k_fixed[i])) return true;
    }
    return false;
}

static bool init_setup(const char *a)
{
    static const char *const k_setup[] = {
        "Z", "D", "D0", "E0", "H1", "S0", "M0", "AT1", "AT0", "AT2", "AL", NULL
    };
    size_t i;
    for (i = 0; k_setup[i]; i++) {
        if (!strcmp(a, k_setup[i])) return true;
    }
    /* Car Scanner's legacy/J1850 and Nissan branches use these adapter
     * controls. Keep their accepted shapes narrow and init-only; they are
     * not safe request-local escapes. */
    if (!strcmp(a, "IB10") || !strcmp(a, "SI") || !strcmp(a, "FI") ||
        !strcmp(a, "CER") || !strcmp(a, "TAF1")) {
        return true;
    }
    if (starts(a, "SW") && is_hex_str(a + 2, 2)) return true;
    if (starts(a, "IIA") && (a[3] == '\0' || is_hex_str(a + 3, 2))) return true;
    if (starts(a, "WM") && at_hex_range(a + 2, 2, 14)) return true;
    return false;
}

/* Request-local commands are the small, non-destructive subset used by the
 * APK command queue around one OBD request.  Header/framing state is still
 * represented by the structured request fields when possible; this escape
 * hatch exists for translated BeforeCommands/AfterCommands that must retain
 * their original order. */
static bool request_setup(const char *a)
{
    return !strcmp(a, "AL");
}

bool vp_init_raw_command(const char *cmd)
{
    return cmd && !strcmp(cmd, "2212010401");
}

bool vp_init_wait_ms(const char *token, uint16_t *out_ms)
{
    char b[VP_AT_MAX_LEN + 1];
    size_t n = 0;
    size_t i;
    uint32_t value = 0;
    bool prefix;

    if (!token) return false;
    for (; *token; token++) {
        char c = *token;
        if (c == ' ' || c == '\r' || c == '\n') continue;
        if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E || n >= VP_AT_MAX_LEN) {
            return false;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        b[n++] = c;
    }
    b[n] = '\0';
    prefix = (n >= 6 && !strncmp(b, "DELAY", 5)) ||
             (n >= 5 && !strncmp(b, "WAIT", 4));
    if (!prefix) return false;
    i = b[1] == 'E' ? 5 : 4;
    if (i == n) return false;
    for (; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return false;
        value = value * 10u + (uint32_t)(b[i] - '0');
        if (value > 65535u) return false;
    }
    if (out_ms) *out_ms = (uint16_t)value;
    return true;
}

bool vp_at_check(const char *cmd, vp_at_ctx_t ctx, char *norm, size_t norm_cap,
                 const char **why)
{
    char b[VP_AT_MAX_LEN + 1];
    size_t n = 0;
    const char *a;
    const char *reason = NULL;
    bool ok = false;

    if (why) *why = NULL;
    if (!cmd) {
        if (why) *why = "missing";
        return false;
    }
    for (; *cmd; cmd++) {
        char c = *cmd;
        if (c == ' ' || c == '\r' || c == '\n') continue;
        if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E) {
            if (why) *why = "non-printable character";
            return false;
        }
        if (n >= VP_AT_MAX_LEN) {
            if (why) *why = "too long";
            return false;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        b[n++] = c;
    }
    b[n] = '\0';
    if (ctx == VP_AT_CTX_INIT && vp_init_wait_ms(b, NULL)) {
        if (norm && norm_cap) snprintf(norm, norm_cap, "%s", b);
        return true;
    }
    if (ctx == VP_AT_CTX_INIT && vp_init_raw_command(b)) {
        if (norm && norm_cap) snprintf(norm, norm_cap, "%s", b);
        return true;
    }
    if (n < 3 || b[0] != 'A' || b[1] != 'T') {
        if (why) *why = "not an AT command";
        return false;
    }
    a = b + 2;

    if (starts(a, "SH")) {
        ok = vp_classify_tx_header(a + 2, NULL) != VP_HDR_INVALID ||
             (ctx == VP_AT_CTX_INIT && vp_kwp_init_header(b));
        if (!ok) reason = "header not whitelisted";
    } else if (starts(a, "CRA")) {
        ok = a[3] == '\0' || vp_parse_rx_filter(a + 3, NULL);
        if (!ok) reason = "receive filter not whitelisted";
    } else if (starts(a, "CEA")) {
        if (ctx == VP_AT_CTX_INIT) {
            reason = "CEA only via a source's cea field";
        } else {
            ok = a[3] == '\0' || is_hex_str(a + 3, 2);
            if (!ok) reason = "CEA takes one hex byte";
        }
    } else if (!strcmp(a, "CAF0")) {
        if (ctx == VP_AT_CTX_INIT) {
            reason = "CAF0 only via a source's raw field";
        } else {
            ok = true;
        }
    } else if (!strcmp(a, "CAF1")) {
        ok = true;
    } else if (!strcmp(a, "CFC0") || !strcmp(a, "CFC1")) {
        /* Manual ISO-TP reception temporarily disables ELM automatic flow
         * control; the engine owns both transitions. */
        ok = ctx != VP_AT_CTX_INIT;
        if (!ok) reason = "CFC is controlled by the engine";
    } else if (starts(a, "FCSH")) {
        /* ELM CAN flow-control transmit header: 11-bit or 29-bit id. */
        ok = at_hex_width(a + 4, 3, 8);
        if (!ok) reason = "FCSH takes an 11-bit or 29-bit CAN id";
    } else if (starts(a, "FCSD")) {
        /* ELM CAN flow-control data: three or four CAN bytes. */
        ok = at_hex_width(a + 4, 6, 8);
        if (!ok) reason = "FCSD takes 3 or 4 CAN bytes";
    } else if (starts(a, "FCSM")) {
        ok = !strcmp(a + 4, "0") || !strcmp(a + 4, "1");
        if (!ok) reason = "FCSM takes 0 or 1";
    } else if (starts(a, "CP")) {
        ok = is_hex_str(a + 2, 2);
        if (!ok) reason = "CP takes one hex byte";
    } else if (starts(a, "ST") && is_hex_str(a + 2, 2)) {
        ok = strcmp(a + 2, "02") >= 0;
        if (!ok) reason = "ATST below 02";
    } else if (starts(a, "SP")) {
        const char *p = a + 2;
        if (ctx == VP_AT_CTX_REQUEST) {
            reason = "protocol selection is connection setup only";
        } else if (p[1] == '\0') {
            ok = (p[0] >= '0' && p[0] <= '9') || (p[0] >= 'A' && p[0] <= 'C');
        } else if (p[0] == 'A' && p[2] == '\0') {
            ok = (p[1] >= '1' && p[1] <= '9') || (p[1] >= 'A' && p[1] <= 'C');
        } else {
            ok = false;
        }
        if (!ok && !reason) reason = "protocol not 0-9/A-C";
    } else if (ctx == VP_AT_CTX_INIT && init_setup(a)) {
        /* A translated Car Scanner InitSequence may carry the complete
         * reset/default and common setup sequence in its original order. */
        ok = true;
    } else if (ctx == VP_AT_CTX_ENGINE && engine_fixed(a)) {
        ok = true;
    } else if (ctx == VP_AT_CTX_REQUEST && request_setup(a)) {
        ok = true;
    } else {
        reason = at_refusal(a);
    }

    if (!ok) {
        if (why) *why = reason ? reason : "command not whitelisted";
        return false;
    }
    if (norm && norm_cap) snprintf(norm, norm_cap, "%s", b);
    return true;
}

/* ------------------------------------------------------------------ */
/* validator                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const vp_validate_cfg_t *cfg;
    vp_issues_t *out;
    size_t       errors;
    bool         req_atma;
    bool         req_atcea;
    bool         has_monitor;
    int          mon_mode;
    bool         has_rpm;
    bool         has_speed;
    bool         ratio_ok;       /* drivetrain good enough for derive:ratio */
    int          n_luts;
    uint32_t     bus_hdr;
    size_t       n_src;
    size_t       n_can;
    size_t       n_req_src;
    size_t       n_every;
    size_t       n_veh;
    double       rate_sum;
} vctx_t;

static void verr(vctx_t *c, const char *code, const char *path, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
static void vwarn(vctx_t *c, const char *code, const char *path, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

static void verr(vctx_t *c, const char *code, const char *path, const char *fmt, ...)
{
    va_list ap;
    c->errors++;
    va_start(ap, fmt);
    issue_addv(c->out, code, VP_SEV_ERROR, path, fmt, ap);
    va_end(ap);
}

static void vwarn(vctx_t *c, const char *code, const char *path, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    issue_addv(c->out, code, VP_SEV_WARN, path, fmt, ap);
    va_end(ap);
}

/* bounded append; paths are informational, truncation is fine */
static size_t pcat(char *buf, size_t n, size_t pos, const char *s)
{
    while (s && *s && pos + 1 < n) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

/* path helper: base + "." + leaf */
static const char *pj(char *buf, size_t n, const char *base, const char *leaf)
{
    size_t pos = pcat(buf, n, 0, base);
    if (pos > 0) pos = pcat(buf, n, pos, ".");
    pcat(buf, n, pos, leaf);
    return buf;
}

static void check_keys(vctx_t *c, const cJSON *obj, const char *base, const char *const *keys)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        size_t i;
        bool known = false;
        char p[48];
        for (i = 0; keys[i]; i++) {
            if (it->string && !strcmp(it->string, keys[i])) {
                known = true;
                break;
            }
        }
        if (!known) {
            vwarn(c, "W_UNKNOWN", pj(p, sizeof(p), base, it->string ? it->string : "?"),
                  "unknown key ignored");
        }
    }
}

typedef enum { F_ABSENT = 0, F_OK, F_BAD } fstate_t;

static fstate_t f_int(vctx_t *c, const cJSON *obj, const char *key, const char *base,
                      long lo, long hi, const char *code, long *out)
{
    const cJSON *it = jget(obj, key);
    char p[48];
    if (!it) return F_ABSENT;
    if (!cJSON_IsNumber(it) || !vp_int_json_int(it, -1e12, 1e12, NULL)) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "must be an integer");
        return F_BAD;
    }
    if (!vp_int_json_int(it, (double)lo, (double)hi, out)) {
        verr(c, code, pj(p, sizeof(p), base, key), "out of range %ld..%ld", lo, hi);
        return F_BAD;
    }
    return F_OK;
}

static fstate_t f_num(vctx_t *c, const cJSON *obj, const char *key, const char *base,
                      double lo, double hi, const char *code, double *out)
{
    const cJSON *it = jget(obj, key);
    char p[48];
    if (!it) return F_ABSENT;
    if (!cJSON_IsNumber(it)) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "must be a number");
        return F_BAD;
    }
    if (!vp_int_json_num(it, lo, hi, out)) {
        verr(c, code, pj(p, sizeof(p), base, key), "out of range %g..%g", lo, hi);
        return F_BAD;
    }
    return F_OK;
}

static fstate_t f_bool(vctx_t *c, const cJSON *obj, const char *key, const char *base, bool *out)
{
    const cJSON *it = jget(obj, key);
    char p[48];
    if (!it) return F_ABSENT;
    if (!cJSON_IsBool(it)) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "must be true/false");
        return F_BAD;
    }
    if (out) *out = cJSON_IsTrue(it);
    return F_OK;
}

static fstate_t f_str(vctx_t *c, const cJSON *obj, const char *key, const char *base,
                      const char **out)
{
    const cJSON *it = jget(obj, key);
    char p[48];
    if (!it) return F_ABSENT;
    if (!cJSON_IsString(it)) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "must be a string");
        return F_BAD;
    }
    if (out) *out = it->valuestring;
    return F_OK;
}

static fstate_t f_pair(vctx_t *c, const cJSON *obj, const char *key, const char *base)
{
    const cJSON *it = jget(obj, key);
    char p[48];
    if (!it) return F_ABSENT;
    if (!vp_int_json_pair(it, NULL, NULL)) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "must be [lo, hi] with lo <= hi");
        return F_BAD;
    }
    return F_OK;
}

/* index into a fixed string list, -1 when absent/unknown (error emitted for unknown) */
static int f_enum(vctx_t *c, const cJSON *obj, const char *key, const char *base,
                  const char *const *names, int dflt)
{
    const char *s = NULL;
    fstate_t st = f_str(c, obj, key, base, &s);
    char p[48];
    int i;
    if (st == F_ABSENT) return dflt;
    if (st == F_BAD) return -1;
    for (i = 0; names[i]; i++) {
        if (!strcmp(s, names[i])) return i;
    }
    verr(c, "E_SCHEMA", pj(p, sizeof(p), base, key), "unknown value \"%.24s\"", s);
    return -1;
}

static bool schema_ok(const char *s)
{
    size_t pl = strlen(VP_SCHEMA_PREFIX);
    const char *p;
    long major = 0;
    int digits = 0;

    if (!s || strncmp(s, VP_SCHEMA_PREFIX, pl) != 0) return false;
    p = s + pl;
    while (*p >= '0' && *p <= '9' && digits < 4) {
        major = major * 10 + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0 || major != VP_SCHEMA_MAJOR) return false;
    if (*p == '\0') return true;
    if (*p != '.') return false;
    p++;
    digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) {
        p++;
        digits++;
    }
    return digits > 0 && *p == '\0';
}

static const char *rpn_code(vp_rpn_err_t e)
{
    switch (e) {
    case VP_RPN_ERR_TOO_LONG:
        return "E_LIMIT";
    case VP_RPN_ERR_EMPTY:
    case VP_RPN_ERR_UNDERFLOW:
    case VP_RPN_ERR_OVERFLOW:
    case VP_RPN_ERR_RESULT:
        return "E_RPN_STACK";
    case VP_RPN_ERR_INDEX:
    case VP_RPN_ERR_RAW:
    case VP_RPN_ERR_X:
    case VP_RPN_ERR_LUT:
        return "E_RPN_INDEX";
    case VP_RPN_ERR_CONST:
    case VP_RPN_ERR_DIV0:
        return "E_RPN_CONST";
    default:
        return "E_RPN_OP";
    }
}

static void check_rpn(vctx_t *c, const cJSON *src, const char *base)
{
    const cJSON *rpn = jget(src, "rpn");
    vp_rpn_insn_t code[VP_RPN_MAX_INSNS];
    vp_rpn_raw_t x;
    vp_rpn_verify_cfg_t vc;
    uint16_t data_len = 0;
    bool has_x;
    bool from_end;
    size_t n = 0, at = 0;
    vp_rpn_err_t e;
    char p[48];

    pj(p, sizeof(p), base, "rpn");
    if (!rpn) {
        if (jget(src, "expr")) {
            verr(c, "E_SCHEMA", p, "rpn required; compile expr with expr2rpn.py");
        } else {
            verr(c, "E_SCHEMA", p, "rpn required");
        }
        return;
    }
    if (!cJSON_IsArray(rpn)) {
        verr(c, "E_SCHEMA", p, "must be an array");
        return;
    }
    if (cJSON_GetArraySize(rpn) > VP_RPN_MAX_INSNS) {
        verr(c, "E_LIMIT", p, "%d instructions, max %d", cJSON_GetArraySize(rpn), VP_RPN_MAX_INSNS);
        return;
    }
    has_x = vp_int_src_xfield(src, &x, &data_len);
    from_end = has_x && vp_int_src_kind(src) == VP_SRC_REQ;

    if (from_end) {
        /* byte indexes would be relative to the tail view: only X is meaningful */
        const cJSON *op;
        int i = 0;
        cJSON_ArrayForEach(op, rpn) {
            const char *tag = cJSON_IsArray(op) ? cJSON_GetStringValue(cJSON_GetArrayItem(op, 0)) : NULL;
            if (tag && (!strcmp(tag, "A") || !strcmp(tag, "R"))) {
                char q[64];
                snprintf(q, sizeof(q), "%s[%d]", p, i);
                verr(c, "E_RPN_INDEX", q, "byte_from_end sources may only use X");
                return;
            }
            i++;
        }
    }

    e = vp_rpn_from_json(rpn, has_x ? &x : NULL, code, VP_RPN_MAX_INSNS, &n, &at);
    if (e == VP_RPN_OK) {
        vc.data_len = data_len;
        vc.n_luts = (uint16_t)c->n_luts;
        e = vp_rpn_verify(code, n, &vc, &at);
    }
    if (e != VP_RPN_OK) {
        char q[64];
        if (e == VP_RPN_ERR_EMPTY || e == VP_RPN_ERR_RESULT) {
            snprintf(q, sizeof(q), "%s", p);
        } else {
            snprintf(q, sizeof(q), "%s[%u]", p, (unsigned)at);
        }
        if (e == VP_RPN_ERR_INDEX || e == VP_RPN_ERR_RAW) {
            verr(c, rpn_code(e), q, "%s (len %u)", vp_rpn_err_str(e), (unsigned)data_len);
        } else {
            verr(c, rpn_code(e), q, "%s", vp_rpn_err_str(e));
        }
    }
}

static const char *const k_src_req_keys[] = {
    "req", "raw", "hdr", "rx", "cea", "len", "multiframe", "multiframe_mode",
    "byte_from_end", "hold",
    "expect", "before", "after", "valid", "fail_after", "rpn", "expr", "notes", NULL
};
static const char *const k_src_can_keys[] = {
    "can", "bit", "bits", "endian", "signed", "valid", "fail_after", "rpn", "expr", "notes", NULL
};
static const char *const k_hold_keys[] = { "delta", "max_hold", "gap_ms", NULL };

static void check_req_commands(vctx_t *c, const cJSON *src, const char *key, const char *base)
{
    const cJSON *list = jget(src, key);
    char p[48];
    int i;

    if (!list) return;
    pj(p, sizeof(p), base, key);
    if (!cJSON_IsArray(list)) {
        verr(c, "E_SCHEMA", p, "must be an array");
        return;
    }
    if (cJSON_GetArraySize(list) > VP_MAX_REQ_COMMANDS) {
        verr(c, "E_LIMIT", p, "%d commands, max %d", cJSON_GetArraySize(list), VP_MAX_REQ_COMMANDS);
    }
    for (i = 0; i < cJSON_GetArraySize(list) && i < VP_MAX_REQ_COMMANDS; i++) {
        const cJSON *it = cJSON_GetArrayItem(list, i);
        const char *why = NULL;
        char q[64];
        snprintf(q, sizeof(q), "%s[%d]", p, i);
        if (!cJSON_IsString(it)) {
            verr(c, "E_SCHEMA", q, "must be a string");
        } else if (!vp_at_check(it->valuestring, VP_AT_CTX_REQUEST, NULL, 0, &why)) {
            verr(c, "E_AT", q, "request command refused: %s", why ? why : "not whitelisted");
        }
    }
}

/* Validate the service(s) carried by a request source against header rules. */
static void check_service_hdr(vctx_t *c, const char *path, uint8_t svc, bool functional,
                              const char *hdr_txt)
{
    if (!vp_service_allowed(svc)) {
        verr(c, "E_SERVICE", path, "service 0x%02X forbidden", svc);
        return;
    }
    if (functional && !vp_service_functional_ok(svc)) {
        verr(c, "E_HDR", path, "service 0x%02X not allowed on functional %s", svc, hdr_txt);
    }
}

static void check_req_source(vctx_t *c, const cJSON *src, const char *base)
{
    const char *req = NULL, *raw = NULL, *hdr = NULL, *rx = NULL, *cea = NULL, *exp = NULL;
    uint8_t tx[8], ebuf[8];
    size_t n = 0, en = 0;
    uint8_t svcs[2];
    size_t nsvc = 0;
    bool functional;
    bool multi = false;
    long v;
    char p[48];
    char hdr_txt[12];

    check_keys(c, src, base, k_src_req_keys);
    f_str(c, src, "req", base, &req);
    f_str(c, src, "raw", base, &raw);
    f_str(c, src, "hdr", base, &hdr);
    f_str(c, src, "rx", base, &rx);
    f_str(c, src, "cea", base, &cea);
    f_str(c, src, "expect", base, &exp);

    /* header first: it decides what the services may be */
    functional = false;
    snprintf(hdr_txt, sizeof(hdr_txt), "%s", "bus.hdr");
    if (hdr) {
        uint32_t id;
        vp_hdr_class_t hc = vp_classify_tx_header(hdr, &id);
        if (hc == VP_HDR_INVALID) {
            verr(c, "E_HDR", pj(p, sizeof(p), base, "hdr"), "header \"%.12s\" not allowed", hdr);
        } else {
            functional = (hc == VP_HDR_FUNCTIONAL);
        }
        snprintf(hdr_txt, sizeof(hdr_txt), "%.8s", hdr);
    } else {
        functional = vp_classify_tx_id(c->bus_hdr) == VP_HDR_FUNCTIONAL;
    }

    if (raw) {
        vp_rawframe_t fr[2];
        size_t nf, i, good = 0;
        pj(p, sizeof(p), base, "raw");
        if (!vp_parse_hex_bytes(raw, true, tx, sizeof(tx), &n)) {
            verr(c, "E_RAWFRAME", p, "must be 1..8 hex bytes");
        } else if ((nf = vp_rawframe_parse(tx, n, fr)) == 0) {
            verr(c, "E_RAWFRAME", p, "not an ISO-TP single frame");
        } else {
            for (i = 0; i < nf; i++) {
                uint8_t svc = tx[fr[i].off];
                if (!vp_service_allowed(svc)) {
                    verr(c, nf > 1 ? "E_RAWFRAME" : "E_SERVICE", p,
                         "%s service 0x%02X forbidden", fr[i].ext ? "ext-addressed" : "normal-addressed",
                         svc);
                    continue;
                }
                if (!vp_service_len_ok(svc, fr[i].len)) {
                    verr(c, "E_SERVICE", p, "service 0x%02X with %u bytes", svc, fr[i].len);
                    continue;
                }
                svcs[nsvc++] = svc;
                good++;
                if (req) {
                    uint8_t rb[8];
                    size_t rn;
                    if (vp_parse_hex_bytes(req, false, rb, sizeof(rb), &rn) &&
                        rn == fr[i].len && memcmp(rb, &tx[fr[i].off], rn) == 0) {
                        req = NULL;   /* matched */
                    }
                }
            }
            if (good && req) {
                verr(c, "E_RAWFRAME", pj(p, sizeof(p), base, "req"), "does not match raw payload");
            }
        }
        if (!hdr) {
            verr(c, "E_HDR", pj(p, sizeof(p), base, "hdr"), "raw frames need an explicit hdr");
        }
        if (cea) {
            verr(c, "E_RAWFRAME", pj(p, sizeof(p), base, "cea"), "cea cannot be combined with raw");
        }
    } else if (req) {
        pj(p, sizeof(p), base, "req");
        if (!vp_parse_hex_bytes(req, false, tx, sizeof(tx), &n)) {
            verr(c, "E_SCHEMA", p, "must be compact hex (e.g. \"010C\")");
        } else if (!vp_service_allowed(tx[0])) {
            verr(c, "E_SERVICE", p, "service 0x%02X forbidden", tx[0]);
        } else if (!vp_service_len_ok(tx[0], n)) {
            verr(c, "E_SERVICE", p, "service 0x%02X needs %d bytes", tx[0], tx[0] == 0x22 ? 3 : 2);
        } else {
            svcs[nsvc++] = tx[0];
        }
    }

    {
        size_t i;
        for (i = 0; i < nsvc; i++) {
            check_service_hdr(c, pj(p, sizeof(p), base, raw ? "raw" : "req"), svcs[i], functional,
                              hdr_txt);
        }
    }

    if (rx && !vp_parse_rx_filter(rx, NULL)) {
        verr(c, "E_HDR", pj(p, sizeof(p), base, "rx"), "receive filter \"%.12s\" not allowed", rx);
    }
    if (cea) {
        uint8_t b;
        size_t cn = 0;
        pj(p, sizeof(p), base, "cea");
        if (!vp_parse_hex_bytes(cea, false, &b, 1, &cn) || cn != 1) {
            verr(c, "E_HDR", p, "must be one hex byte");
        } else if (!c->req_atcea) {
            verr(c, "E_SCHEMA", p, "needs requires.elm \"ATCEA\"");
        } else if (functional) {
            verr(c, "E_HDR", p, "extended addressing needs a physical hdr");
        }
    }

    check_req_commands(c, src, "before", base);
    check_req_commands(c, src, "after", base);

    f_int(c, src, "len", base, 1, VP_MAX_DATA_LEN, "E_LIMIT", NULL);
    f_bool(c, src, "multiframe", base, &multi);
    {
        static const char *const modes[] = { "elm", NULL };
        int mode = f_enum(c, src, "multiframe_mode", base, modes, -2);
        if (mode >= 0 && !multi) {
            verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "multiframe_mode"),
                 "needs \"multiframe\": true");
        } else if (mode >= 0 && jget(src, "raw")) {
            verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "multiframe_mode"),
                 "not supported with raw requests");
        }
    }
    if (f_int(c, src, "byte_from_end", base, 1, VP_MAX_DATA_LEN, "E_LIMIT", &v) == F_OK && !multi) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "byte_from_end"), "needs \"multiframe\": true");
    }
    {
        const cJSON *hold = jget(src, "hold");
        if (hold) {
            char hb[48];
            pj(hb, sizeof(hb), base, "hold");
            if (!cJSON_IsObject(hold)) {
                verr(c, "E_SCHEMA", hb, "must be an object");
            } else {
                check_keys(c, hold, hb, k_hold_keys);
                if (f_num(c, hold, "delta", hb, 0.001, 1000.0, "E_SCHEMA", NULL) == F_ABSENT ||
                    f_int(c, hold, "max_hold", hb, 1, 255, "E_SCHEMA", NULL) == F_ABSENT ||
                    f_int(c, hold, "gap_ms", hb, 0, 60000, "E_RATE", NULL) == F_ABSENT) {
                    verr(c, "E_SCHEMA", hb, "needs delta, max_hold and gap_ms");
                }
            }
        }
    }
    if (exp) {
        pj(p, sizeof(p), base, "expect");
        if (!vp_parse_hex_bytes(exp, false, ebuf, sizeof(ebuf), &en)) {
            verr(c, "E_SCHEMA", p, "must be compact hex, 1..8 bytes");
        } else if (nsvc) {
            size_t i;
            bool match = false;
            for (i = 0; i < nsvc; i++) {
                if (ebuf[0] == (uint8_t)(svcs[i] + 0x40u)) match = true;
            }
            if (!match) {
                verr(c, "E_SCHEMA", p, "must start with positive response 0x%02X",
                     (unsigned)(uint8_t)(svcs[0] + 0x40u));
            }
        }
    }
    c->n_req_src++;
}

static void check_can_source(vctx_t *c, const cJSON *src, const char *base)
{
    const char *can = NULL;
    long bit = 0, bits = 0;
    char p[48];
    static const char *const endians[] = { "le", "be", NULL };

    check_keys(c, src, base, k_src_can_keys);
    if (f_str(c, src, "can", base, &can) == F_OK && !vp_parse_can_id(can, NULL)) {
        verr(c, "E_HDR", pj(p, sizeof(p), base, "can"), "CAN id must be 3 (<=7FF) or 8 hex digits");
    }
    if (f_int(c, src, "bit", base, 0, 63, "E_RPN_INDEX", &bit) == F_ABSENT) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "bit"), "required");
    }
    if (f_int(c, src, "bits", base, 1, 32, "E_RPN_INDEX", &bits) == F_ABSENT) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "bits"), "required");
    }
    if (bit + bits > 64) {
        verr(c, "E_RPN_INDEX", pj(p, sizeof(p), base, "bits"), "field ends past bit 63");
    }
    f_enum(c, src, "endian", base, endians, 0);
    f_bool(c, src, "signed", base, NULL);
    if (!c->has_monitor) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "can"), "can sources need a monitor block");
    }
    c->n_can++;
}

static void check_source(vctx_t *c, const cJSON *src, const char *base)
{
    int kind = vp_int_src_kind(src);

    if (!cJSON_IsObject(src)) {
        verr(c, "E_SCHEMA", base, "must be an object");
        return;
    }
    if (kind == 0) {
        verr(c, "E_SCHEMA", base, "needs exactly one of req/raw or can");
        return;
    }
    if (kind == VP_SRC_CAN) {
        check_can_source(c, src, base);
    } else {
        check_req_source(c, src, base);
    }
    f_pair(c, src, "valid", base);
    f_int(c, src, "fail_after", base, 1, 50, "E_SCHEMA", NULL);
    check_rpn(c, src, base);
    c->n_src++;
}

static const char *const k_sig_keys[] = {
    "id", "name", "unit", "rate", "div", "period_ms", "valid", "post", "filter",
    "derive", "fallback", "src", "disabled", "notes", NULL
};
static const char *const k_post_keys[] = { "zero_le", "zero_if_rpm_lt", "clamp", NULL };
static const char *const k_rates[] = { "round", "every", "slow", "on_demand", NULL };
static const char *const k_filters[] = { "none", "temp_hyst", "ema", NULL };
static const char *const k_derive[] = { "none", "ratio", NULL };

static int registry_find(const vp_validate_cfg_t *cfg, const char *key)
{
    size_t i;
    if (!cfg || !cfg->registry) return -1;
    for (i = 0; i < cfg->n_registry; i++) {
        if (cfg->registry[i].key && !strcmp(cfg->registry[i].key, key)) return (int)i;
    }
    return -1;
}

static bool veh_id_ok(const char *id)
{
    const char *p = id + 4;
    size_t n = 0;
    for (; p[n]; n++) {
        char ch = p[n];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return n >= 1 && n + 4 <= VP_ID_MAX_LEN;
}

static void check_signal(vctx_t *c, const cJSON *sig, int idx, const cJSON *signals)
{
    char base[24];
    char p[48];
    const char *id = NULL;
    const char *s = NULL;
    int rate, derive, fallback;
    long period = 0;
    const cJSON *src;
    bool is_gear = false;
    int reg = -1;

    snprintf(base, sizeof(base), "signals[%d]", idx);
    if (!cJSON_IsObject(sig)) {
        verr(c, "E_SCHEMA", base, "must be an object");
        return;
    }
    check_keys(c, sig, base, k_sig_keys);

    fstate_t id_st = f_str(c, sig, "id", base, &id);
    if (id_st != F_OK) {
        if (id_st == F_ABSENT) verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "id"), "required");
    } else {
        pj(p, sizeof(p), base, "id");
        if (!strncmp(id, "obd.", 4)) {
            if (!vp_id_ok(id)) {
                verr(c, "E_NAME", p, "malformed signal id");
            } else if ((reg = registry_find(c->cfg, id)) < 0) {
                verr(c, "E_SIGNAL", p, "\"%.31s\" not in signal registry", id);
            }
            is_gear = !strcmp(id, "obd.gear");
        } else if (!strncmp(id, "veh.", 4)) {
            if (!veh_id_ok(id)) {
                verr(c, "E_NAME", p, "veh.* ids are veh.[a-z0-9_], max 31 chars");
            }
            c->n_veh++;
        } else {
            verr(c, "E_SIGNAL", p, "signal ids must be obd.* or veh.*");
        }
        /* duplicates */
        {
            int j;
            for (j = 0; j < idx; j++) {
                const char *o = jstr(cJSON_GetArrayItem(signals, j), "id");
                if (o && !strcmp(o, id)) {
                    verr(c, "E_SIGNAL", p, "duplicate of signals[%d]", j);
                    break;
                }
            }
        }
    }

    {
        bool disabled = false;
        if (f_bool(c, sig, "disabled", base, &disabled) == F_OK && disabled) {
            const cJSON *field;
            cJSON_ArrayForEach(field, sig) {
                if (field->string && strcmp(field->string, "id") &&
                    strcmp(field->string, "disabled") && strcmp(field->string, "notes")) {
                    verr(c, "E_SCHEMA", pj(p, sizeof(p), base, field->string),
                         "disabled signal only allows id, disabled and notes");
                }
            }
            return;
        }
    }

    if (f_str(c, sig, "name", base, &s) == F_OK && !vp_ascii_ok(s, VP_NAME_MAX_LEN)) {
        verr(c, "E_NAME", pj(p, sizeof(p), base, "name"), "printable ASCII, 1..%d chars", VP_NAME_MAX_LEN);
    }
    if (f_str(c, sig, "unit", base, &s) == F_OK) {
        if (!vp_ascii_ok(s, VP_UNIT_MAX_LEN)) {
            verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "unit"), "printable ASCII, 1..%d chars", VP_UNIT_MAX_LEN);
        } else if (reg >= 0 && c->cfg->unit_ok && !c->cfg->unit_ok((size_t)reg, s)) {
            verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "unit"), "\"%.7s\" is not a unit of %.31s", s, id);
        }
    }

    rate = f_enum(c, sig, "rate", base, k_rates, 0);
    if (f_int(c, sig, "div", base, 2, 64, "E_RATE", NULL) == F_OK && rate != 2) {
        verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "div"), "only with rate \"slow\"");
    }
    if (f_int(c, sig, "period_ms", base, VP_MIN_PERIOD_MS, 60000, "E_RATE", &period) == F_OK) {
        if (rate == 1) {
            verr(c, "E_RATE", pj(p, sizeof(p), base, "period_ms"), "not allowed with rate \"every\"");
        } else {
            c->rate_sum += 1000.0 / (double)period;
        }
    }
    if (rate == 1) c->n_every++;

    f_pair(c, sig, "valid", base);
    {
        const cJSON *post = jget(sig, "post");
        if (post) {
            char pb[48];
            pj(pb, sizeof(pb), base, "post");
            if (!cJSON_IsObject(post)) {
                verr(c, "E_SCHEMA", pb, "must be an object");
            } else {
                check_keys(c, post, pb, k_post_keys);
                f_num(c, post, "zero_le", pb, 0.0, 1e6, "E_SCHEMA", NULL);
                if (f_int(c, post, "zero_if_rpm_lt", pb, 1, 20000, "E_SCHEMA", NULL) == F_OK &&
                    !c->has_rpm) {
                    verr(c, "E_SCHEMA", pj(p, sizeof(p), pb, "zero_if_rpm_lt"), "profile has no obd.rpm");
                }
                f_pair(c, post, "clamp", pb);
            }
        }
    }
    f_enum(c, sig, "filter", base, k_filters, 0);

    derive = f_enum(c, sig, "derive", base, k_derive, 0);
    fallback = f_enum(c, sig, "fallback", base, k_derive, 0);
    src = jget(sig, "src");

    if (derive == 1 || fallback == 1) {
        const char *which = derive == 1 ? "derive" : "fallback";
        pj(p, sizeof(p), base, which);
        if (!is_gear) {
            verr(c, "E_SCHEMA", p, "ratio only for obd.gear");
        } else if (!c->ratio_ok) {
            verr(c, "E_SCHEMA", p, "ratio needs drivetrain fd, tire_m and gears");
        } else if (!c->has_rpm || !c->has_speed) {
            verr(c, "E_SCHEMA", p, "ratio needs obd.rpm and obd.speed");
        }
    }

    if (derive == 1) {
        if (src) verr(c, "E_SCHEMA", pj(p, sizeof(p), base, "src"), "not allowed with derive");
        return;
    }
    pj(p, sizeof(p), base, "src");
    if (!src) {
        verr(c, "E_SCHEMA", p, "required (or derive)");
        return;
    }
    if (!cJSON_IsArray(src) || cJSON_GetArraySize(src) == 0) {
        verr(c, "E_SCHEMA", p, "must be a non-empty array");
        return;
    }
    if (cJSON_GetArraySize(src) > VP_MAX_SRC_PER_SIGNAL) {
        verr(c, "E_LIMIT", p, "%d sources, max %d", cJSON_GetArraySize(src), VP_MAX_SRC_PER_SIGNAL);
        return;
    }
    {
        const cJSON *it;
        int j = 0;
        cJSON_ArrayForEach(it, src) {
            char sb[48];
            snprintf(sb, sizeof(sb), "%s.src[%d]", base, j);
            check_source(c, it, sb);
            j++;
        }
    }
}

static void check_bus(vctx_t *c, const cJSON *bus)
{
    static const char *const keys[] = { "proto", "hdr", "atst", "gap_ms", "init",
                                        "check_connection", "tester_present",
                                        "tester_present_every_ms", NULL };
    const char *hdr = NULL;
    const char *check = NULL;
    const char *tester = NULL;
    long proto = 0;
    const cJSON *init;
    char p[48];

    if (!bus) return;
    if (!cJSON_IsObject(bus)) {
        verr(c, "E_SCHEMA", "bus", "must be an object");
        return;
    }
    check_keys(c, bus, "bus", keys);
    f_int(c, bus, "proto", "bus", 0, 12, "E_AT", &proto);
    if (f_str(c, bus, "hdr", "bus", &hdr) == F_OK &&
        vp_classify_tx_header(hdr, NULL) == VP_HDR_INVALID) {
        verr(c, "E_HDR", "bus.hdr", "header \"%.12s\" not allowed", hdr);
    }
    f_int(c, bus, "atst", "bus", 2, 255, "E_AT", NULL);
    f_int(c, bus, "gap_ms", "bus", 0, 1000, "E_RATE", NULL);
    if (f_str(c, bus, "check_connection", "bus", &check) == F_OK && !obd_command(check)) {
        verr(c, "E_REQUEST", "bus.check_connection", "OBD command not allowed");
    }
    if (f_str(c, bus, "tester_present", "bus", &tester) == F_OK && !tester_command(tester)) {
        verr(c, "E_REQUEST", "bus.tester_present", "tester-present command must be 3E or 3Exx");
    }
    f_int(c, bus, "tester_present_every_ms", "bus", 0, 60000, "E_RATE", NULL);

    init = jget(bus, "init");
    if (init) {
        const cJSON *it;
        int i = 0;
        if (!cJSON_IsArray(init)) {
            verr(c, "E_SCHEMA", "bus.init", "must be an array");
            return;
        }
        if (cJSON_GetArraySize(init) > VP_MAX_INIT_CMDS) {
            verr(c, "E_LIMIT", "bus.init", "%d commands, max %d", cJSON_GetArraySize(init), VP_MAX_INIT_CMDS);
            return;
        }
        cJSON_ArrayForEach(it, init) {
            const char *why = NULL;
            char normalized[VP_AT_MAX_LEN + 1];
            snprintf(p, sizeof(p), "bus.init[%d]", i++);
            if (!cJSON_IsString(it)) {
                verr(c, "E_SCHEMA", p, "must be a string");
            } else if (!vp_at_check(it->valuestring, VP_AT_CTX_INIT, normalized, sizeof(normalized), &why)) {
                verr(c, "E_AT", p, "\"%.16s\": %s", it->valuestring, why ? why : "refused");
            } else if (vp_kwp_init_header(normalized) &&
                       proto != 4 && proto != 5) {
                verr(c, "E_AT", p, "KWP initialization header requires explicit bus.proto 4 or 5");
            }
        }
    }
}

static void check_requires(vctx_t *c, const cJSON *req)
{
    static const char *const keys[] = { "elm", NULL };
    const cJSON *elm, *it;
    int i = 0;
    char p[48];

    if (!req) return;
    if (!cJSON_IsObject(req)) {
        verr(c, "E_SCHEMA", "requires", "must be an object");
        return;
    }
    check_keys(c, req, "requires", keys);
    elm = jget(req, "elm");
    if (!elm) return;
    if (!cJSON_IsArray(elm) || cJSON_GetArraySize(elm) > 4) {
        verr(c, "E_SCHEMA", "requires.elm", "must be an array of at most 4 capabilities");
        return;
    }
    cJSON_ArrayForEach(it, elm) {
        snprintf(p, sizeof(p), "requires.elm[%d]", i++);
        if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATMA")) {
            c->req_atma = true;
        } else if (cJSON_IsString(it) && !strcmp(it->valuestring, "ATCEA")) {
            c->req_atcea = true;
        } else {
            verr(c, "E_SCHEMA", p, "unknown capability (ATMA, ATCEA)");
        }
    }
}

static void check_drivetrain(vctx_t *c, const cJSON *dt)
{
    static const char *const keys[] = { "fd", "tire_m", "gears", "tol", "speed_scale", NULL };
    const cJSON *gears;
    bool fd_ok, tire_ok;
    int ngears = 0;

    if (!dt) return;
    if (!cJSON_IsObject(dt)) {
        verr(c, "E_SCHEMA", "drivetrain", "must be an object");
        return;
    }
    check_keys(c, dt, "drivetrain", keys);
    fd_ok = f_num(c, dt, "fd", "drivetrain", 0.1, 30.0, "E_SCHEMA", NULL) == F_OK;
    tire_ok = f_num(c, dt, "tire_m", "drivetrain", 0.1, 1.0, "E_SCHEMA", NULL) == F_OK;
    f_num(c, dt, "tol", "drivetrain", 0.0, 0.5, "E_SCHEMA", NULL);
    f_num(c, dt, "speed_scale", "drivetrain", 0.5, 2.0, "E_SCHEMA", NULL);
    gears = jget(dt, "gears");
    if (gears) {
        const cJSON *g;
        int i = 0;
        if (!cJSON_IsArray(gears)) {
            verr(c, "E_SCHEMA", "drivetrain.gears", "must be an array");
        } else if (cJSON_GetArraySize(gears) > VP_MAX_GEARS) {
            verr(c, "E_LIMIT", "drivetrain.gears", "%d gears, max %d", cJSON_GetArraySize(gears), VP_MAX_GEARS);
        } else {
            bool all_ok = true;
            cJSON_ArrayForEach(g, gears) {
                char p[48];
                if (!vp_int_json_num(g, 0.05, 30.0, NULL)) {
                    snprintf(p, sizeof(p), "drivetrain.gears[%d]", i);
                    verr(c, "E_SCHEMA", p, "gear ratio must be 0.05..30");
                    all_ok = false;
                }
                i++;
            }
            if (all_ok) ngears = i;
        }
    }
    c->ratio_ok = fd_ok && tire_ok && ngears > 0;
}

static void check_monitor(vctx_t *c, const cJSON *mon)
{
    static const char *const keys[] = { "mode", "every_ms", "window_ms", "stale_ms", NULL };
    static const char *const modes[] = { "none", "probe", "continuous", NULL };
    int mode;

    if (!mon) return;
    if (!cJSON_IsObject(mon)) {
        verr(c, "E_SCHEMA", "monitor", "must be an object");
        return;
    }
    check_keys(c, mon, "monitor", keys);
    c->has_monitor = true;
    mode = f_enum(c, mon, "mode", "monitor", modes, -2);
    if (mode == -2) {
        verr(c, "E_SCHEMA", "monitor.mode", "required (probe, continuous)");
        mode = -1;
    } else if (mode == 0) {
        verr(c, "E_SCHEMA", "monitor.mode", "use probe or continuous, or drop the block");
        mode = -1;
    }
    c->mon_mode = mode;
    if (!c->req_atma) {
        verr(c, "E_SCHEMA", "monitor", "needs requires.elm \"ATMA\"");
    }
    if (mode == 1) {
        if (f_int(c, mon, "every_ms", "monitor", 250, 60000, "E_RATE", NULL) == F_ABSENT) {
            verr(c, "E_SCHEMA", "monitor.every_ms", "required for probe");
        }
        if (f_int(c, mon, "window_ms", "monitor", 10, 200, "E_RATE", NULL) == F_ABSENT) {
            verr(c, "E_SCHEMA", "monitor.window_ms", "required for probe");
        }
    }
    f_int(c, mon, "stale_ms", "monitor", 0, 60000, "E_RATE", NULL);
}

static void check_luts(vctx_t *c, const cJSON *luts)
{
    const cJSON *t;
    int i = 0;

    if (!luts) return;
    if (!cJSON_IsArray(luts)) {
        verr(c, "E_SCHEMA", "luts", "must be an array");
        return;
    }
    if (cJSON_GetArraySize(luts) > VP_MAX_LUTS) {
        verr(c, "E_LIMIT", "luts", "%d tables, max %d", cJSON_GetArraySize(luts), VP_MAX_LUTS);
        return;
    }
    cJSON_ArrayForEach(t, luts) {
        char p[48];
        const cJSON *e;
        int j = 0;
        snprintf(p, sizeof(p), "luts[%d]", i);
        if (!cJSON_IsArray(t) || cJSON_GetArraySize(t) == 0) {
            verr(c, "E_SCHEMA", p, "must be a non-empty array");
        } else if (cJSON_GetArraySize(t) > VP_MAX_LUT_ENTRIES) {
            verr(c, "E_LIMIT", p, "%d entries, max %d", cJSON_GetArraySize(t), VP_MAX_LUT_ENTRIES);
        } else {
            cJSON_ArrayForEach(e, t) {
                int ne = cJSON_GetArraySize(e);
                bool ok = cJSON_IsArray(e) && (ne == 2 || ne == 3);
                int k;
                for (k = 0; ok && k < ne; k++) {
                    ok = vp_int_json_num(cJSON_GetArrayItem(e, k), -VP_RPN_MAX_CONST, VP_RPN_MAX_CONST, NULL);
                }
                if (ok && ne == 3) {
                    ok = cJSON_GetArrayItem(e, 0)->valuedouble <= cJSON_GetArrayItem(e, 1)->valuedouble;
                }
                if (!ok) {
                    char q[64];
                    snprintf(q, sizeof(q), "%s[%d]", p, j);
                    verr(c, "E_SCHEMA", q, "entry must be [key, out] or [lo, hi, out]");
                }
                j++;
            }
        }
        i++;
    }
    c->n_luts = cJSON_GetArraySize(luts);
}

size_t vp_int_count_requests(const cJSON *signals, uint32_t bus_hdr)
{
    int si = 0, sj = 0;
    size_t distinct = 0;
    const cJSON *s;

    while ((s = vp_int_next_src(signals, &si, &sj, NULL)) != NULL) {
        vp_request_t k;
        int ti = 0, tj = 0;
        const cJSON *t;
        bool seen = false;
        int cur_i = si, cur_j = sj - 1;

        if (!vp_int_req_key(s, bus_hdr, &k)) continue;
        while ((t = vp_int_next_src(signals, &ti, &tj, NULL)) != NULL) {
            vp_request_t o;
            if (ti == cur_i && tj - 1 == cur_j) break;
            if (vp_int_req_key(t, bus_hdr, &o) && memcmp(&o, &k, sizeof(k)) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) distinct++;
    }
    return distinct;
}

size_t vp_int_validate_resolved(const cJSON *root, const vp_validate_cfg_t *cfg, vp_issues_t *out)
{
    static const char *const root_keys[] = {
        "schema", "id", "rev", "name", "origin", "meta", "aliases", "force", "notes",
        "requires", "bus", "drivetrain", "monitor", "luts", "signals", "defaults", NULL
    };
    vctx_t c;
    const char *s = NULL;
    const cJSON *signals;

    memset(&c, 0, sizeof(c));
    c.cfg = cfg;
    c.out = out;

    if (!cJSON_IsObject(root)) {
        verr(&c, "E_SCHEMA", "$", "profile must be a JSON object");
        return c.errors;
    }
    if (f_str(&c, root, "schema", "", &s) != F_OK || !schema_ok(s)) {
        verr(&c, "E_SCHEMA", "schema", "unsupported schema (want %s%d)", VP_SCHEMA_PREFIX, VP_SCHEMA_MAJOR);
        return c.errors;   /* nothing else is meaningful */
    }
    check_keys(&c, root, "", root_keys);

    s = NULL;
    if (f_str(&c, root, "id", "", &s) == F_ABSENT) {
        verr(&c, "E_SCHEMA", "id", "required");
    } else if (s && !vp_id_ok(s)) {
        verr(&c, "E_NAME", "id", "[a-z0-9._-], 1..%d chars", VP_ID_MAX_LEN);
    }
    if (f_int(&c, root, "rev", "", 1, 65535, "E_SCHEMA", NULL) == F_ABSENT) {
        verr(&c, "E_SCHEMA", "rev", "required");
    }
    s = NULL;
    if (f_str(&c, root, "name", "", &s) == F_ABSENT) {
        verr(&c, "E_SCHEMA", "name", "required");
    } else if (s && !vp_ascii_ok(s, VP_NAME_MAX_LEN)) {
        verr(&c, "E_NAME", "name", "printable ASCII, 1..%d chars", VP_NAME_MAX_LEN);
    }
    f_bool(&c, root, "force", "", NULL);

    {
        static const char *const policies[] = { "obd2", "none", NULL };
        f_enum(&c, root, "defaults", "", policies, 0);
    }
    check_requires(&c, jget(root, "requires"));
    check_bus(&c, jget(root, "bus"));
    c.bus_hdr = vp_int_bus_hdr(root, NULL);
    check_drivetrain(&c, jget(root, "drivetrain"));
    check_monitor(&c, jget(root, "monitor"));
    check_luts(&c, jget(root, "luts"));

    signals = jget(root, "signals");
    if (!cJSON_IsArray(signals) || cJSON_GetArraySize(signals) == 0) {
        verr(&c, "E_SCHEMA", "signals", "required non-empty array");
        return c.errors;
    }
    if (cJSON_GetArraySize(signals) > VP_MAX_SIGNALS) {
        verr(&c, "E_LIMIT", "signals", "%d signals, max %d", cJSON_GetArraySize(signals), VP_MAX_SIGNALS);
        return c.errors;
    }
    {
        const cJSON *sig;
        int i = 0;
        cJSON_ArrayForEach(sig, signals) {
            const char *id = jstr(sig, "id");
            if (cJSON_IsTrue(jget(sig, "disabled"))) continue;
            if (id && !strcmp(id, "obd.rpm") && (jget(sig, "src") || jget(sig, "derive"))) c.has_rpm = true;
            if (id && !strcmp(id, "obd.speed") && (jget(sig, "src") || jget(sig, "derive"))) c.has_speed = true;
        }
        cJSON_ArrayForEach(sig, signals) {
            check_signal(&c, sig, i, signals);
            i++;
        }
    }

    if (c.n_veh > VP_MAX_VEH_SIGNALS) {
        verr(&c, "E_LIMIT", "signals", "%u veh.* signals, max %d", (unsigned)c.n_veh, VP_MAX_VEH_SIGNALS);
    }
    if (c.n_src > VP_MAX_SOURCES) {
        verr(&c, "E_LIMIT", "signals", "%u sources, max %d", (unsigned)c.n_src, VP_MAX_SOURCES);
    }
    if (c.n_can > VP_MAX_CAN_RULES) {
        verr(&c, "E_LIMIT", "signals", "%u can sources, max %d", (unsigned)c.n_can, VP_MAX_CAN_RULES);
    }
    {
        size_t nreq = vp_int_count_requests(signals, c.bus_hdr);
        if (nreq > VP_MAX_REQUESTS) {
            verr(&c, "E_LIMIT", "signals", "%u distinct requests, max %d", (unsigned)nreq, VP_MAX_REQUESTS);
        }
    }
    if (c.n_every > VP_MAX_EVERY_SIGNALS) {
        verr(&c, "E_RATE", "signals", "%u signals with rate \"every\", max %d", (unsigned)c.n_every,
             VP_MAX_EVERY_SIGNALS);
    }
    if (c.rate_sum > (double)VP_MAX_REQ_RATE_HZ) {
        verr(&c, "E_RATE", "signals", "period_ms total %.0f req/s, max %d", c.rate_sum, VP_MAX_REQ_RATE_HZ);
    }
    if (c.has_monitor && c.n_can == 0) {
        vwarn(&c, "W_UNUSED", "monitor", "no can sources");
    }
    if (c.mon_mode == 2 && c.n_req_src > 0) {
        verr(&c, "E_SCHEMA", "monitor.mode", "continuous monitoring leaves no slot for requests");
    }
    return c.errors;
}

size_t vp_validate(const cJSON *root, const vp_validate_cfg_t *cfg, vp_issues_t *out)
{
    cJSON *owned = NULL;
    const cJSON *resolved = vp_int_resolve_defaults(root, &owned, out);
    size_t errors;
    if (!resolved && root) return 1;
    errors = vp_int_validate_resolved(resolved, cfg, out);
    cJSON_Delete(owned);
    return errors;
}

size_t vp_validate_json(const char *text, size_t len, const vp_validate_cfg_t *cfg, vp_issues_t *out)
{
    size_t max = (cfg && cfg->max_json_bytes) ? cfg->max_json_bytes : VP_MAX_JSON_BYTES;
    cJSON *root;
    size_t n;

    if (!text) {
        vp_issue_add(out, "E_SCHEMA", VP_SEV_ERROR, "$", "no data");
        return 1;
    }
    if (len > max) {
        vp_issue_add(out, "E_SIZE", VP_SEV_ERROR, "$", "%u bytes, max %u", (unsigned)len, (unsigned)max);
        return 1;
    }
    root = cJSON_ParseWithLength(text, len);
    if (!root) {
        vp_issue_add(out, "E_SCHEMA", VP_SEV_ERROR, "$", "not valid JSON");
        return 1;
    }
    n = vp_validate(root, cfg, out);
    cJSON_Delete(root);
    return n;
}

/* ------------------------------------------------------------------ */
/* firmware configuration from the generated signal registry           */
/* ------------------------------------------------------------------ */

static vp_sig_def_t s_default_reg[SIG_OBD_COUNT];
static vp_validate_cfg_t s_default_cfg;

static bool default_unit_ok(size_t reg_index, const char *unit)
{
    const sig_def_t *d;
    size_t i;
    if (reg_index >= SIG_OBD_COUNT || !unit) return false;
    d = &sig_registry[SIG_OBD_FIRST + reg_index];
    for (i = 0; i < d->n_units; i++) {
        if (d->units[i].unit && !strcmp(d->units[i].unit, unit)) return true;
    }
    return false;
}

const vp_validate_cfg_t *vp_validate_default_cfg(void)
{
    size_t i;
    if (s_default_cfg.registry) return &s_default_cfg;
    for (i = 0; i < SIG_OBD_COUNT; i++) {
        const sig_def_t *d = &sig_registry[SIG_OBD_FIRST + i];
        vp_sig_def_t *o = &s_default_reg[i];
        o->key = d->key;
        o->num_id = d->num_id;
        o->editor_index = d->editor_index;
        o->n_units = d->n_units;
        o->scale = d->scale;
        o->min = d->min;
        o->max = d->max;
        o->unit_default = (d->n_units && d->units) ? d->units[0].unit : NULL;
    }
    s_default_cfg.n_registry = SIG_OBD_COUNT;
    s_default_cfg.max_json_bytes = VP_MAX_JSON_BYTES;
    s_default_cfg.unit_ok = default_unit_ok;
    s_default_cfg.registry = s_default_reg;
    return &s_default_cfg;
}
