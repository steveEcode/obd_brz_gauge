/*
 * vp_engine - compiled vehicle profile -> ELM327 traffic -> signal values.
 * See vp_engine.h for the behaviour summary; the decode pipeline follows
 * contracts/vehicle-rpn.md section 3 step by step (comments "P1".."P10").
 *
 * Execution model: one command in flight at a time. A command is a step
 * {cmd, kind, delay}; the response is collected until the '>' prompt (or a
 * timeout), handled, then the step's delay runs before the next command.
 * Bring-up, detection and monitor windows queue their steps up front; the
 * polling loop plans one step at a time so adapter state (ATSH / ATCRA /
 * ATCEA / ATCAF) is only changed right before the request that needs it.
 */

#include "vp_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vp_common.h"
#include "vp_validate.h"
#include "app_obd_dsp/signal_registry.h"
#include "app_obd_dsp/signal_store.h"

#define ENG_RX_CAP          5120
#define ENG_SCRATCH_CAP     5120    /* parsed response bytes / reassembled payload */
#define ENG_LINE_CAP        256     /* parsed bytes retained per response line */
#define ENG_MAX_LINES       32
#define ENG_STEPS           40
#define ENG_CMD_CAP         20
#define ENG_ISOTP_TIMEOUT   2500
#define ENG_ISOTP_BLOCK_SIZE 1
#if ENG_CMD_CAP < 14
#error "state commands require prefix + 8 hex digits + NUL"
#endif

#define ENG_DEF_TIMEOUT     2500
#define ENG_DEF_DETECT      2000
#define ENG_SLOW_INIT_TIMEOUT 10000
#define ENG_FAST_INIT_TIMEOUT 5000
#define ENG_DRAIN_TIMEOUT   1500
#define ENG_DEF_LINK_HOLD   3000
#define ENG_DEF_VBAT        5000
#define ENG_DEF_HEAL        10000
#define ENG_HEAL_RECONNECT  3       /* heals in a row before asking for a reconnect */
#define ENG_AT_DELAY        30
#define ENG_ELM_CHECK_DELAY 500
#define ENG_DETECT_SETTLE   200
#define ENG_DETECT_SP_DELAY 50
#define ENG_DETECT_ATTEMPTS 3
#define ENG_WARM_DELAY      150
#define ENG_STOPPED_DELAY   200
#define ENG_STOPPED_RETRIES 1
#define ENG_NISSAN_INIT_TIMEOUT 10000
#define ENG_MA_STOP_MS      300
#define ENG_CONT_WINDOW_MS  5000
#define ENG_IDLE_MS         50
#define ENG_WAIT_CAP        100
#define ENG_DEF_GEAR_TOL    0.15f
#define ENG_RESP_SIGNAL_COUNT 4u
#define ENG_EMA_ALPHA       0.3f
#define ENG_HYST_NONE       (-100)

#define ENG_ID_UNKNOWN      0xFFFFFFFFu   /* hdr / rx not known (never a valid id) */
#define ENG_INDEX_NONE      0xFFu

/* Default candidates for a normal ELM327.  Car Scanner's private protocol
 * arrays are not recoverable with enough certainty from the shipped APK, so
 * this is deliberately a firmware default rather than a claim about the
 * app's exact private ordering.  Callers with stronger vehicle/adapter
 * knowledge can provide an ordered list through vp_engine_cfg_t. */
static const uint8_t k_detect_protocols[] = {
    6, 7, 4, 5, 3, 1, 2, 8, 9, 10, 11
};

#define ENG_MAX_DETECT_PROTOCOLS 16

#if VP_MAX_REQUESTS > 32
#error "poll request mask requires VP_MAX_REQUESTS <= 32"
#endif
#define ENG_CEA_OFF         (-1)
#define ENG_ST_UNKNOWN      (-2)
#define ENG_CEA_OFF_VAL     0x100u        /* step value for "ATCEA" (off) */

enum { K_AT = 0, K_INIT, K_STATE, K_DETECT, K_DRAIN, K_WARM, K_TESTER, K_RV, K_REQ, K_MA,
       K_REQ_AT,
       K_ISO_FC, K_ISO_WAIT, K_DELAY, K_ELM_CHECK, K_PROTOCOL };
enum { ST_HDR = 0, ST_RX, ST_CEA, ST_CAF, ST_CFC };
enum { DONE_PROMPT = 0, DONE_TIMEOUT, DONE_FAIL };

typedef struct {
    char     cmd[ENG_CMD_CAP];
    uint8_t  kind;          /* K_* */
    uint8_t  st;            /* ST_* for K_STATE */
    uint16_t delay_ms;      /* pause after the response */
    uint16_t timeout_ms;    /* 0 = cfg.resp_timeout_ms */
    int16_t  req;           /* request index (K_REQ, K_STATE before a request), else -1 */
    uint8_t  req_at_side;   /* 0 = BeforeCommands, 1 = deferred AfterCommands */
    uint8_t  retry_count;   /* transient STOPPED retry count */
    uint32_t val;           /* state value (K_STATE), protocol (K_DETECT) */
} eng_step_t;

typedef struct {
    /* fixed at create */
    double   factor;        /* P8: RPN result unit -> internal */
    int16_t  slot;          /* signal_store slot, SIG_SLOT_NONE = not driven */
    /* runtime */
    uint8_t  cur;           /* current source (relative to src_first) */
    uint8_t  fails;         /* consecutive failures of the current source */
    bool     exhausted;
    bool     fallback_on;   /* ratio fallback active (obd.gear) */
    bool     can_seen;      /* a frame matched during the current monitor window */
    bool     can_stale;     /* invalidated by monitor.stale_ms */
    bool     filt_init;
    uint8_t  hyst_cnt;
    int16_t  hyst_pending;
    float    filt;
    uint32_t last_poll_ms;
    uint32_t last_can_ms;
} eng_sig_t;

typedef struct {
    float    last;
    uint32_t last_ms;
    uint8_t  held;
    bool     has;
} eng_hold_t;

typedef struct {
    uint16_t off;
    uint8_t  n;
    bool     num;
} eng_line_t;

struct vp_engine {
    const vp_profile_t  *p;
    const vp_request_t  *req;
    const vp_signal_t   *sig;
    const vp_source_t   *src;
    const vp_can_rule_t *can;
    vp_engine_sink_t     sink;
    vp_engine_cfg_t      cfg;
    vp_allocator_t       alloc;

    uint8_t  phase;
    uint8_t  link;
    uint8_t  run_proto;     /* protocol used for ATSP */
    uint8_t  det_proto;     /* detected this session (0 = none) */
    uint8_t  detect_next;   /* current candidate index */
    uint8_t  detect_attempt;
    bool     detect_fallback; /* generic 0100 probe fell back to demanded RPM */
    bool     skip_init_atz; /* adapter check already reset the ELM state */
    bool     mon_ok;
    bool     busy;
    bool     prompt;

    /* in-flight step */
    eng_step_t cur;
    uint32_t   sent_ms;
    bool       ma_stopping;
    bool       ma_refused;
    uint32_t   ma_end_ms;

    /* queued steps (ring) */
    eng_step_t q[ENG_STEPS];
    uint8_t    q_head;
    uint8_t    q_n;
    uint32_t   next_ms;

    /* what the adapter is believed to be set to */
    uint32_t hdr;
    bool     hdr_explicit;
    uint32_t rx;            /* 0 = no filter */
    uint32_t base_rx;       /* filter set by bus.init, restored after rx requests */
    int16_t  cea;           /* ENG_CEA_OFF, 0..255, ENG_ST_UNKNOWN */
    int8_t   caf0;          /* 0 = CAF1, 1 = CAF0, ENG_ST_UNKNOWN */
    int8_t   cfc0;          /* 0 = CFC1, 1 = CFC0, ENG_ST_UNKNOWN */

    /* Manual ISO-TP response reassembly. */
    bool     iso_active;
    uint16_t iso_req;
    uint16_t iso_total;
    uint16_t iso_len;
    uint8_t  iso_next_seq;
    uint8_t  iso_block_count;
    uint8_t  iso_block_size;
    uint16_t iso_stmin_ms;
    uint8_t  iso_payload[ENG_SCRATCH_CAP];

    /* polling rounds */
    uint8_t  round_req[VP_MAX_REQUESTS];
    uint8_t  round_n;
    uint8_t  round_pos;
    bool     in_round;
    bool     every_done;
    uint8_t  slot_req[2 + ENG_RESP_SIGNAL_COUNT + VP_MAX_EVERY_SIGNALS];
    uint8_t  slot_n;
    uint8_t  slot_pos;
    bool     rpm_refreshed;
    int16_t  pending_after_req;
    int16_t  chain_req;
    bool     chain_failed;
    uint8_t  every_sig[VP_MAX_EVERY_SIGNALS];
    uint8_t  n_every;
    uint32_t functional_fallback_mask;
    uint16_t obd_demand_mask;
    bool     req_sent;
    uint32_t round_no;
    uint32_t last_req_ms;
    uint32_t req_last_ms[VP_MAX_REQUESTS];
    uint8_t req_fail_run[VP_MAX_REQUESTS];
    uint32_t req_seen_mask;

    /* Static request -> source index. A response only walks sources that can
     * reference that request, while preserving profile signal/source order. */
    uint8_t  req_head[VP_MAX_REQUESTS];
    uint8_t  req_tail[VP_MAX_REQUESTS];
    uint8_t  req_next[VP_MAX_SOURCES];
    uint8_t  poll_sig[VP_MAX_SIGNALS];
    uint8_t  n_poll;
    uint64_t factor_identity_mask;

    /* timers */
    uint32_t last_data_ms;  /* last decoded write (sys.obd_link 2) */
    uint32_t last_good_ms;  /* last good sample (self-heal) */
    uint32_t last_vbat_ms;
    uint32_t last_mon_ms;
    uint32_t last_tester_ms;
    uint8_t  heal_count;

    /* derived signals */
    int16_t  sig_rpm;
    int16_t  sig_gear;
    int32_t  rpm;
    int32_t  speed;
    int8_t   gear;
    bool     rpm_valid;
    bool     speed_valid;
    bool     gear_valid;

    vp_engine_stats_t st;

    eng_sig_t  sigs[VP_MAX_SIGNALS];
    eng_hold_t holds[VP_MAX_SOURCES];
    uint16_t   rx_len;
    char       rxbuf[ENG_RX_CAP + 1];
    uint8_t    lines[ENG_LINE_CAP];
    uint8_t    payload[ENG_SCRATCH_CAP];
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__)
static void elog(vp_engine_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
static void elog(vp_engine_t *e, const char *fmt, ...)
{
    char b[96];
    va_list ap;

    if (!e->sink.log) return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    e->sink.log(e->sink.ctx, b);
}

static uint32_t cap_wait(uint32_t ms)
{
    if (ms < 1) return 1;
    return ms > ENG_WAIT_CAP ? ENG_WAIT_CAP : ms;
}

static bool elapsed(uint32_t now, uint32_t since, uint32_t ms)
{
    return (uint32_t)(now - since) >= ms;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* State commands have fixed prefixes and at most eight hex digits. */
static const char eng_hex[] = "0123456789ABCDEF";

static void fmt_hex(char *out, uint32_t value, unsigned digits)
{
    out[digits] = '\0';
    while (digits) {
        out[--digits] = eng_hex[value & 0x0Fu];
        value >>= 4;
    }
}

static void fmt_id(char *out, uint32_t id)
{
    if (id & VP_ID_29BIT) fmt_hex(out, id & VP_ID_MASK, 8);
    else fmt_hex(out, id & 0x7FFu, 3);
}

static uint32_t resp_timeout(const vp_engine_t *e)
{
    return e->cfg.resp_timeout_ms ? e->cfg.resp_timeout_ms : ENG_DEF_TIMEOUT;
}

/* Slow K-line initiation alone takes longer than the normal CAN deadline.
 * A caller-supplied detection deadline remains authoritative for tests and
 * adapters with known timing. */
static uint16_t connection_timeout(const vp_engine_t *e)
{
    if (e->cfg.detect_timeout_ms) return e->cfg.detect_timeout_ms;
    if (e->run_proto == 3 || e->run_proto == 4) return ENG_SLOW_INIT_TIMEOUT;
    if (e->run_proto == 5) return ENG_FAST_INIT_TIMEOUT;
    if (e->run_proto == 0) return 30000; /* ELM's complete automatic search */
    return ENG_DEF_DETECT;
}

static bool legacy_obd_protocol(const vp_engine_t *e)
{
    return e->run_proto >= 1 && e->run_proto <= 5;
}

static uint32_t request_header(const vp_engine_t *e, uint32_t hdr)
{
    if (e->run_proto == 7 || e->run_proto == 9) {
        /* Standard OBD addresses in profiles are usually written as 11-bit
         * ids. Retain their meaning on a detected 29-bit ISO 15765 bus. */
        if (hdr == 0x7DFu) return VP_ID_29BIT | 0x18DB33F1u;
        if (hdr >= 0x7E0u && hdr <= 0x7E7u) {
            return VP_ID_29BIT | 0x18DA10F1u | ((hdr - 0x7E0u) << 8);
        }
    }
    return hdr;
}

static uint32_t response_filter(const vp_engine_t *e, uint32_t rx)
{
    if ((e->run_proto == 7 || e->run_proto == 9) && rx >= 0x7E8u && rx <= 0x7EFu) {
        return VP_ID_29BIT | (0x18DAF110u + rx - 0x7E8u);
    }
    return rx;
}

static uint32_t link_hold(const vp_engine_t *e)
{
    return e->cfg.link_hold_ms ? e->cfg.link_hold_ms : ENG_DEF_LINK_HOLD;
}

static uint32_t vbat_every(const vp_engine_t *e)
{
    return e->cfg.vbat_every_ms ? e->cfg.vbat_every_ms : ENG_DEF_VBAT;
}

static uint32_t heal_after(const vp_engine_t *e)
{
    return e->cfg.heal_ms ? e->cfg.heal_ms : ENG_DEF_HEAL;
}

static bool detect_config_valid(const vp_engine_t *e)
{
    size_t n = e->cfg.detect_protocol_count;
    size_t i;

    if (!e->cfg.detect_protocols || n == 0 || n > ENG_MAX_DETECT_PROTOCOLS) return false;
    for (i = 0; i < n; i++) {
        if (e->cfg.detect_protocols[i] == 0 || e->cfg.detect_protocols[i] > 12) return false;
    }
    return true;
}

static size_t detect_count(const vp_engine_t *e)
{
    size_t def = sizeof(k_detect_protocols) / sizeof(k_detect_protocols[0]);

    return detect_config_valid(e) ? e->cfg.detect_protocol_count : def;
}

static uint8_t detect_protocol(const vp_engine_t *e, size_t index)
{
    if (detect_config_valid(e) && index < e->cfg.detect_protocol_count) {
        return e->cfg.detect_protocols[index];
    }
    return k_detect_protocols[index];
}

static bool mon_usable(const vp_engine_t *e)
{
    return e->p->mon_mode != VP_MON_NONE && e->p->n_can > 0 && e->mon_ok;
}

static const vp_source_t *cur_src(const vp_engine_t *e, int i)
{
    const vp_signal_t *s = &e->sig[i];
    if (s->src_count == 0) return NULL;
    return &e->src[s->src_first + e->sigs[i].cur];
}

/* ------------------------------------------------------------------ */
/* sink wrappers                                                       */
/* ------------------------------------------------------------------ */

static void set_link(vp_engine_t *e, uint8_t state)
{
    e->link = state;
    e->sink.on_link(e->sink.ctx, state);
}

static void put(vp_engine_t *e, int slot, int32_t v, uint32_t now)
{
    if (e->link != SIG_LINK_DATA) set_link(e, SIG_LINK_DATA);
    e->last_data_ms = now;
    if (slot == SIG_OBD_SPEED) {
        int32_t k = v < 0 ? 0 : (v > 255 ? 255 : v);
        e->sink.set_speed_kmh(e->sink.ctx, (uint8_t)k);
    } else {
        e->sink.set_signal(e->sink.ctx, slot, v);
    }
}

/* ------------------------------------------------------------------ */
/* step queue                                                          */
/* ------------------------------------------------------------------ */

static void q_clear(vp_engine_t *e)
{
    e->q_head = 0;
    e->q_n = 0;
}

static eng_step_t *q_push(vp_engine_t *e, uint8_t kind, const char *cmd, uint16_t delay_ms)
{
    eng_step_t *s;

    if (e->q_n >= ENG_STEPS) {
        elog(e, "step queue full, dropped %s", cmd);
        return NULL;
    }
    s = &e->q[(e->q_head + e->q_n) % ENG_STEPS];
    memset(s, 0, sizeof(*s));
    /* The zeroed tail supplies the terminator, even for truncated commands. */
    for (size_t i = 0; i < sizeof(s->cmd) - 1 && cmd[i]; i++) {
        s->cmd[i] = cmd[i];
    }
    s->kind = kind;
    s->delay_ms = delay_ms;
    s->req = -1;
    e->q_n++;
    return s;
}

/* Insert an urgent continuation before steps already queued after the
 * current command. ISO-TP flow control must run before restoring adapter
 * framing or sending the next poll request. */
static eng_step_t *q_push_front(vp_engine_t *e, uint8_t kind, const char *cmd,
                                uint16_t delay_ms)
{
    eng_step_t *s;

    if (e->q_n >= ENG_STEPS) {
        elog(e, "step queue full, dropped %s", cmd);
        return NULL;
    }
    e->q_head = (uint8_t)((e->q_head + ENG_STEPS - 1) % ENG_STEPS);
    s = &e->q[e->q_head];
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < sizeof(s->cmd) - 1 && cmd[i]; i++) {
        s->cmd[i] = cmd[i];
    }
    s->kind = kind;
    s->delay_ms = delay_ms;
    s->req = -1;
    e->q_n++;
    return s;
}

static eng_step_t *push_init(vp_engine_t *e, const char *cmd, uint16_t delay_ms)
{
    const char *queued = cmd;
    eng_step_t *s;
    char selected_proto[8];

    if (vp_kwp_init_header(cmd) && e->run_proto != 4 && e->run_proto != 5) return NULL;
    if (legacy_obd_protocol(e) && !strncmp(cmd, "ATSH", 4) &&
        !(vp_kwp_init_header(cmd) && (e->run_proto == 4 || e->run_proto == 5) &&
          (e->p->proto == 4 || e->p->proto == 5))) return NULL;
    /* ATSP0 in a translated profile means automatic protocol selection.
     * Keep a concrete protocol already selected by the detector/user instead
     * of undoing it during the second bring-up. */
    if (!strcmp(cmd, "ATSP0") && e->run_proto != 0) {
        snprintf(selected_proto, sizeof(selected_proto), "ATSP%X", (unsigned)e->run_proto);
        queued = selected_proto;
    }
    /* Legacy OBD response headers are not CAN address headers. Hide them in the
     * adapter so response matching sees the OBD payload directly. */
    if (legacy_obd_protocol(e) && !strcmp(cmd, "ATH1")) {
        queued = "ATH0";
    }
    s = q_push(e, K_INIT, queued, delay_ms);

    if (s && vp_init_raw_command(cmd)) s->timeout_ms = ENG_NISSAN_INIT_TIMEOUT;
    return s;
}

static void q_pop(vp_engine_t *e, eng_step_t *out)
{
    *out = e->q[e->q_head];
    e->q_head = (uint8_t)((e->q_head + 1) % ENG_STEPS);
    e->q_n--;
}

static void push_state(vp_engine_t *e, uint8_t st, uint32_t val, int16_t req)
{
    char c[ENG_CMD_CAP];
    eng_step_t *s;

    /* Fixed byte stores avoid out-of-line memcpy under IDF compiler flags.
     * Keep constant-only snprintf calls: the compiler folds them to stores. */
    switch (st) {
    case ST_HDR:
        c[0] = 'A'; c[1] = 'T';
        c[2] = 'S'; c[3] = 'H';
        fmt_id(c + 4, val);
        break;
    case ST_RX:
        if (val == 0) snprintf(c, sizeof(c), "ATCRA");
        else {
            c[0] = 'A'; c[1] = 'T';
            c[2] = 'C'; c[3] = 'R'; c[4] = 'A';
            fmt_id(c + 5, val);
        }
        break;
    case ST_CEA:
        if (val == ENG_CEA_OFF_VAL) snprintf(c, sizeof(c), "ATCEA");
        else {
            c[0] = 'A'; c[1] = 'T';
            c[2] = 'C'; c[3] = 'E'; c[4] = 'A';
            fmt_hex(c + 5, val & 0xFFu, 2);
        }
        break;
    case ST_CFC:
        c[0] = 'A'; c[1] = 'T'; c[2] = 'C'; c[3] = 'F'; c[4] = 'C';
        c[5] = val ? '0' : '1'; c[6] = '\0';
        break;
    default:
        c[0] = 'A'; c[1] = 'T';
        c[2] = 'C'; c[3] = 'A'; c[4] = 'F';
        c[5] = val ? '0' : '1'; c[6] = '\0';
        break;
    }
    s = q_push(e, K_STATE, c, 0);
    if (!s) return;
    s->st = st;
    s->val = val;
    s->req = req;
}

/* ------------------------------------------------------------------ */
/* response parsing                                                    */
/* ------------------------------------------------------------------ */

/*
 * One response line as bytes: 2-digit tokens (or compact even-length hex),
 * optional "N:" line number, and an optional leading CAN header emitted by
 * ATH1, including ATS0 compact ids and 29-bit ids printed as four bytes.
 * Returns 1 with *n bytes, 0 when the line is not a byte line (text such as
 * NO DATA, a 3-digit byte count), -1 when the scratch buffer is full.
 */
static int parse_byte_line(const char *s, size_t len, uint8_t *out, size_t cap, size_t *n,
                           bool *numbered)
{
    size_t i = 0, cnt = 0, j;
    bool header_consumed = false;

    *numbered = false;
    *n = 0;
    while (i < len && s[i] == ' ') i++;
    j = i;
    while (j < len && j - i < 3 && hexval(s[j]) >= 0) j++;
    if (j < len && s[j] == ':' && j > i && j - i <= 2) {
        *numbered = true;
        i = j + 1;
    }
    if (!*numbered) {
        char id[9];
        size_t pos = i, digits = 0;
        uint32_t rx_id;
        /* ELM prints 29-bit OBD response ids either as four bytes or one
         * token; ATS0 also joins the id directly to the ISO-TP data. The
         * diagnostic response range is unambiguous with headerless payloads. */
        while (pos < len && digits < 8) {
            if (s[pos] == ' ') { pos++; continue; }
            if (hexval(s[pos]) < 0) break;
            id[digits++] = s[pos++];
        }
        id[digits] = '\0';
        if (digits == 8 && pos < len && vp_parse_rx_filter(id, &rx_id) &&
            (rx_id & VP_ID_29BIT)) {
            i = pos;
            header_consumed = true;
        }
    }
    bool first_token = !header_consumed;
    while (i < len) {
        size_t t0, tl, k;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        t0 = i;
        while (i < len && s[i] != ' ') i++;
        tl = i - t0;
        if (first_token && !*numbered && tl > 3 && (tl & 1u)) {
            unsigned id = 0;
            for (k = 0; k < 3; k++) {
                int h = hexval(s[t0 + k]);
                if (h < 0) return 0;
                id = (id << 4) | (unsigned)h;
            }
            if (id > 0x7FFu) return 0;
            /* A compact 11-bit id has three nibbles, so its length cannot
             * be confused with a headerless sequence of complete bytes. */
            t0 += 3;
            tl -= 3;
            first_token = false;
        }
        if (first_token && !*numbered && (tl == 3 || tl == 8)) {
            size_t next = i;
            bool header = true;
            uint32_t id = 0;
            while (next < len && s[next] == ' ') next++;
            if (next < len) {
                for (k = 0; k < tl; k++) {
                    int h = hexval(s[t0 + k]);
                    if (h < 0) {
                        header = false;
                        break;
                    }
                    id = (id << 4) | (uint32_t)h;
                }
                if (id > (tl == 3 ? 0x7FFu : VP_ID_MASK)) header = false;
                if (header) {
                    /* A header is only unambiguous when payload follows it;
                     * compact response bytes such as 410C0BB8 remain intact. */
                    i = next;
                    first_token = false;
                    continue;
                }
            }
        }
        first_token = false;
        if (tl & 1u) return 0;
        /* Decode each nibble once. ELM responses commonly use compact
         * strings (e.g. 410C0BB8), so this removes a second full token scan. */
        for (k = 0; k < tl; k += 2) {
            int hi = hexval(s[t0 + k]);
            int lo = hexval(s[t0 + k + 1]);
            if (hi < 0 || lo < 0) return 0;
            if (cnt >= cap) return -1;
            out[cnt++] = (uint8_t)((hi << 4) | lo);
        }
    }
    *n = cnt;
    return cnt > 0 ? 1 : 0;
}

static bool try_match(const uint8_t *b, size_t len, const uint8_t *exp, size_t elen,
                      const uint8_t **d, size_t *n)
{
    if (len < elen || memcmp(b, exp, elen) != 0) return false;
    *d = b + elen;
    *n = len - elen;
    return true;
}

/*
 * Find the positive response for expect[] in the collected text. The data
 * after the expect prefix is returned through d/n (points into e->lines or
 * e->payload). Accepted layouts, first match wins (several ECUs may answer):
 *   - ELM numbered multi-frame lines ("0: .." "1: ..") concatenated;
 *   - raw ISO-TP first frame "1L LL .." + "2N .." continuation lines (CAF0
 *     and some clones);
 *   - a single line with the prefix at byte 0, after a single-frame PCI byte
 *     (CAF0), or after an extended address + PCI byte.
 */
static bool resp_match(vp_engine_t *e, const uint8_t *exp, size_t elen, const uint8_t **d,
                       size_t *n)
{
    eng_line_t ln[ENG_MAX_LINES];
    size_t nl = 0, used = 0, k, t;
    const char *s = e->rxbuf;
    const char *end = e->rxbuf + e->rx_len;
    bool any_num = false;

    while (s < end && nl < ENG_MAX_LINES) {
        const char *le = s;
        while (le < end && *le != '\r' && *le != '\n' && *le != '>') le++;
        if (le > s) {
            size_t got = 0;
            bool num = false;
            int r = parse_byte_line(s, (size_t)(le - s), e->lines + used,
                                    sizeof(e->lines) - used, &got, &num);
            if (r > 0) {
                ln[nl].off = (uint16_t)used;
                ln[nl].n = (uint8_t)(got > 255 ? 255 : got);
                ln[nl].num = num;
                any_num |= num;
                used += got;
                nl++;
            }
        }
        s = le + 1;
    }

    if (any_num) {
        t = 0;
        for (k = 0; k < nl; k++) {
            size_t c;
            if (!ln[k].num) continue;
            c = ln[k].n;
            if (t + c > sizeof(e->payload)) c = sizeof(e->payload) - t;
            memcpy(e->payload + t, e->lines + ln[k].off, c);
            t += c;
        }
        if (try_match(e->payload, t, exp, elen, d, n)) return true;
    }

    for (k = 0; k < nl; k++) {
        const uint8_t *L = e->lines + ln[k].off;
        size_t m = ln[k].n;

        if (ln[k].num) continue;
        if (m >= 3 && (L[0] & 0xF0u) == 0x10u) {
            size_t j;
            t = m - 2;
            memcpy(e->payload, L + 2, t);
            for (j = k + 1; j < nl; j++) {
                const uint8_t *C = e->lines + ln[j].off;
                size_t c = ln[j].n;
                if (ln[j].num || c < 2 || (C[0] & 0xF0u) != 0x20u) break;
                c -= 1;
                if (t + c > sizeof(e->payload)) c = sizeof(e->payload) - t;
                memcpy(e->payload + t, C + 1, c);
                t += c;
            }
            if (try_match(e->payload, t, exp, elen, d, n)) return true;
        }
        if (try_match(L, m, exp, elen, d, n)) return true;
        if (m >= 2 && L[0] != 0 && (L[0] & 0xF0u) == 0) {
            size_t pl = L[0] < m - 1 ? L[0] : m - 1;
            if (try_match(L + 1, pl, exp, elen, d, n)) return true;
        }
        if (m >= 3 && L[1] != 0 && (L[1] & 0xF0u) == 0) {
            size_t pl = L[1] < m - 2 ? L[1] : m - 2;
            if (try_match(L + 2, pl, exp, elen, d, n)) return true;
        }
    }
    return false;
}

/* Parse the current ELM response into byte lines.  The line scratch remains
 * bounded because a CAN line is at most one frame; the assembled payload has
 * the larger ISO-TP capacity above. */
static size_t response_lines(vp_engine_t *e, eng_line_t *ln, size_t cap, bool *any_num)
{
    size_t nl = 0, used = 0;
    const char *s = e->rxbuf;
    const char *end = e->rxbuf + e->rx_len;

    if (any_num) *any_num = false;
    while (s < end && nl < cap) {
        const char *le = s;
        size_t got = 0;
        bool num = false;
        int r;

        while (le < end && *le != '\r' && *le != '\n' && *le != '>') le++;
        if (le > s) {
            r = parse_byte_line(s, (size_t)(le - s), e->lines + used,
                                sizeof(e->lines) - used, &got, &num);
            if (r > 0) {
                ln[nl].off = (uint16_t)used;
                ln[nl].n = (uint8_t)(got > 255 ? 255 : got);
                ln[nl].num = num;
                if (any_num) *any_num |= num;
                used += got;
                nl++;
            }
        }
        s = le + 1;
        if (used >= sizeof(e->lines)) break;
    }
    return nl;
}

static bool resp_mode01_unsupported(vp_engine_t *e, uint8_t service)
{
    eng_line_t ln[ENG_MAX_LINES];
    size_t nl, i, j;

    if (service != 0x01u) return false;
    nl = response_lines(e, ln, ENG_MAX_LINES, NULL);
    for (i = 0; i < nl; i++) {
        const uint8_t *b = e->lines + ln[i].off;
        for (j = 0; j + 2 < ln[i].n; j++) {
            if (b[j] == 0x7Fu && b[j + 1] == service &&
                (b[j + 2] == 0x11u || b[j + 2] == 0x12u)) return true;
        }
    }
    return false;
}

static void iso_reset(vp_engine_t *e)
{
    e->iso_active = false;
    e->iso_req = 0;
    e->iso_total = 0;
    e->iso_len = 0;
    e->iso_next_seq = 1;
    e->iso_block_count = 0;
    e->iso_block_size = ENG_ISOTP_BLOCK_SIZE;
    e->iso_stmin_ms = 0;
}

/* A few adapters expose the ECU's ISO-TP flow-control frame while manual
 * framing is active.  It is not needed for the normal fixed 30 08 00 path,
 * but retaining it lets the receiver honor a smaller block size and keeps
 * malformed flow-control records fail-closed. */
static bool iso_flow_control(const vp_request_t *q, const uint8_t *frame, size_t n,
                             uint8_t *block_size, uint16_t *stmin_ms)
{
    size_t pci = 0;
    uint8_t st;

    if ((q->flags & VP_REQ_F_CEA) && n > 0 && frame[0] == q->cea) pci = 1;
    if (pci + 2 >= n || (frame[pci] >> 4) != 3) return false;
    st = frame[pci + 2];
    *block_size = frame[pci + 1];
    if (st <= 0x7Fu) *stmin_ms = st;
    else if (st >= 0xF1u && st <= 0xF9u) *stmin_ms = 1;
    else return false;
    return true;
}

/* Append one ISO-TP frame.  Returns false for malformed or out-of-order
 * frames.  The first frame establishes the total application payload size;
 * the data presented to vp_profile is the reassembled positive response,
 * without PCI or extended-address bytes. */
static bool iso_append_frame(vp_engine_t *e, const vp_request_t *q,
                             const uint8_t *frame, size_t n, bool first)
{
    size_t pci = 0, data = 0, take = 0;
    uint8_t type;
    uint16_t total;

    if ((q->flags & VP_REQ_F_CEA) && n > 0 && frame[0] == q->cea) pci = 1;
    if (pci >= n) return false;
    type = (uint8_t)(frame[pci] >> 4);
    if (first) {
        iso_reset(e);
        if (type == 0) {
            total = (uint16_t)(frame[pci] & 0x0Fu);
            data = pci + 1;
            e->iso_next_seq = 1;
        } else if (type == 1 && pci + 1 < n) {
            total = (uint16_t)(((frame[pci] & 0x0Fu) << 8) | frame[pci + 1]);
            data = pci + 2;
            e->iso_next_seq = 1;
        } else {
            return false;
        }
        if (total == 0 || total > sizeof(e->iso_payload) || data > n) return false;
        e->iso_total = total;
        e->iso_active = type == 1 && total > n - data;
    } else {
        if (!e->iso_active || type != 2 || (frame[pci] & 0x0Fu) != e->iso_next_seq) {
            return false;
        }
        data = pci + 1;
        if (data > n) return false;
    }
    take = n - data;
    if (take > (size_t)e->iso_total - e->iso_len) take = (size_t)e->iso_total - e->iso_len;
    if (take) memcpy(e->iso_payload + e->iso_len, frame + data, take);
    e->iso_len = (uint16_t)(e->iso_len + take);
    if (type == 2) {
        e->iso_next_seq = (uint8_t)((e->iso_next_seq + 1u) & 0x0Fu);
        e->iso_block_count++;
    }
    if (e->iso_len >= e->iso_total) e->iso_active = false;
    return true;
}

/* Consume manual ISO-TP frames in the current response.  A normal ELM
 * response returns false so the legacy matcher remains the fallback for
 * numbered/CAF1 layouts. */
static bool iso_consume(vp_engine_t *e, const vp_request_t *q, bool first,
                        bool *complete, bool *need_fc)
{
    eng_line_t ln[ENG_MAX_LINES];
    size_t nl, k;
    bool any_num;
    bool saw = false;

    *complete = false;
    *need_fc = false;
    nl = response_lines(e, ln, ENG_MAX_LINES, &any_num);
    if (any_num) return false;
    for (k = 0; k < nl; k++) {
        const uint8_t *frame = e->lines + ln[k].off;
        size_t n = ln[k].n;
        uint8_t type;
        size_t pci = ((q->flags & VP_REQ_F_CEA) && n > 0 && frame[0] == q->cea) ? 1 : 0;
        uint8_t block_size;
        uint16_t stmin_ms;

        if (pci >= n) continue;
        type = (uint8_t)(frame[pci] >> 4);
        if (type == 3) {
            if (!iso_flow_control(q, frame, n, &block_size, &stmin_ms)) return false;
            e->iso_block_size = block_size;
            e->iso_stmin_ms = stmin_ms;
            continue;
        }
        if (first && !saw) {
            if (type != 0 && type != 1) continue;
            if (!iso_append_frame(e, q, frame, n, true)) return false;
            saw = true;
        } else if (!first && !saw) {
            if (type != 2) continue;
            if (!iso_append_frame(e, q, frame, n, false)) return false;
            saw = true;
        } else if (type == 2) {
            if (!iso_append_frame(e, q, frame, n, false)) return false;
        }
        if (e->iso_len >= e->iso_total) {
            *complete = true;
            break;
        }
    }
    if (!saw) return false;
    *need_fc = !*complete && e->iso_active && e->iso_block_size != 0 &&
               e->iso_block_count >= e->iso_block_size;
    return true;
}

typedef struct {
    const char *s;
    uint8_t     n;
    uint8_t     hex;
    uint32_t    value;
} eng_tok_t;

/* Space-separated tokens; returns max + 1 when there are more than max.
 * Decode tokens while finding their boundaries: monitor lines otherwise
 * validate every token and then convert the same characters again. */
static size_t tokenize(const char *s, size_t len, eng_tok_t *t, size_t max)
{
    size_t i = 0, nt = 0;
    while (i < len) {
        size_t t0, digits = 0;
        uint32_t value = 0;
        uint8_t hex = 1;
        while (i < len && s[i] == ' ') i++;
        if (i >= len) break;
        t0 = i;
        while (i < len && s[i] != ' ') {
            int h = hexval(s[i]);
            if (h < 0) {
                hex = 0;
            } else if (digits < 8) {
                value = (value << 4) | (uint32_t)h;
            }
            digits++;
            i++;
        }
        if (nt >= max) return max + 1;
        t[nt].s = s + t0;
        t[nt].n = (uint8_t)(digits > 255 ? 255 : digits);
        t[nt].hex = hex;
        t[nt].value = value;
        nt++;
    }
    return nt;
}

/*
 * One ATMA line (ATH1, ATS1): "360 00 11 22 ..", "18DAF110 03 ..",
 * "18 DA F1 10 03 ..", optionally "N:"-prefixed or with a DLC digit.
 */
static bool parse_can_line(const char *s, size_t len, uint32_t *id, uint8_t *data, size_t *n)
{
    eng_tok_t t[16];
    size_t nt = tokenize(s, len, t, 16), idx = 0, k;

    if (nt == 0 || nt > 16) return false;
    if (t[0].n >= 2 && t[0].s[t[0].n - 1] == ':') idx = 1;
    for (k = idx; k < nt; k++) {
        if (!t[k].hex || t[k].n == 0) return false;
    }
    if (idx >= nt) return false;
    if (t[idx].n == 3) {
        *id = t[idx].value;
        if (*id > 0x7FFu) return false;
        idx++;
    } else if (t[idx].n == 8) {
        uint32_t v = t[idx].value;
        if (v > VP_ID_MASK) return false;
        *id = v | VP_ID_29BIT;
        idx++;
    } else if (t[idx].n == 2 && nt - idx >= 5 && t[idx + 1].n == 2 && t[idx + 2].n == 2 &&
               t[idx + 3].n == 2) {
        uint32_t v = (t[idx].value << 24) | (t[idx + 1].value << 16) |
                     (t[idx + 2].value << 8) | t[idx + 3].value;
        if (v > VP_ID_MASK) return false;
        *id = v | VP_ID_29BIT;
        idx += 4;
    } else {
        return false;
    }
    if (idx < nt && t[idx].n == 1) {
        idx++;
    } else if (nt - idx == 9 && t[idx].n == 2 && t[idx].value <= 8) {
        idx++;
    }
    if (idx >= nt || nt - idx > 8) return false;
    *n = 0;
    for (k = idx; k < nt; k++) {
        if (t[k].n != 2) return false;
        data[(*n)++] = (uint8_t)t[k].value;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* gear                                                                */
/* ------------------------------------------------------------------ */

static bool gear_band(const vp_profile_t *p, int g, float total, float tol)
{
    float r = p->gears[g - 1] * p->final_drive;
    return total >= r * (1.0f - tol) && total <= r * (1.0f + tol);
}

int8_t vp_engine_gear_from_ratio(const vp_profile_t *p, float rpm, float speed_kmh, int8_t cur)
{
    float total, tol;
    int g;

    if (!p || p->gear_count == 0 || p->final_drive <= 0.0f || p->tire_m <= 0.0f) return cur;
    if (rpm <= 0.0f || speed_kmh <= 0.0f) return 0;
    total = rpm * 0.377f * p->tire_m / speed_kmh;
    tol = p->gear_tol > 0.0f ? p->gear_tol : ENG_DEF_GEAR_TOL;
    if (cur >= 1 && cur <= p->gear_count && gear_band(p, cur, total, tol)) return cur;
    for (g = 1; g <= p->gear_count; g++) {
        if (gear_band(p, g, total, tol)) return (int8_t)g;
    }
    if (rpm > 800.0f && speed_kmh < 5.0f) return 0;
    return cur;
}

static void gear_update(vp_engine_t *e, uint32_t now)
{
    const vp_profile_t *p = e->p;
    const vp_signal_t *s;
    eng_sig_t *g;
    int8_t gear;

    if (e->sig_gear < 0) return;
    s = &e->sig[e->sig_gear];
    g = &e->sigs[e->sig_gear];
    if (g->slot == SIG_SLOT_NONE) return;
    if (!(s->derive == VP_DERIVE_RATIO || (s->fallback == VP_DERIVE_RATIO && g->fallback_on))) return;
    if (!e->rpm_valid || !e->speed_valid || p->gear_count == 0 || p->final_drive <= 0.0f ||
        p->tire_m <= 0.0f) {
        if (e->gear_valid) {
            e->sink.invalidate(e->sink.ctx, g->slot);
            e->gear_valid = false;
        }
        return;
    }
    gear = vp_engine_gear_from_ratio(p, (float)e->rpm, (float)e->speed,
                                     e->gear_valid ? e->gear : 0);
    e->gear = gear;
    e->gear_valid = true;
    put(e, g->slot, gear, now);
}

/* ------------------------------------------------------------------ */
/* signal pipeline                                                     */
/* ------------------------------------------------------------------ */

static void sig_invalidate(vp_engine_t *e, int i, uint32_t now)
{
    int slot = e->sigs[i].slot;

    if (slot == SIG_SLOT_NONE) return;
    e->sink.invalidate(e->sink.ctx, slot);
    if (slot == SIG_OBD_RPM) {
        e->rpm_valid = false;
        gear_update(e, now);
    } else if (slot == SIG_OBD_SPEED) {
        e->speed_valid = false;
        gear_update(e, now);
    } else if (slot == SIG_OBD_GEAR) {
        e->gear_valid = false;
    }
}

static void store(vp_engine_t *e, int i, int32_t v, uint32_t now)
{
    int slot = e->sigs[i].slot;

    e->sigs[i].can_stale = false;
    put(e, slot, v, now);
    if (slot == SIG_OBD_RPM) {
        e->rpm = v;
        e->rpm_valid = true;
        gear_update(e, now);
    } else if (slot == SIG_OBD_SPEED) {
        e->speed = v;
        e->speed_valid = true;
        gear_update(e, now);
    } else if (slot == SIG_OBD_GEAR) {
        e->gear = (int8_t)v;
        e->gear_valid = true;
    }
}

/* Filters use float state but the signal pipeline stores int32. Avoid
 * truncation toward zero for negative temperatures and saturate the rare
 * int32 endpoint that rounds to 2^31 in float representation. */
static bool round_identity(float v, int32_t *out);

static int32_t filter_round(float f)
{
    int32_t rounded;
    if (round_identity(f, &rounded)) return rounded;
    return f < 0.0f ? INT32_MIN : INT32_MAX;
}

/* Legacy oil temperature hysteresis (elm327_ble_client.c default_on_parsed_oil_temp). */
static bool filter_temp_hyst(eng_sig_t *g, int32_t in, bool from_can, int32_t *out)
{
    float fd;
    float diff;

    if (in < -20 || in > 255) return false;
    if (from_can) {
        g->filt = (float)in;
        g->filt_init = true;
        g->hyst_pending = (int16_t)in;
        g->hyst_cnt = 0;
        *out = in;
        return true;
    }
    if (!g->filt_init) {
        g->filt = (float)in;
        g->filt_init = true;
        g->hyst_pending = (int16_t)in;
        g->hyst_cnt = 1;
        *out = in;
        return true;
    }
    fd = g->filt - (float)in;
    diff = fd >= 0.0f ? fd : -fd;
    if (diff < 6.0f) {
        g->hyst_pending = ENG_HYST_NONE;
        g->hyst_cnt = 0;
        g->filt = 0.65f * g->filt + 0.35f * (float)in;
        *out = filter_round(g->filt);
        return true;
    }
    {
        bool medium = diff < 20.0f;
        uint8_t need = medium ? 4 : 3;
        if (g->hyst_pending == in) {
            g->hyst_cnt++;
        } else {
            g->hyst_pending = (int16_t)in;
            g->hyst_cnt = 1;
            return false;
        }
        if (g->hyst_cnt < need) return false;
        g->filt = medium ? 0.5f * g->filt + 0.5f * (float)in : (float)in;
        g->hyst_pending = ENG_HYST_NONE;
        g->hyst_cnt = 0;
        *out = medium ? filter_round(g->filt) : in;
        return true;
    }
}

static int32_t filter_ema(eng_sig_t *g, int32_t in)
{
    if (!g->filt_init) {
        g->filt = (float)in;
        g->filt_init = true;
    } else {
        g->filt += ENG_EMA_ALPHA * ((float)in - g->filt);
    }
    return (int32_t)floorf(g->filt + 0.5f);
}

/* Round Binary32 like floor((double)v + 0.5), using hardware integer/float
 * conversions. Guard the cast first; fractional inputs are too small for the
 * final +/-1 to overflow. Negative half ties round toward positive infinity. */
static bool round_identity(float v, int32_t *out)
{
    int32_t integer;
    float fraction;
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return false;
    integer = (int32_t)v;
    fraction = v - (float)integer;
    if (fraction >= 0.5f) integer++;
    else if (fraction < -0.5f) integer--;
    *out = integer;
    return true;
}

/* P6..P10 for a source value v (P1..P5 done by the caller). */
static void write_value(vp_engine_t *e, int i, float v, uint32_t now, bool from_can)
{
    const vp_signal_t *s = &e->sig[i];
    eng_sig_t *g = &e->sigs[i];
    double x;
    int32_t internal;

    if (g->slot == SIG_OBD_SPEED && e->p->speed_scale > 0.0f) v *= e->p->speed_scale;
    /* P6 post: clamp -> zero_if_rpm_lt -> zero_le */
    if (s->flags & VP_SIG_F_CLAMP) {
        if (v < s->clamp_lo) v = s->clamp_lo;
        if (v > s->clamp_hi) v = s->clamp_hi;
    }
    if (s->flags & VP_SIG_F_ZERO_RPM) {
        int32_t rpm = e->rpm_valid ? e->rpm : 0;
        if (rpm < (int32_t)s->zero_if_rpm_lt) v = 0.0f;
    }
    if ((s->flags & VP_SIG_F_ZERO_LE) && v <= s->zero_le) v = 0.0f;
    /* P7 signal valid */
    if ((s->flags & VP_SIG_F_VALID) && (v < s->valid_lo || v > s->valid_hi)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P8 internal units. Most standard signals use an identity factor; keep
     * that hot path in single precision and avoid the Xtensa double helpers.
     * The general path preserves the profile's double-precision rounding. */
    if (e->factor_identity_mask & (UINT64_C(1) << (unsigned)i)) {
        if (!round_identity(v, &internal)) {
            sig_invalidate(e, i, now);
            return;
        }
    } else {
        x = floor((double)v * g->factor + 0.5);
        if (!(x >= -2147483648.0 && x <= 2147483647.0)) {
            sig_invalidate(e, i, now);
            return;
        }
        internal = (int32_t)x;
    }
    /* P9 registry range (veh.* ranges are enforced by signal_store) */
    if (g->slot < SIG_OBD_COUNT &&
        (internal < sig_registry[g->slot].min || internal > sig_registry[g->slot].max)) {
        sig_invalidate(e, i, now);
        return;
    }
    /* P10 filter + write */
    if (s->filter == VP_FILTER_TEMP_HYST) {
        if (!filter_temp_hyst(g, internal, from_can, &internal)) return;
    } else if (s->filter == VP_FILTER_EMA) {
        internal = filter_ema(g, internal);
    }
    store(e, i, internal, now);
}

static void source_exhausted(vp_engine_t *e, int i, uint32_t now)
{
    e->sigs[i].exhausted = true;
    if (i == e->sig_gear && e->sig[i].fallback == VP_DERIVE_RATIO) {
        e->sigs[i].fallback_on = true;
        gear_update(e, now);
        return;
    }
    sig_invalidate(e, i, now);
}

/* Move to the next usable source (CAN sources are skipped without a monitor). */
static void advance_source(vp_engine_t *e, int i, uint32_t now)
{
    const vp_signal_t *s = &e->sig[i];
    eng_sig_t *g = &e->sigs[i];
    bool wrapped = false;
    uint8_t k;

    g->fails = 0;
    for (k = 0; k < s->src_count; k++) {
        g->cur++;
        if (g->cur >= s->src_count) {
            g->cur = 0;
            wrapped = true;
        }
        if (e->src[s->src_first + g->cur].kind != VP_SRC_CAN || mon_usable(e)) break;
    }
    if (wrapped) source_exhausted(e, i, now);
}

/* P5: one failed sample of signal i's current source. */
static void sig_fail(vp_engine_t *e, int i, uint32_t now)
{
    const vp_source_t *s = cur_src(e, i);
    eng_sig_t *g = &e->sigs[i];
    uint8_t fa;

    if (!s) return;
    fa = s->fail_after ? s->fail_after : VP_DEFAULT_FAIL_AFTER;
    if (++g->fails < fa) return;
    advance_source(e, i, now);
}

/* P1 len check + P2 RPN + P3 source valid. */
static bool src_eval(vp_engine_t *e, uint16_t si, const uint8_t *d, size_t n, float *v)
{
    const vp_source_t *s = &e->src[si];

    if (s->kind == VP_SRC_REQ && n < s->len) return false;
    return vp_profile_eval_source(e->p, si, d, n, v) == VP_RPN_OK;
}

/* P4 hold. */
static float apply_hold(vp_engine_t *e, uint16_t si, float v, uint32_t now)
{
    const vp_source_t *s = &e->src[si];
    eng_hold_t *h = &e->holds[si];

    if (!(s->flags & VP_SRC_F_HOLD)) return v;
    if (!h->has || (uint32_t)(now - h->last_ms) > s->hold_gap_ms ||
        fabsf(v - h->last) <= s->hold_delta) {
        h->has = true;
        h->last = v;
        h->last_ms = now;
        h->held = 0;
        return v;
    }
    if (h->held < s->hold_max) {
        h->held++;
        return h->last;
    }
    h->last = v;
    h->last_ms = now;
    h->held = 0;
    return v;
}

static void sample_ok(vp_engine_t *e, int i, uint16_t si, float v, uint32_t now, bool from_can)
{
    eng_sig_t *g = &e->sigs[i];

    v = apply_hold(e, si, v, now);
    g->fails = 0;
    g->exhausted = false;
    g->fallback_on = false;
    e->last_good_ms = now;
    e->heal_count = 0;
    if (e->sink.on_response) e->sink.on_response(e->sink.ctx, g->slot);
    write_value(e, i, v, now, from_can);
}

/* ------------------------------------------------------------------ */
/* request / frame handling                                            */
/* ------------------------------------------------------------------ */

static bool sig_driven(const vp_engine_t *e, int i)
{
    return e->sigs[i].slot != SIG_SLOT_NONE && e->sig[i].derive == VP_DERIVE_NONE &&
           e->sig[i].src_count > 0;
}

static bool signal_demanded(const vp_engine_t *e, int i)
{
    int slot;

    if (!sig_driven(e, i)) return false;
    slot = e->sigs[i].slot;
    if (slot >= SIG_VEH_BASE) return true;
    if (slot < SIG_OBD_FIRST || slot >= SIG_OBD_FIRST + SIG_OBD_COUNT) return false;
    if (e->obd_demand_mask & (UINT16_C(1) << (unsigned)slot)) return true;

    /* Ratio-derived gear needs speed in addition to the always-on RPM. */
    if (slot == SIG_OBD_SPEED && e->sig_gear >= 0 &&
        (e->obd_demand_mask & (UINT16_C(1) << SIG_OBD_GEAR))) {
        const vp_signal_t *gear = &e->sig[e->sig_gear];
        if (gear->derive == VP_DERIVE_RATIO ||
            (gear->fallback == VP_DERIVE_RATIO && e->sigs[e->sig_gear].fallback_on)) {
            return true;
        }
    }
    return false;
}

static void req_index_append(vp_engine_t *e, uint16_t r, uint16_t si)
{
    uint8_t source = (uint8_t)si;

    if (r >= e->p->n_req || si >= e->p->n_src) return;
    e->req_next[source] = ENG_INDEX_NONE;
    if (e->req_head[r] == ENG_INDEX_NONE) {
        e->req_head[r] = source;
    } else {
        e->req_next[e->req_tail[r]] = source;
    }
    e->req_tail[r] = source;
}

static void req_complete(vp_engine_t *e, int r, bool match, const uint8_t *d,
                         size_t n, uint32_t now)
{
    uint8_t source;

    if (e->cfg.responsive_polling) {
        if (match) e->req_fail_run[r] = 0;
        else if (e->req_fail_run[r] < 5) e->req_fail_run[r]++;
    }
    if (match) e->st.rx_ok++;
    else e->st.rx_fail++;
    for (source = e->req_head[r]; source != ENG_INDEX_NONE; source = e->req_next[source]) {
        const vp_source_t *s = &e->src[source];
        int i = (int)s->sig;
        uint16_t si;
        float v;

        if (i < 0 || i >= e->p->n_sig || !signal_demanded(e, i)) continue;
        si = (uint16_t)(e->sig[i].src_first + e->sigs[i].cur);
        if (si != source) continue;
        if (match && src_eval(e, si, d, n, &v)) {
            sample_ok(e, i, si, v, now, false);
        } else {
            sig_fail(e, i, now);
        }
    }
}

static void req_done(vp_engine_t *e, int r, bool answered, uint32_t now)
{
    const vp_request_t *q = &e->req[r];
    const uint8_t *d = NULL;
    size_t n = 0;
    bool match = answered && resp_match(e, q->expect, q->expect_len, &d, &n);

    req_complete(e, r, match, d, n, now);
}

static void iso_complete(vp_engine_t *e, int r, uint32_t now)
{
    const vp_request_t *q = &e->req[r];
    const uint8_t *d = NULL;
    size_t n = 0;
    bool match = try_match(e->iso_payload, e->iso_len, q->expect, q->expect_len, &d, &n);

    req_complete(e, r, match, d, n, now);
    iso_reset(e);
}

static void iso_fail(vp_engine_t *e, int r, uint32_t now)
{
    req_complete(e, r, false, NULL, 0, now);
    iso_reset(e);
}

static bool push_iso_flow_control(vp_engine_t *e, int r)
{
    eng_step_t *s;

    e->iso_req = (uint16_t)r;
    e->iso_block_count = 0;
    s = q_push_front(e, K_ISO_FC, "300100", 0);
    if (!s) return false;
    s->req = (int16_t)r;
    s->timeout_ms = ENG_ISOTP_TIMEOUT;
    return true;
}

static bool push_iso_wait(vp_engine_t *e, int r)
{
    eng_step_t *w = q_push_front(e, K_ISO_WAIT, "", 0);

    if (!w) return false;
    w->req = (int16_t)r;
    w->timeout_ms = ENG_ISOTP_TIMEOUT;
    w->delay_ms = e->iso_stmin_ms;
    return true;
}

static void can_frame(vp_engine_t *e, uint32_t id, const uint8_t *d, size_t n, uint32_t now)
{
    uint16_t k;

    e->st.can_frames++;
    for (k = 0; k < e->p->n_can; k++) {
        const vp_can_rule_t *rule = &e->can[k];
        uint16_t si = rule->src, cur_abs;
        int i = rule->sig;
        float v;

        if (rule->can_id != id || i >= e->p->n_sig || !signal_demanded(e, i)) continue;
        cur_abs = (uint16_t)(e->sig[i].src_first + e->sigs[i].cur);
        if (si > cur_abs) continue;
        if (!src_eval(e, si, d, n, &v)) {
            if (si == cur_abs) sig_fail(e, i, now);
            continue;
        }
        if (si < cur_abs) {
            e->sigs[i].cur = (uint8_t)(si - e->sig[i].src_first);
            e->sigs[i].fails = 0;
        }
        e->sigs[i].can_seen = true;
        e->sigs[i].last_can_ms = now;
        sample_ok(e, i, si, v, now, true);
    }
}

static void mon_line(vp_engine_t *e, const char *s, size_t len, uint32_t now)
{
    uint8_t data[8];
    size_t n = 0;
    uint32_t id = 0;

    while (len && s[0] == ' ') {
        s++;
        len--;
    }
    while (len && s[len - 1] == ' ') len--;
    if (len == 0) return;
    if (len == 1 && s[0] == '?') {
        e->ma_refused = true;
        return;
    }
    if (parse_can_line(s, len, &id, data, &n)) can_frame(e, id, data, n, now);
}

/* Consume complete monitor lines from the receive buffer. */
static void mon_consume(vp_engine_t *e, uint32_t now, bool flush)
{
    size_t start = 0, i;

    for (i = 0; i < e->rx_len; i++) {
        char c = e->rxbuf[i];
        if (c == '\r' || c == '\n' || c == '>') {
            if (i > start) mon_line(e, &e->rxbuf[start], i - start, now);
            start = i + 1;
        }
    }
    if (flush && start < e->rx_len) {
        mon_line(e, &e->rxbuf[start], e->rx_len - start, now);
        start = e->rx_len;
    }
    if (start == 0 && e->rx_len >= ENG_RX_CAP) start = e->rx_len;   /* garbage without newlines */
    if (start) {
        size_t remain = e->rx_len - start;
        if (remain) memmove(e->rxbuf, e->rxbuf + start, remain);
        e->rx_len = (uint16_t)remain;
    }
}

static void ma_done(vp_engine_t *e, uint32_t now)
{
    int i;

    mon_consume(e, now, true);
    if (e->ma_refused) {
        elog(e, "ATMA refused, monitor disabled for this link");
        e->mon_ok = false;
        for (i = 0; i < e->p->n_sig; i++) {
            const vp_source_t *s;
            if (!signal_demanded(e, i)) continue;
            s = cur_src(e, i);
            if (s->kind == VP_SRC_CAN) advance_source(e, i, now);
        }
        return;
    }
    for (i = 0; i < e->p->n_sig; i++) {
        eng_sig_t *g = &e->sigs[i];
        const vp_source_t *s;
        if (!signal_demanded(e, i)) continue;
        s = cur_src(e, i);
        if (s->kind != VP_SRC_CAN) {
            g->can_seen = false;
            continue;
        }
        if (!g->can_seen) sig_fail(e, i, now);
        g->can_seen = false;
        /* sig_fail may have moved to a request source */
        if (cur_src(e, i)->kind == VP_SRC_CAN && e->p->mon_stale_ms && !g->can_stale &&
            elapsed(now, g->last_can_ms, e->p->mon_stale_ms)) {
            sig_invalidate(e, i, now);
            g->can_stale = true;
        }
    }
}

static void rv_done(vp_engine_t *e)
{
    const char *s = e->rxbuf;
    double v;

    while (*s && !(*s >= '0' && *s <= '9')) s++;
    if (!*s) return;
    v = strtod(s, NULL);
    if (v > 0.0 && v < 40.0 && e->sink.on_voltage) e->sink.on_voltage(e->sink.ctx, (float)v);
}

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

static bool init_has_exact(const vp_profile_t *p, const vp_at_cmd_t *init,
                           const char *want)
{
    uint16_t i;
    for (i = 0; i < p->n_init; i++) {
        if (!strcmp(init[i].cmd, want)) return true;
    }
    return false;
}

static bool init_has_prefix(const vp_profile_t *p, const vp_at_cmd_t *init,
                            const char *prefix)
{
    uint16_t i;
    size_t n = strlen(prefix);
    for (i = 0; i < p->n_init; i++) {
        if (!strncmp(init[i].cmd, prefix, n)) return true;
    }
    return false;
}

static bool init_has_adaptive_timing(const vp_profile_t *p,
                                     const vp_at_cmd_t *init)
{
    uint16_t i;
    for (i = 0; i < p->n_init; i++) {
        const char *s = init[i].cmd;
        if (!strncmp(s, "ATAT", 4) && s[4] >= '0' && s[4] <= '2' && s[5] == '\0') {
            return true;
        }
    }
    return false;
}

static bool init_has_reset(const vp_profile_t *p, const vp_at_cmd_t *init)
{
    return init_has_exact(p, init, "ATZ") ||
           init_has_exact(p, init, "ATD") ||
           init_has_exact(p, init, "ATD0");
}

static bool init_is_common_command(const char *cmd)
{
    return !strcmp(cmd, "ATZ") || !strcmp(cmd, "ATD") || !strcmp(cmd, "ATD0") ||
           !strcmp(cmd, "ATE0") || !strcmp(cmd, "ATH1") || !strcmp(cmd, "ATH0") ||
           !strcmp(cmd, "ATS0") || !strcmp(cmd, "ATM0") ||
           !strcmp(cmd, "ATAL") || !strncmp(cmd, "ATSP", 4) ||
           (!strncmp(cmd, "ATAT", 4) && cmd[4] >= '0' && cmd[4] <= '2' &&
            cmd[5] == '\0');
}

static bool init_has_common_command(const vp_profile_t *p,
                                    const vp_at_cmd_t *init)
{
    uint16_t i;

    for (i = 0; i < p->n_init; i++) {
        if (init_is_common_command(init[i].cmd)) return true;
    }
    return false;
}

static const char *init_find_exact(const vp_profile_t *p,
                                   const vp_at_cmd_t *init,
                                   const char *want)
{
    uint16_t i;

    for (i = 0; i < p->n_init; i++) {
        if (!strcmp(init[i].cmd, want)) return init[i].cmd;
    }
    return NULL;
}

static const char *init_find_prefix(const vp_profile_t *p,
                                    const vp_at_cmd_t *init,
                                    const char *prefix)
{
    uint16_t i;

    for (i = 0; i < p->n_init; i++) {
        if (!strncmp(init[i].cmd, prefix, strlen(prefix))) return init[i].cmd;
    }
    return NULL;
}

static const char *init_find_adaptive_timing(const vp_profile_t *p,
                                             const vp_at_cmd_t *init)
{
    uint16_t i;

    for (i = 0; i < p->n_init; i++) {
        if (!strncmp(init[i].cmd, "ATAT", 4) && init[i].cmd[4] >= '0' &&
            init[i].cmd[4] <= '2' && init[i].cmd[5] == '\0') {
            return init[i].cmd;
        }
    }
    return NULL;
}

/* A profile owns the whole bring-up only when it carries the same common
 * sequence shape as Car Scanner.  A lone ATZ/ATD is still a profile hint,
 * not proof that the translated sequence is complete. */
static bool init_has_complete_sequence(const vp_profile_t *p,
                                       const vp_at_cmd_t *init)
{
    uint16_t i;
    uint8_t step = 0;
    bool reset = false;

    for (i = 0; i < p->n_init; i++) {
        const char *cmd = init[i].cmd;

        if (!strcmp(cmd, "ATZ") || !strcmp(cmd, "ATD") || !strcmp(cmd, "ATD0")) {
            if (step != 0) return false;
            reset = true;
            continue;
        }
        if (!reset || step >= 7) {
            if (step >= 7) continue;
            return false;
        }
        if (step == 0 && !strcmp(cmd, "ATE0")) step++;
        else if (step == 1 && !strcmp(cmd, "ATH1")) step++;
        else if (step == 2 && !strncmp(cmd, "ATSP", 4)) step++;
        else if (step == 3 && !strcmp(cmd, "ATM0")) step++;
        else if (step == 4 && !strcmp(cmd, "ATS0")) step++;
        else if (step == 5 && !strncmp(cmd, "ATAT", 4) &&
                 cmd[4] >= '0' && cmd[4] <= '2' && cmd[5] == '\0') step++;
        else if (step == 6 && !strcmp(cmd, "ATAL")) step++;
        else return false;
    }
    return reset && step == 7;
}

static void queue_profile_init(vp_engine_t *e, const vp_profile_t *p,
                               const vp_at_cmd_t *init)
{
    uint16_t i;
    bool default_commands_queued = false;
    bool profile_has_sequence = init_has_reset(p, init) || init_has_common_command(p, init);
    bool has_atd = init_has_exact(p, init, "ATD");
    bool has_atd0 = init_has_exact(p, init, "ATD0");

    for (i = 0; i < p->n_init; i++) {
        uint16_t wait_ms;
        bool reset = !strcmp(init[i].cmd, "ATZ") ||
                     !strcmp(init[i].cmd, "ATD") ||
                     !strcmp(init[i].cmd, "ATD0");

        if (profile_has_sequence && !default_commands_queued && !reset) {
            if (!has_atd) push_init(e, "ATD", ENG_AT_DELAY);
            if (!has_atd0) push_init(e, "ATD0", ENG_AT_DELAY);
            default_commands_queued = true;
        }
        if (vp_init_wait_ms(init[i].cmd, &wait_ms)) {
            q_push(e, K_DELAY, "", wait_ms);
        } else if (e->skip_init_atz && !strcmp(init[i].cmd, "ATZ")) {
            /* CheckIsELM327 already sent ATZ and verified the prompt. */
            continue;
        } else {
            push_init(e, init[i].cmd, ENG_AT_DELAY);
        }
    }
    if (profile_has_sequence && !default_commands_queued) {
        if (!has_atd) push_init(e, "ATD", ENG_AT_DELAY);
        if (!has_atd0) push_init(e, "ATD0", ENG_AT_DELAY);
    }
}

/* Complete a translated profile around the same common setup that Car
 * Scanner uses.  A profile may contain only a reset, or may have ATSP and
 * custom commands in an older/legacy order.  Keep the profile's concrete
 * variants, but put the shared setup in a deterministic order and retain
 * all non-common commands after it. */
static void queue_partial_init(vp_engine_t *e, const vp_profile_t *p,
                               const vp_at_cmd_t *init)
{
    static const char *const k_common[] = {
        "ATE0", "ATH1", "ATSP", "ATM0", "ATS0", "ATAT1", "ATAL"
    };
    const char *cmd;
    char generated[ENG_CMD_CAP];
    uint16_t i;

    /* Preserve one or more explicit reset commands, including the APK's
     * ATD/ATD0 pair.  Profiles without a reset retain the historical ATZ
     * fallback before the common setup. */
    for (i = 0; i < p->n_init; i++) {
        if (!strcmp(init[i].cmd, "ATZ") || !strcmp(init[i].cmd, "ATD") ||
            !strcmp(init[i].cmd, "ATD0")) {
            if (!(e->skip_init_atz && !strcmp(init[i].cmd, "ATZ"))) {
                push_init(e, init[i].cmd, ENG_AT_DELAY);
            }
        }
    }
    if (!init_has_reset(p, init) && !e->skip_init_atz) {
        q_push(e, K_AT, "ATZ", ENG_AT_DELAY);
    }
    if (!init_has_exact(p, init, "ATD")) {
        push_init(e, "ATD", ENG_AT_DELAY);
    }
    if (!init_has_exact(p, init, "ATD0")) {
        push_init(e, "ATD0", ENG_AT_DELAY);
    }

    for (i = 0; i < sizeof(k_common) / sizeof(k_common[0]); i++) {
        if (!strcmp(k_common[i], "ATSP")) {
            cmd = init_find_prefix(p, init, "ATSP");
            if (!cmd) {
                snprintf(generated, sizeof(generated), "ATSP%X",
                         (unsigned)e->run_proto);
                cmd = generated;
            }
        } else if (!strcmp(k_common[i], "ATAT1")) {
            cmd = init_find_adaptive_timing(p, init);
            if (!cmd) cmd = k_common[i];
        } else {
            cmd = init_find_exact(p, init, k_common[i]);
            if (!cmd) cmd = k_common[i];
        }
        push_init(e, cmd, ENG_AT_DELAY);
    }

    for (i = 0; i < p->n_init; i++) {
        uint16_t wait_ms;
        if (vp_init_wait_ms(init[i].cmd, &wait_ms)) {
            q_push(e, K_DELAY, "", wait_ms);
        } else if (!init_is_common_command(init[i].cmd)) {
            push_init(e, init[i].cmd, ENG_AT_DELAY);
        }
    }
}

static void start_init(vp_engine_t *e)
{
    const vp_profile_t *p = e->p;
    const vp_at_cmd_t *init = vp_profile_init(p);
    static const char *const k_base[] = {
        "ATZ", "ATD", "ATD0", "ATE0", "ATH1", "ATSP", "ATM0", "ATS0", "ATAT1", "ATAL"
    };
    char c[ENG_CMD_CAP];
    eng_step_t *s;
    size_t k;
    bool profile_has_reset = init_has_reset(p, init);
    bool profile_has_common = init_has_common_command(p, init);
    bool profile_owns_sequence = init_has_complete_sequence(p, init);

    e->phase = VP_ENGINE_PH_INIT;
    e->st.phase = e->phase;
    q_clear(e);
    /* state after ATZ */
    e->hdr = ENG_ID_UNKNOWN;
    e->rx = 0;
    e->base_rx = 0;
    e->cea = ENG_CEA_OFF;
    e->caf0 = 0;
    /* ELM327 resets with automatic flow control enabled. */
    e->cfc0 = 0;

    if (profile_owns_sequence) {
        queue_profile_init(e, p, init);
    } else if (profile_has_reset || profile_has_common) {
        queue_partial_init(e, p, init);
    } else if (!profile_owns_sequence) {
        for (k = 0; k < sizeof(k_base) / sizeof(k_base[0]); k++) {
            if (!strcmp(k_base[k], "ATSP")) {
                if (init_has_prefix(p, init, "ATSP")) continue;
                snprintf(c, sizeof(c), "ATSP%X", (unsigned)e->run_proto);
                q_push(e, K_AT, c, ENG_AT_DELAY);
            } else if (!strcmp(k_base[k], "ATZ") && e->skip_init_atz) {
                continue;
            } else if (!strcmp(k_base[k], "ATD") || !strcmp(k_base[k], "ATD0")) {
                if (!init_has_exact(p, init, k_base[k])) {
                    push_init(e, k_base[k], ENG_AT_DELAY);
                }
            } else if (!strcmp(k_base[k], "ATAT1")) {
                if (!init_has_adaptive_timing(p, init)) {
                    q_push(e, K_AT, k_base[k], ENG_AT_DELAY);
                }
            } else if (!strcmp(k_base[k], "ATH1") &&
                       legacy_obd_protocol(e)) {
                q_push(e, K_AT, "ATH0", ENG_AT_DELAY);
            } else if (!init_has_exact(p, init, k_base[k])) {
                q_push(e, K_AT, k_base[k], ENG_AT_DELAY);
            }
        }
    }

    /* Profiles without a reset keep the existing common setup, then append
     * their extras.  Complete reset-bearing sequences were sent verbatim;
     * partial reset-bearing sequences were normalized above. */
    if (!profile_owns_sequence && !profile_has_reset && !profile_has_common) {
        queue_profile_init(e, p, init);
    }

    /* Complete profile sequences can bypass the common setup queue. Reassert
     * header-off after profile init so legacy OBD replies remain unprefixed. */
    if (legacy_obd_protocol(e)) {
        q_push(e, K_AT, "ATH0", ENG_AT_DELAY);
    }

    /* The flag applies to this bring-up only.  A later self-heal must retain
     * the established reset semantics instead of silently omitting ATZ. */
    e->skip_init_atz = false;

    /* CheckConnectionCommand is a single request in the APK's generic path;
     * voltage is a periodic measurement, not part of link initialization. */
    s = q_push(e, K_WARM, p->check_connection[0] ? p->check_connection : "0100", ENG_WARM_DELAY);
    if (s) s->timeout_ms = connection_timeout(e);
    elog(e, "init %s proto %u", p->id, (unsigned)e->run_proto);
}

static void start_detect(vp_engine_t *e)
{
    e->phase = VP_ENGINE_PH_DETECT;
    e->st.phase = e->phase;
    e->detect_next = 0;
    e->detect_attempt = 0;
    e->detect_fallback = false;
    e->run_proto = 0;
    q_clear(e);
    elog(e, "protocol auto-detect across %u candidates",
         (unsigned)detect_count(e));
}

/* Car Scanner performs an adapter-level check before ECU protocol probing.
 * The ELM prompt is the only positive signal here; an ECU request such as
 * 0100 must not be used to decide whether the BLE adapter is an ELM327. */
static void elm_check_done(vp_engine_t *e, uint32_t now, bool answered)
{
    uint8_t pr;

    if (!answered || strchr(e->rxbuf, '>') == NULL) {
        elog(e, "ELM327 check failed");
        q_clear(e);
        e->phase = VP_ENGINE_PH_RECONNECT;
        e->st.phase = e->phase;
        if (e->sink.reconnect) {
            e->sink.reconnect(e->sink.ctx);
        } else {
            e->phase = VP_ENGINE_PH_DOWN;
            e->st.phase = e->phase;
        }
        return;
    }

    elog(e, "ELM327 check passed");
    e->skip_init_atz = true;
    pr = e->p->proto ? e->p->proto : (e->det_proto ? e->det_proto : e->cfg.proto);
    if (pr == 0) {
        start_detect(e);
    } else {
        e->run_proto = pr;
        start_init(e);
    }
    (void)now;
}

static void detect_next(vp_engine_t *e)
{
    eng_step_t *s;
    char c[ENG_CMD_CAP];
    uint8_t proto;

    if (e->detect_attempt >= ENG_DETECT_ATTEMPTS) {
        e->detect_next++;
        e->detect_attempt = 0;
    }
    if (e->detect_next >= detect_count(e)) {
        /* Keep the adapter in auto mode if every explicit candidate failed. */
        elog(e, "all protocol probes failed, continuing with ATSP0");
        e->run_proto = 0;
        start_init(e);
        return;
    }
    proto = detect_protocol(e, e->detect_next);
    e->run_proto = proto;
    e->detect_attempt++;
    e->detect_fallback = false;
    q_push(e, K_AT, "ATZ", ENG_AT_DELAY);
    push_init(e, "ATD", ENG_AT_DELAY);
    push_init(e, "ATD0", ENG_AT_DELAY);
    q_push(e, K_AT, "ATE0", ENG_AT_DELAY);
    q_push(e, K_AT, legacy_obd_protocol(e) ? "ATH0" : "ATH1", ENG_AT_DELAY);
    snprintf(c, sizeof(c), "ATSP%X", (unsigned)proto);
    q_push(e, K_AT, c, ENG_AT_DELAY);
    q_push(e, K_AT, "ATM0", ENG_AT_DELAY);
    q_push(e, K_AT, "ATS0", ENG_AT_DELAY);
    q_push(e, K_AT, "ATAT1", ENG_DETECT_SETTLE);
    q_push(e, K_AT, "ATAL", ENG_AT_DELAY);
    s = q_push(e, K_DETECT, e->p->check_connection[0] ? e->p->check_connection : "0100", 0);
    if (s) {
        s->timeout_ms = connection_timeout(e);
        s->val = 0;
    }
    elog(e, "probe protocol %u attempt %u/%u", (unsigned)proto,
         (unsigned)e->detect_attempt, (unsigned)ENG_DETECT_ATTEMPTS);
}

static void detect_done(vp_engine_t *e, const eng_step_t *s, int how)
{
    bool answered = how == DONE_PROMPT;
    uint8_t expect[8];
    size_t expect_len = 0;
    const uint8_t *d;
    size_t n;
    const char *probe = s->cmd;

    (void)s;
    /* Protocol probing must use the profile's connection check.  Hard-coding
     * 010C makes a valid adapter look absent for profiles that intentionally
     * omit the optional RPM PID.  Positive OBD responses retain the request
     * parameters (0100 -> 4100, 0902 -> 4902, 22F190 -> 62F190). */
    if (!answered && how == DONE_TIMEOUT) {
        eng_step_t *drain;
        q_clear(e);
        drain = q_push(e, K_DRAIN, "", 0);
        if (drain) {
            drain->timeout_ms = ENG_DRAIN_TIMEOUT;
            elog(e, "protocol probe timed out, interrupting ELM before next candidate");
        } else {
            e->phase = e->st.phase = VP_ENGINE_PH_RECONNECT;
            if (e->sink.reconnect) e->sink.reconnect(e->sink.ctx);
            else e->phase = e->st.phase = VP_ENGINE_PH_DOWN;
        }
        return;
    }
    if (!vp_parse_hex_bytes(probe,
                            false, expect, sizeof(expect), &expect_len)) {
        q_clear(e);
        detect_next(e);
        return;
    }
    expect[0] = (uint8_t)(expect[0] + 0x40u);
    if (!answered || !resp_match(e, expect, expect_len, &d, &n)) {
        /* Some generic ECUs do not implement the supported-PID bitmap (0100)
         * but do answer the demanded RPM PID. Try that once for each
         * candidate before discarding the protocol. Explicit profile checks
         * keep their original semantics. */
        bool generic_check = e->p->check_connection[0] == '\0';
        bool has_rpm = false;
        if (generic_check && !e->detect_fallback) {
            for (uint16_t i = 0; i < e->p->n_req; i++) {
                if (!(e->req[i].flags & VP_REQ_F_RAW) && e->req[i].service == 0x01 &&
                    e->req[i].tx_len >= 2 && e->req[i].tx[1] == 0x0C) {
                    has_rpm = true;
                    break;
                }
            }
        }
        if (has_rpm && !strcmp(probe, "0100")) {
            eng_step_t *fallback;
            e->detect_fallback = true;
            q_clear(e);
            fallback = q_push(e, K_DETECT, "010C", 0);
            if (fallback) fallback->timeout_ms = connection_timeout(e);
            return;
        }
        /* APK retries each candidate before moving to the next protocol. */
        q_clear(e);
        detect_next(e);
        return;
    }
    e->det_proto = e->run_proto;
    if (e->sink.on_proto_detected) e->sink.on_proto_detected(e->sink.ctx, e->det_proto);
    elog(e, "protocol %u accepted", (unsigned)e->det_proto);
    start_init(e);
}

static void init_cmd_done(vp_engine_t *e, const eng_step_t *s, bool ok)
{
    const char *c = s->cmd;
    uint32_t id = 0;
    char *end = NULL;
    unsigned long proto;

    if (!strncmp(c, "ATSH", 4)) {
        e->hdr = (ok && vp_classify_tx_header(c + 4, &id) != VP_HDR_INVALID) ? id : ENG_ID_UNKNOWN;
    } else if (!strncmp(c, "ATCRA", 5)) {
        if (ok && (c[5] == '\0' || vp_parse_rx_filter(c + 5, &id))) {
            e->base_rx = id;
            e->rx = id;
        } else {
            e->rx = ENG_ID_UNKNOWN;
        }
    } else if (!strncmp(c, "ATSP", 4)) {
        proto = strtoul(c + 4, &end, 16);
        if (ok && c[4] == 'A' && c[5] != '\0') {
            e->run_proto = 0; /* ATSPAh allows the adapter to choose another protocol */
        } else if (ok && end && *end == '\0' && proto <= 12) {
            e->run_proto = (uint8_t)proto;
        }
    } else if (!strcmp(c, "ATCAF0")) {
        e->caf0 = ok ? 1 : ENG_ST_UNKNOWN;
    } else if (!strcmp(c, "ATCAF1")) {
        e->caf0 = ok ? 0 : ENG_ST_UNKNOWN;
    } else if (!strncmp(c, "ATCEA", 5)) {
        if (!ok) {
            e->cea = ENG_ST_UNKNOWN;
        } else if (c[5] == '\0') {
            e->cea = ENG_CEA_OFF;
        } else {
            proto = strtoul(c + 5, &end, 16);
            e->cea = (end && *end == '\0' && proto <= 0xFFu) ? (int16_t)proto : ENG_ST_UNKNOWN;
        }
    } else if (!strcmp(c, "ATCFC0")) {
        e->cfc0 = ok ? 1 : ENG_ST_UNKNOWN;
    } else if (!strcmp(c, "ATCFC1")) {
        e->cfc0 = ok ? 0 : ENG_ST_UNKNOWN;
    }
}

static void enter_run(vp_engine_t *e, uint32_t now)
{
    int i;

    e->phase = VP_ENGINE_PH_RUN;
    e->st.phase = e->phase;
    e->last_good_ms = now;
    e->last_vbat_ms = now;
    e->last_mon_ms = now;
    e->last_tester_ms = now;
    e->in_round = false;
    e->slot_n = e->slot_pos = 0;
    for (i = 0; i < e->p->n_sig; i++) {
        e->sigs[i].last_poll_ms = now - 60000u;
        e->sigs[i].last_can_ms = now;
    }
    e->req_seen_mask = 0;
    memset(e->req_fail_run, 0, sizeof(e->req_fail_run));
    elog(e, "running");
}

/* ------------------------------------------------------------------ */
/* polling plan                                                        */
/* ------------------------------------------------------------------ */

/* Fast-changing driver inputs are serviced before each background slot.
 * Keep state/before/request/after and ISO-TP transactions indivisible. */
static bool responsive_signal(const vp_engine_t *e, int i)
{
    if (!e->cfg.responsive_polling) return false;
    int slot = e->sigs[i].slot;
    if (slot == SIG_OBD_RPM || slot == SIG_OBD_SPEED) return true;
    /* A profile can explicitly retain slow polling for auxiliary inputs. */
    return e->sig[i].rate != VP_RATE_SLOW &&
           (slot == SIG_OBD_BOOST || slot == SIG_OBD_THROTTLE);
}

static uint32_t responsive_period(const vp_engine_t *e, int i)
{
    if (e->sig[i].period_ms) return e->sig[i].period_ms;
    if (e->sigs[i].slot == SIG_OBD_RPM) return 50u;
    if (e->sigs[i].slot == SIG_OBD_SPEED) return 100u;
    return 0u;  /* pressure / throttle: each batch, bounded by serial polling */
}

static bool request_ready(const vp_engine_t *e, uint8_t r, uint32_t now)
{
    uint8_t failures = e->req_fail_run[r];
    if (!e->cfg.responsive_polling || !failures) return true;
    return elapsed(now, e->req_last_ms[r], 1000u << (failures - 1u));
}

static bool responsive_due(const vp_engine_t *e, int i, uint32_t now)
{
    const vp_source_t *src = cur_src(e, i);
    if (!responsive_signal(e, i) || !sig_driven(e, i) || !signal_demanded(e, i) ||
        src->kind != VP_SRC_REQ || !request_ready(e, (uint8_t)src->ref, now)) return false;
    uint32_t period = responsive_period(e, i);
    return !(e->req_seen_mask & (UINT32_C(1) << src->ref)) ||
           elapsed(now, e->req_last_ms[src->ref], period);
}

static bool poll_candidate(const vp_engine_t *e, int i)
{
    if (!sig_driven(e, i) || e->sig[i].rate == VP_RATE_EVERY ||
        !signal_demanded(e, i) || responsive_signal(e, i)) return false;
    return cur_src(e, i)->kind == VP_SRC_REQ;
}

static bool request_header_same(const vp_request_t *a, const vp_request_t *b)
{
    return a->hdr == b->hdr &&
           ((a->rsv ^ b->rsv) & VP_REQ_RSV_HDR_EXPLICIT) == 0;
}

static bool sig_due(const vp_engine_t *e, int i, uint32_t now)
{
    const vp_signal_t *s = &e->sig[i];

    if (s->period_ms && !elapsed(now, e->sigs[i].last_poll_ms, s->period_ms)) return false;
    if (s->rate == VP_RATE_SLOW) {
        uint32_t div = s->slow_div ? s->slow_div : VP_DEFAULT_SLOW_DIV;
        return ((e->round_no + (uint32_t)i) % div) == 0;
    }
    return true;
}

static size_t every_reqs(const vp_engine_t *e, uint8_t *out, uint32_t now)
{
    size_t n = 0, k, j;
    for (k = 0; k < e->n_every; k++) {
        int i = e->every_sig[k];
        const vp_source_t *s;
        if (!sig_driven(e, i) || !signal_demanded(e, i) || responsive_signal(e, i)) continue;
        s = cur_src(e, i);
        if (s->kind != VP_SRC_REQ || !request_ready(e, (uint8_t)s->ref, now)) continue;
        for (j = 0; j < n && out[j] != s->ref; j++) {}
        if (j == n) out[n++] = (uint8_t)s->ref;
    }
    return n;
}

static bool request_demanded(const vp_engine_t *e, uint8_t r)
{
    uint8_t source;
    if (r >= e->p->n_req) return false;
    for (source = e->req_head[r]; source != ENG_INDEX_NONE; source = e->req_next[source]) {
        int i = (int)e->src[source].sig;
        if (i >= 0 && i < e->p->n_sig && signal_demanded(e, i)) return true;
    }
    return false;
}

static void start_round(vp_engine_t *e, uint32_t now)
{
    uint8_t ev[VP_MAX_EVERY_SIGNALS];
    uint8_t k;
    uint32_t seen = 0;

    /* Validated request refs are 0..31. Keep first-seen profile order in the
     * array; the local mask only replaces its repeated membership scans. */
    e->round_n = 0;
    e->round_pos = 0;
    e->every_done = false;
    for (k = 0; k < e->n_poll; k++) {
        int i = e->poll_sig[k];
        uint8_t r;
        uint32_t bit;
        if (!poll_candidate(e, i) || !sig_due(e, i, now)) continue;
        r = (uint8_t)cur_src(e, i)->ref;
        if (!request_ready(e, r, now)) continue;
        bit = UINT32_C(1) << r;
        if (!(seen & bit)) {
            seen |= bit;
            e->round_req[e->round_n++] = r;
        }
        /* A shared request still advances each due signal's deadline. */
        e->sigs[i].last_poll_ms = now;
    }
    e->in_round = e->round_n > 0 || every_reqs(e, ev, now) > 0;
    for (int i = 0; i < e->p->n_sig; i++) {
        if (responsive_due(e, i, now)) e->in_round = true;
    }
}

static bool build_slot(vp_engine_t *e, uint32_t now)
{
    uint8_t ev[VP_MAX_EVERY_SIGNALS];
    size_t nev = every_reqs(e, ev, now), k;

    e->slot_n = 0;
    e->slot_pos = 0;
    e->rpm_refreshed = false;
    if (e->round_pos >= e->round_n && e->every_done) return false;
    e->every_done = true;
    /* At most four dynamic inputs, then one background request for fairness. */
    for (int i = 0; i < e->p->n_sig; i++) {
        if (!responsive_due(e, i, now)) continue;
        uint8_t r = (uint8_t)cur_src(e, i)->ref;
        for (k = 0; k < e->slot_n && e->slot_req[k] != r; k++) {}
        if (k == e->slot_n) e->slot_req[e->slot_n++] = r;
    }
    while (e->round_pos < e->round_n) {
        uint8_t main_req = e->round_req[e->round_pos++];
        if (!request_demanded(e, main_req) || !request_ready(e, main_req, now)) continue;
        for (k = 0; k < nev; k++) {
            size_t j;
            for (j = 0; j < e->slot_n && e->slot_req[j] != ev[k]; j++) {}
            if (j == e->slot_n) e->slot_req[e->slot_n++] = ev[k];
        }
        for (k = 0; k < e->slot_n && e->slot_req[k] != main_req; k++) {}
        if (k == e->slot_n) e->slot_req[e->slot_n++] = main_req;
        break;
    }
    if (e->round_n == 0) {
        for (k = 0; k < nev; k++) {
            size_t j;
            for (j = 0; j < e->slot_n && e->slot_req[j] != ev[k]; j++) {}
            if (j == e->slot_n) e->slot_req[e->slot_n++] = ev[k];
        }
    }
    return e->slot_n > 0;
}

/* A full dynamic-input batch must not halve RPM freshness. Insert at most
 * one overdue RPM refresh after two completed requests, then finish the batch
 * including its background item. Short batches do not need this extra sample;
 * this never interrupts an in-flight chain. */
static void refresh_rpm_slot(vp_engine_t *e, uint32_t now)
{
    if (!e->cfg.responsive_polling || e->rpm_refreshed || e->slot_pos < 2 ||
        e->slot_n - e->slot_pos < 2 || e->sig_rpm < 0 ||
        !responsive_due(e, e->sig_rpm, now) || e->slot_n >= sizeof(e->slot_req)) return;
    uint8_t r = (uint8_t)cur_src(e, e->sig_rpm)->ref;
    for (uint8_t k = e->slot_pos; k < e->slot_n; k++) {
        if (e->slot_req[k] == r) return; // an RPM request is already pending
    }
    memmove(e->slot_req + e->slot_pos + 1, e->slot_req + e->slot_pos,
            (size_t)e->slot_n - e->slot_pos);
    e->slot_req[e->slot_pos] = r;
    e->slot_n++;
    e->rpm_refreshed = true;
}

/* Queue the first adapter-state change request r needs; false when none. */
static bool push_state_for(vp_engine_t *e, uint8_t r)
{
    const vp_request_t *q = &e->req[r];
    bool functional_fallback = (e->functional_fallback_mask & (UINT32_C(1) << r)) != 0;
    uint32_t want_hdr = request_header(e, functional_fallback ? 0x7DFu : q->hdr);
    bool want_hdr_explicit = functional_fallback ||
                             ((q->rsv & VP_REQ_RSV_HDR_EXPLICIT) != 0);
    uint32_t want_rx = response_filter(e, (q->flags & VP_REQ_F_RX) ? q->rx : e->base_rx);
    int16_t want_cea = (q->flags & VP_REQ_F_CEA) ? (int16_t)q->cea : (int16_t)ENG_CEA_OFF;
    bool manual_multi = (q->flags & VP_REQ_F_MULTI) &&
                        !(q->flags & VP_REQ_F_MULTI_ELM);
    int8_t want_caf0 = ((q->flags & VP_REQ_F_RAW) || manual_multi) ? 1 : 0;
    int8_t want_cfc0 = manual_multi ? 1 : 0;
    bool defer_same_header = e->pending_after_req >= 0 &&
                             request_header_same(&e->req[e->pending_after_req], q);

    if (e->hdr != want_hdr || e->hdr_explicit != want_hdr_explicit) {
        /* J1850 / ISO 9141 / KWP use native OBD addressing, not CAN
         * headers. Imported profiles may explicitly carry a CAN-style 7E0;
         * keep native ELM addressing for these protocols regardless. */
        if (legacy_obd_protocol(e)) {
            e->hdr = want_hdr;
            e->hdr_explicit = want_hdr_explicit;
        } else {
            push_state(e, ST_HDR, want_hdr, r);
            return true;
        }
    }
    if (e->rx != want_rx && ((q->flags & VP_REQ_F_RX) || !defer_same_header)) {
        push_state(e, ST_RX, want_rx, r);
        return true;
    }
    if (e->cea != want_cea && ((q->flags & VP_REQ_F_CEA) || !defer_same_header)) {
        push_state(e, ST_CEA, want_cea == ENG_CEA_OFF ? ENG_CEA_OFF_VAL : (uint32_t)want_cea, r);
        return true;
    }
    if (e->caf0 != want_caf0 &&
        ((q->flags & VP_REQ_F_RAW) || manual_multi || !defer_same_header)) {
        push_state(e, ST_CAF, (uint32_t)want_caf0, r);
        return true;
    }
    if (e->cfc0 != want_cfc0 && (manual_multi || !defer_same_header)) {
        push_state(e, ST_CFC, (uint32_t)want_cfc0, r);
        return true;
    }
    return false;
}

static void push_req(vp_engine_t *e, uint8_t r)
{
    const vp_request_t *q = &e->req[r];
    char c[ENG_CMD_CAP];
    eng_step_t *s;
    uint8_t bytes[8];
    uint8_t k, n = q->tx_len < sizeof(bytes) ? q->tx_len : sizeof(bytes);

    /* CAF0 sends the bytes exactly as entered, including the ISO-TP
     * single-frame length. CAF1 adds that byte for normal ELM requests. */
    if ((q->flags & VP_REQ_F_MULTI) &&
        !(q->flags & (VP_REQ_F_RAW | VP_REQ_F_MULTI_ELM)) && n < 8) {
        bytes[0] = n;
        memcpy(bytes + 1, q->tx, n);
        n++;
    } else {
        memcpy(bytes, q->tx, n);
    }
    for (k = 0; k < n; k++) {
        c[2u * k] = eng_hex[bytes[k] >> 4];
        c[2u * k + 1u] = eng_hex[bytes[k] & 0x0Fu];
    }
    c[2u * n] = '\0';
    s = q_push(e, K_REQ, c, e->p->gap_ms);
    if (s) s->req = r;
}

static void push_req_at(vp_engine_t *e, uint8_t r, const vp_at_cmd_t *cmd, uint8_t side)
{
    eng_step_t *s = q_push(e, K_REQ_AT, cmd->cmd, 0);

    if (!s) return;
    s->req = r;
    s->req_at_side = side;
}

/* Car Scanner retains a request's AfterCommands until the next header change. */
static bool push_pending_after(vp_engine_t *e, uint8_t next_req)
{
    int16_t pending = e->pending_after_req;
    const vp_request_t *q;
    uint8_t i;

    if (pending < 0 || pending >= e->p->n_req ||
        request_header_same(&e->req[pending], &e->req[next_req])) {
        return false;
    }
    q = &e->req[pending];
    for (i = 0; i < q->after_count; i++) {
        push_req_at(e, (uint8_t)pending, &q->after[i], 1);
    }
    e->pending_after_req = -1;
    return q->after_count != 0;
}

static void push_req_before(vp_engine_t *e, uint8_t r)
{
    const vp_request_t *q = &e->req[r];
    uint8_t i;

    e->chain_req = r;
    e->chain_failed = false;
    for (i = 0; i < q->before_count; i++) {
        push_req_at(e, r, &q->before[i], 0);
    }
}

/* Car Scanner restores automatic CAN formatting and adapter-managed flow
 * control when a manually reassembled response is complete. Queue these
 * changes immediately so the next request cannot observe the temporary
 * CAF0/CFC0 state, even when it uses the same header. */
static void restore_iso_state(vp_engine_t *e)
{
    if (e->caf0 != 0) push_state(e, ST_CAF, 0, -1);
    if (e->cfc0 != 0) push_state(e, ST_CFC, 0, -1);
}

static void start_monitor(vp_engine_t *e, uint32_t now)
{
    e->last_mon_ms = now;
    e->st.mon_windows++;
    if (e->caf0 != 0) push_state(e, ST_CAF, 0, -1);
    if (e->cfc0 != 0) push_state(e, ST_CFC, 0, -1);
    if (e->cea != ENG_CEA_OFF) push_state(e, ST_CEA, ENG_CEA_OFF_VAL, -1);
    if (e->rx != 0) push_state(e, ST_RX, 0, -1);
    q_push(e, K_AT, "ATS1", 0); /* parse_can_line consumes space-separated CAN bytes */
    q_push(e, K_AT, "ATH1", 0);
    q_push(e, K_MA, "ATMA", 0);
    q_push(e, K_AT, "ATAR", 0);
    q_push(e, K_AT, "ATH0", 0);
    q_push(e, K_AT, "ATS0", 0);
}

static bool mon_due(const vp_engine_t *e, uint32_t now)
{
    if (!mon_usable(e)) return false;
    if (e->p->mon_mode == VP_MON_CONTINUOUS) return true;
    return elapsed(now, e->last_mon_ms, e->p->mon_every_ms);
}

static void heal(vp_engine_t *e, uint32_t now)
{
    e->st.heals++;
    e->heal_count++;
    elog(e, "no data for %u ms, heal %u", (unsigned)heal_after(e), (unsigned)e->heal_count);
    if (e->heal_count >= ENG_HEAL_RECONNECT) {
        e->heal_count = 0;
        if (e->sink.reconnect) {
            q_clear(e);
            e->phase = VP_ENGINE_PH_RECONNECT;
            e->st.phase = e->phase;
            e->sink.reconnect(e->sink.ctx);
            return;
        }
    }
    e->last_good_ms = now;
    if (e->p->proto == 0 && e->cfg.proto == 0) {
        /* A previous auto result is a hint, never a permanent protocol lock.
         * Discard it when normal polling cannot decode any data. */
        e->det_proto = 0;
        e->functional_fallback_mask = 0;
        start_detect(e);
    } else {
        start_init(e);
    }
}

static void plan_run(vp_engine_t *e, uint32_t now)
{
    int guard;

    if (elapsed(now, e->last_good_ms, heal_after(e))) {
        heal(e, now);
        return;
    }
    for (guard = 0; guard < 4; guard++) {
        if (e->slot_pos < e->slot_n) {
            refresh_rpm_slot(e, now);
            uint8_t r = e->slot_req[e->slot_pos];

            if (push_pending_after(e, r)) return;
            if (!push_state_for(e, r)) {
                push_req_before(e, r);
                push_req(e, r);
                if (e->req[r].after_count) e->pending_after_req = (int16_t)r;
                e->slot_pos++;
            }
            return;
        }
        if (e->in_round) {
            if (build_slot(e, now)) continue;
            e->in_round = false;
            e->round_no++;
            e->st.rounds++;
        }
        if (elapsed(now, e->last_vbat_ms, vbat_every(e))) {
            e->last_vbat_ms = now;
            q_push(e, K_RV, "ATRV", 0);
            return;
        }
        if (mon_due(e, now)) {
            start_monitor(e, now);
            return;
        }
        if (e->p->tester_present[0] && e->p->tester_present_every_ms &&
            elapsed(now, e->last_tester_ms, e->p->tester_present_every_ms)) {
            e->last_tester_ms = now;
            q_push(e, K_TESTER, e->p->tester_present, 0);
            return;
        }
        start_round(e, now);
        if (!e->in_round) {
            e->round_no++;
            uint32_t idle = ENG_IDLE_MS;
            for (int i = 0; i < e->p->n_sig; i++) {
                if (!responsive_signal(e, i) || !sig_driven(e, i) || !signal_demanded(e, i)) continue;
                const vp_source_t *src = cur_src(e, i);
                if (src->kind != VP_SRC_REQ || !request_ready(e, (uint8_t)src->ref, now)) continue;
                uint32_t period = responsive_period(e, i);
                uint32_t age = now - e->req_last_ms[src->ref];
                uint32_t wait = age < period ? period - age : 1u;
                if (wait < idle) idle = wait;
            }
            e->next_ms = now + idle;
            return;
        }
    }
}

static void plan(vp_engine_t *e, uint32_t now)
{
    switch (e->phase) {
    case VP_ENGINE_PH_DETECT:
        detect_next(e);
        break;
    case VP_ENGINE_PH_INIT:
        enter_run(e, now);
        break;
    case VP_ENGINE_PH_RUN:
        plan_run(e, now);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* step execution                                                      */
/* ------------------------------------------------------------------ */

static bool resp_ok(const vp_engine_t *e)
{
    return strstr(e->rxbuf, "OK") != NULL;
}

static bool resp_stopped(const vp_engine_t *e)
{
    return strstr(e->rxbuf, "STOPPED") != NULL;
}

static bool resp_transient(const vp_engine_t *e)
{
    return resp_stopped(e) || strstr(e->rxbuf, "BUS INIT: ERROR") != NULL ||
           strstr(e->rxbuf, "BUFFER FULL") != NULL;
}

static bool transient_retryable(uint8_t kind)
{
    return kind == K_AT || kind == K_INIT || kind == K_STATE ||
           kind == K_DETECT || kind == K_WARM || kind == K_REQ_AT ||
           kind == K_REQ || kind == K_ELM_CHECK;
}

static bool retry_transient(vp_engine_t *e, const eng_step_t *s)
{
    eng_step_t *retry;

    if (!transient_retryable(s->kind) || s->retry_count >= ENG_STOPPED_RETRIES ||
        !resp_transient(e)) return false;
    retry = q_push_front(e, s->kind, s->cmd, ENG_STOPPED_DELAY);
    if (!retry) return false;
    retry->st = s->st;
    retry->timeout_ms = s->timeout_ms;
    retry->req = s->req;
    retry->req_at_side = s->req_at_side;
    retry->val = s->val;
    retry->retry_count = (uint8_t)(s->retry_count + 1u);
    elog(e, "%s transient adapter error, retry in %u ms", s->cmd,
         (unsigned)ENG_STOPPED_DELAY);
    return true;
}

static bool raw_init_response_ok(const vp_engine_t *e)
{
    static const char *const k_errors[] = {
        "ERROR", "NO DATA", "STOPPED", "BUFFER FULL", "BUS INIT: ERROR", "?", NULL
    };
    size_t i;

    for (i = 0; k_errors[i]; i++) {
        if (strstr(e->rxbuf, k_errors[i])) return false;
    }
    return true;
}

static bool init_response_ok(const vp_engine_t *e, const eng_step_t *s, bool answered)
{
    if (!answered) return false;
    return vp_init_raw_command(s->cmd) ? raw_init_response_ok(e) : resp_ok(e);
}

static void protocol_done(vp_engine_t *e)
{
    const char *s = e->rxbuf;
    while (*s) {
        const char *end = s;
        const char *next;
        size_t n;
        int proto;
        while (*end && *end != '\r' && *end != '\n' && *end != '>') end++;
        next = *end ? end + 1 : end;
        while (s < end && *s == ' ') s++;
        while (end > s && end[-1] == ' ') end--;
        n = (size_t)(end - s);
        if (n == 2 && (*s == 'A' || *s == 'a')) { s++; n--; }
        proto = n == 1 ? hexval(*s) : -1;
        if (proto >= 1 && proto <= 12) {
            e->run_proto = (uint8_t)proto;
            if (e->p->proto == 0 && e->cfg.proto == 0) {
                e->det_proto = e->run_proto;
                if (e->sink.on_proto_detected) e->sink.on_proto_detected(e->sink.ctx, e->det_proto);
            }
            if (legacy_obd_protocol(e)) q_push_front(e, K_AT, "ATH0", ENG_AT_DELAY);
            elog(e, "adapter selected protocol %u", (unsigned)e->run_proto);
            return;
        }
        s = next;
    }
    elog(e, "adapter did not report a concrete protocol");
}

static void step_done(vp_engine_t *e, uint32_t now, int how)
{
    eng_step_t s = e->cur;
    bool answered = how == DONE_PROMPT;
    bool need_drain = how == DONE_TIMEOUT && s.kind != K_DETECT &&
                      s.kind != K_DRAIN && s.kind != K_ELM_CHECK;

    e->busy = false;
    e->rxbuf[e->rx_len] = '\0';
    if (how == DONE_TIMEOUT && s.kind != K_MA) e->st.timeouts++;
    /* Car Scanner retries bounded transient adapter errors after 200 ms. Do
     * this before interpreting the response so detection retries the same
     * protocol candidate and request retries do not repeat BeforeCommands. */
    if (answered && retry_transient(e, &s)) {
        e->next_ms = now + ENG_STOPPED_DELAY;
        e->rx_len = 0;
        e->rxbuf[0] = '\0';
        e->prompt = false;
        return;
    }
    switch (s.kind) {
    case K_STATE: {
        bool ok = answered && resp_ok(e);
        switch (s.st) {
        case ST_HDR:
            e->hdr = ok ? s.val : ENG_ID_UNKNOWN;
            if (ok) {
                e->hdr_explicit = s.req >= 0 &&
                    (((e->functional_fallback_mask & (UINT32_C(1) << s.req)) != 0) ||
                     (e->req[s.req].rsv & VP_REQ_RSV_HDR_EXPLICIT) != 0);
            }
            break;
        case ST_RX:  e->rx = ok ? s.val : ENG_ID_UNKNOWN; break;
        case ST_CEA:
            e->cea = ok ? (s.val == ENG_CEA_OFF_VAL ? (int16_t)ENG_CEA_OFF : (int16_t)s.val)
                        : (int16_t)ENG_ST_UNKNOWN;
            break;
        case ST_CFC:  e->cfc0 = ok ? (int8_t)s.val : (int8_t)ENG_ST_UNKNOWN; break;
        default:     e->caf0 = ok ? (int8_t)s.val : (int8_t)ENG_ST_UNKNOWN; break;
        }
        if (!ok) {
            elog(e, "%s failed", s.cmd);
            if (s.req >= 0) {
                /* the request cannot go out with the wrong framing: count it failed */
                if (e->slot_pos < e->slot_n && e->slot_req[e->slot_pos] == s.req) e->slot_pos++;
                req_done(e, s.req, false, now);
            }
        }
        break;
    }
    case K_INIT:
        init_cmd_done(e, &s, init_response_ok(e, &s, answered));
        break;
    case K_REQ_AT: {
        bool ok = answered && resp_ok(e);

        init_cmd_done(e, &s, ok);
        if (!ok && s.req_at_side == 0 && s.req == e->chain_req) {
            e->chain_failed = true;
        }
        break;
    }
    case K_AT:
        /* Protocol probes are queued as K_AT so they use the firmware-owned
         * setup whitelist; retain the selected ATSP value for the probe and
         * the subsequent init sequence. */
        init_cmd_done(e, &s, answered && resp_ok(e));
        if (e->phase == VP_ENGINE_PH_DETECT && !strncmp(s.cmd, "ATSP", 4) &&
            (!answered || !resp_ok(e))) {
            /* A clone that rejects protocol selection may still answer OBD
             * requests on its old protocol. That is not proof of this candidate. */
            q_clear(e);
            detect_next(e);
        }
        if (!strcmp(s.cmd, "ATAR")) e->rx = answered && resp_ok(e) ? 0 : ENG_ID_UNKNOWN;
        break;
    case K_DETECT:
        detect_done(e, &s, how);
        break;
    case K_DRAIN:
        if (answered) {
            if (s.val == 0) detect_next(e);
        } else {
            elog(e, "ELM did not return a prompt after interrupt; reconnecting");
            q_clear(e);
            e->phase = VP_ENGINE_PH_RECONNECT;
            e->st.phase = e->phase;
            if (e->sink.reconnect) e->sink.reconnect(e->sink.ctx);
            else e->phase = e->st.phase = VP_ENGINE_PH_DOWN;
        }
        break;
    case K_ELM_CHECK:
        elm_check_done(e, now, answered);
        break;
    case K_WARM:
        /* ATSP0 / ATSPAh may resolve to any bus. Learn it before planning
         * CAN headers or decoding legacy headers during normal polling. */
        if (answered && e->run_proto == 0) q_push_front(e, K_PROTOCOL, "ATDPN", ENG_AT_DELAY);
        break;
    case K_PROTOCOL:
        if (answered) protocol_done(e);
        break;
    case K_RV:
        if (answered) rv_done(e);
        break;
    case K_TESTER:
        /* The keep-alive response is intentionally not decoded.  Any prompt
         * proves the adapter accepted the command; the next poll remains the
         * source of ECU data and link health. */
        break;
    case K_REQ:
        if (answered && !(e->req[s.req].flags & (VP_REQ_F_RAW | VP_REQ_F_MULTI)) &&
            (e->run_proto >= 6 && e->run_proto <= 9) &&
            ((e->req[s.req].hdr >= 0x7E0u && e->req[s.req].hdr <= 0x7E7u) ||
             ((e->req[s.req].hdr & (VP_ID_29BIT | 0xFFFF00FFu)) ==
              (VP_ID_29BIT | 0x18DA00F1u))) &&
            !(e->functional_fallback_mask & (UINT32_C(1) << s.req)) &&
            resp_mode01_unsupported(e, e->req[s.req].service)) {
            e->functional_fallback_mask |= UINT32_C(1) << s.req;
            if (e->slot_pos > 0) e->slot_pos--;
            elog(e, "Mode 01 request %u rejected on physical header, retrying functional OBD address",
                 (unsigned)s.req);
            iso_reset(e);
            if (s.req == e->chain_req) e->chain_req = -1;
            break;
        }
        if (answered && (e->req[s.req].flags & VP_REQ_F_MULTI) &&
            !(e->req[s.req].flags & VP_REQ_F_MULTI_ELM)) {
            bool complete = false;
            bool need_fc = false;
            bool iso = iso_consume(e, &e->req[s.req], true, &complete, &need_fc);
            if (iso && complete) {
                iso_complete(e, s.req, now);
                restore_iso_state(e);
            } else if (iso && push_iso_flow_control(e, s.req)) {
                /* The initial First Frame is retained in iso_payload; the
                 * adapter may return one Consecutive Frame per flow-control
                 * prompt, so request another FC after each frame. */
                (void)need_fc;
            } else {
                req_done(e, s.req, answered, now);
                iso_reset(e);
                restore_iso_state(e);
            }
        } else {
            req_done(e, s.req, answered, now);
            iso_reset(e);
        }
        /* A failed BeforeCommands chain is drained as synthetic failures so
         * the queue remains ordered. It never sent the request, therefore its
         * deferred AfterCommands must not leak into the next header switch. */
        if (e->chain_failed && s.req == e->chain_req) e->pending_after_req = -1;
        if (s.req == e->chain_req) e->chain_req = -1;
        break;
    case K_ISO_FC:
    {
        const vp_request_t *q = &e->req[s.req];
        bool complete = false;
        bool need_fc = false;
        bool iso = answered && iso_consume(e, q, false, &complete, &need_fc);

        if (!answered || strstr(e->rxbuf, "?") != NULL) {
            iso_fail(e, s.req, now);
            restore_iso_state(e);
        } else if (iso && complete) {
            iso_complete(e, s.req, now);
            restore_iso_state(e);
        } else if (iso && need_fc) {
            if (!push_iso_flow_control(e, s.req)) {
                iso_fail(e, s.req, now);
                restore_iso_state(e);
            }
        } else if (!push_iso_wait(e, s.req)) {
            iso_fail(e, s.req, now);
            restore_iso_state(e);
        }
        break;
    }
    case K_ISO_WAIT: {
        const vp_request_t *q = &e->req[s.req];
        bool complete = false;
        bool need_fc = false;
        bool iso = answered && iso_consume(e, q, false, &complete, &need_fc);
        if (!iso) {
            iso_fail(e, s.req, now);
            restore_iso_state(e);
        } else if (complete) {
            iso_complete(e, s.req, now);
            restore_iso_state(e);
        } else if (need_fc) {
            if (!push_iso_flow_control(e, s.req)) {
                iso_fail(e, s.req, now);
                restore_iso_state(e);
            }
        } else if (!push_iso_wait(e, s.req)) {
            iso_fail(e, s.req, now);
            restore_iso_state(e);
        }
        break;
    }
    case K_MA:
        ma_done(e, now);
        break;
    default:
        break;
    }
    if (need_drain && e->phase != VP_ENGINE_PH_DOWN &&
        e->phase != VP_ENGINE_PH_RECONNECT) {
        /* Preserve the planned continuation, but never let a late ECU reply
         * or a still-running initialization become the next command's reply. */
        eng_step_t *drain = q_push_front(e, K_DRAIN, "", 0);
        if (drain) {
            drain->timeout_ms = ENG_DRAIN_TIMEOUT;
            drain->val = 1; /* resume queued work after the prompt, not detection */
        } else {
            q_clear(e);
            e->phase = e->st.phase = VP_ENGINE_PH_RECONNECT;
            if (e->sink.reconnect) e->sink.reconnect(e->sink.ctx);
            else e->phase = e->st.phase = VP_ENGINE_PH_DOWN;
        }
    }
    e->next_ms = now + s.delay_ms;
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->prompt = false;
}

static void step_send(vp_engine_t *e, const eng_step_t *s, uint32_t now)
{
    char norm[VP_AT_MAX_LEN + 1];
    const char *cmd = s->cmd;
    bool is_at = s->kind != K_REQ && s->kind != K_DETECT && s->kind != K_WARM &&
                 s->kind != K_DRAIN &&
                 s->kind != K_TESTER &&
                 s->kind != K_ISO_FC && s->kind != K_ISO_WAIT && s->kind != K_DELAY &&
                 !(s->kind == K_INIT && vp_init_raw_command(s->cmd));

    /* A failed BeforeCommand invalidates the complete request chain. Drain
     * remaining before commands and the request itself without transmitting. */
    if ((s->kind == K_REQ || (s->kind == K_REQ_AT && s->req_at_side == 0)) &&
        s->req == e->chain_req && e->chain_failed) {
        e->cur = *s;
        e->busy = true;
        e->rx_len = 0;
        e->rxbuf[0] = '\0';
        e->prompt = false;
        step_done(e, now, DONE_FAIL);
        return;
    }

    e->cur = *s;
    if (s->kind == K_WARM) e->cur.timeout_ms = connection_timeout(e);
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->prompt = false;
    e->busy = true;
    e->sent_ms = now;
    if (is_at) {
        const char *why = NULL;
        vp_at_ctx_t ctx = s->kind == K_REQ_AT ? VP_AT_CTX_REQUEST :
                           (s->kind == K_INIT ? VP_AT_CTX_INIT : VP_AT_CTX_ENGINE);
        if (!vp_at_check(s->cmd, ctx, norm, sizeof(norm), &why)) {
            elog(e, "AT refused: %s (%s)", s->cmd, why ? why : "?");
            step_done(e, now, DONE_FAIL);
            return;
        }
        cmd = norm;
    }
    if (s->kind == K_REQ) {
        e->req_sent = true;
        e->last_req_ms = now;
        e->req_last_ms[s->req] = now;
        e->req_seen_mask |= UINT32_C(1) << s->req;
        e->st.tx_req++;
    }
    if (s->kind == K_MA) {
        uint32_t w = e->p->mon_mode == VP_MON_CONTINUOUS ? ENG_CONT_WINDOW_MS : e->p->mon_window_ms;
        e->ma_stopping = false;
        e->ma_refused = false;
        e->ma_end_ms = now + w;
    }
    if (s->kind == K_ISO_WAIT) return;
    e->st.tx_cmds++;
    if (!e->sink.send(e->sink.ctx, cmd)) {
        elog(e, "send failed: %s", cmd);
        step_done(e, now, DONE_FAIL);
        /* A failed transport write may have delivered a partial command.
         * Reset the link before another line can be appended to that prefix. */
        if (e->phase != VP_ENGINE_PH_DOWN && e->phase != VP_ENGINE_PH_RECONNECT) {
            q_clear(e);
            e->phase = e->st.phase = VP_ENGINE_PH_RECONNECT;
            if (e->sink.reconnect) e->sink.reconnect(e->sink.ctx);
            else e->phase = e->st.phase = VP_ENGINE_PH_DOWN;
        }
    }
}

/* Monitor window in flight: false while still listening (*wait set). */
static bool ma_poll(vp_engine_t *e, uint32_t now, uint32_t *wait)
{
    if (e->prompt) {
        step_done(e, now, DONE_PROMPT);
        return true;
    }
    if ((int32_t)(e->ma_end_ms - now) > 0) {
        *wait = cap_wait(e->ma_end_ms - now);
        return false;
    }
    if (!e->ma_stopping) {
        /* any character ends ATMA; the prompt follows */
        e->ma_stopping = true;
        e->ma_end_ms = now + ENG_MA_STOP_MS;
        e->st.tx_cmds++;
        e->sink.send(e->sink.ctx, "");
        *wait = ENG_MA_STOP_MS;
        return false;
    }
    step_done(e, now, DONE_TIMEOUT);
    return true;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

static int registry_slot(const char *key)
{
    int k;
    for (k = 0; k < SIG_OBD_COUNT; k++) {
        if (!strcmp(sig_registry[SIG_OBD_FIRST + k].key, key)) return SIG_OBD_FIRST + k;
    }
    return SIG_SLOT_NONE;
}

static double unit_factor(int slot, const char *unit)
{
    const sig_def_t *d;
    uint8_t k;

    if (slot < 0 || slot >= SIG_STD_COUNT) return 1.0;
    d = &sig_registry[slot];
    if (!d->units || d->n_units == 0) return 1.0;
    if (unit && unit[0]) {
        for (k = 0; k < d->n_units; k++) {
            if (!strcmp(d->units[k].unit, unit)) return d->units[k].factor;
        }
    }
    return d->units[0].factor;
}

static void veh_range(const vp_signal_t *s, int32_t *lo, int32_t *hi)
{
    double a = -2147483648.0, b = 2147483647.0;

    if (s->flags & VP_SIG_F_VALID) {
        a = floor(s->valid_lo);
        b = ceil(s->valid_hi);
    } else if (s->flags & VP_SIG_F_CLAMP) {
        a = floor(s->clamp_lo);
        b = ceil(s->clamp_hi);
    }
    if (a < -2147483648.0) a = -2147483648.0;
    if (b > 2147483647.0) b = 2147483647.0;
    *lo = (int32_t)a;
    *hi = (int32_t)b;
}

static uint32_t veh_stale(const vp_signal_t *s)
{
    if (s->rate == VP_RATE_SLOW) return 60000u;
    if (s->period_ms > 5000u) {
        uint32_t t = 3u * s->period_ms;
        return t > 60000u ? 60000u : t;
    }
    return 15000u;
}

static void reset_runtime(vp_engine_t *e, uint32_t now)
{
    int i;

    q_clear(e);
    e->busy = false;
    e->prompt = false;
    e->rx_len = 0;
    e->rxbuf[0] = '\0';
    e->next_ms = now;
    e->hdr = ENG_ID_UNKNOWN;
    e->rx = ENG_ID_UNKNOWN;
    e->base_rx = 0;
    e->cea = ENG_ST_UNKNOWN;
    e->caf0 = ENG_ST_UNKNOWN;
    e->cfc0 = ENG_ST_UNKNOWN;
    iso_reset(e);
    e->in_round = false;
    e->functional_fallback_mask = 0;
    e->round_n = e->round_pos = 0;
    e->slot_n = e->slot_pos = 0;
    e->pending_after_req = -1;
    e->chain_req = -1;
    e->chain_failed = false;
    e->round_no = 0;
    e->req_sent = false;
    e->mon_ok = true;
    e->heal_count = 0;
    e->rpm_valid = e->speed_valid = e->gear_valid = false;
    e->gear = 0;
    e->last_data_ms = e->last_good_ms = now;
    for (i = 0; i < e->p->n_sig; i++) {
        eng_sig_t *g = &e->sigs[i];
        g->cur = 0;
        g->fails = 0;
        g->exhausted = false;
        g->fallback_on = false;
        g->can_seen = false;
        g->can_stale = false;
        g->filt_init = false;
        g->hyst_cnt = 0;
        g->hyst_pending = ENG_HYST_NONE;
        g->filt = 0.0f;
        g->last_poll_ms = now;
        g->last_can_ms = now;
    }
    memset(e->holds, 0, sizeof(e->holds));
}

vp_engine_t *vp_engine_create(const vp_profile_t *p, const vp_engine_sink_t *sink,
                              const vp_engine_cfg_t *cfg, const vp_allocator_t *alloc)
{
    vp_engine_t *e;
    int i;

    if (!p || !sink || !alloc || !alloc->alloc || !alloc->free) return NULL;
    if (!sink->send || !sink->now_ms || !sink->set_signal || !sink->invalidate ||
        !sink->set_speed_kmh || !sink->on_link) {
        return NULL;
    }
    if (p->n_sig > VP_MAX_SIGNALS || p->n_src > VP_MAX_SOURCES || p->n_req > VP_MAX_REQUESTS) {
        return NULL;
    }
    e = (vp_engine_t *)alloc->alloc(alloc->ctx, sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->p = p;
    e->req = vp_profile_requests(p);
    e->sig = vp_profile_signals(p);
    e->src = vp_profile_sources(p);
    e->can = vp_profile_can_rules(p);
    e->sink = *sink;
    if (cfg) e->cfg = *cfg;
    if (e->cfg.proto > 12) e->cfg.proto = 0;
    e->alloc = *alloc;
    e->phase = VP_ENGINE_PH_DOWN;
    e->link = SIG_LINK_NONE;
    e->mon_ok = true;
    e->sig_rpm = -1;
    e->sig_gear = -1;
    e->obd_demand_mask = (UINT16_C(1) << SIG_OBD_COUNT) - 1u;
    memset(e->req_head, ENG_INDEX_NONE, sizeof(e->req_head));
    memset(e->req_tail, ENG_INDEX_NONE, sizeof(e->req_tail));
    memset(e->req_next, ENG_INDEX_NONE, sizeof(e->req_next));

    for (i = 0; i < p->n_sig; i++) {
        const vp_signal_t *s = &e->sig[i];
        eng_sig_t *g = &e->sigs[i];

        g->slot = SIG_SLOT_NONE;
        g->factor = 1.0;
        if (s->src_first + s->src_count > p->n_src) continue;
        if (s->flags & VP_SIG_F_VEH) {
            int32_t lo, hi;
            veh_range(s, &lo, &hi);
            if (sink->veh_register) {
                int slot = sink->veh_register(sink->ctx, s->key, lo, hi, veh_stale(s));
                g->slot = (int16_t)(slot < 0 ? SIG_SLOT_NONE : slot);
            }
        } else {
            int slot = registry_slot(s->key);
            g->slot = (int16_t)slot;
            g->factor = unit_factor(slot, s->unit);
            if (slot == SIG_OBD_RPM) e->sig_rpm = (int16_t)i;
            if (slot == SIG_OBD_GEAR) e->sig_gear = (int16_t)i;
        }
        if (g->slot == SIG_SLOT_NONE) {
            elog(e, "signal %s not mapped", s->key);
            continue;
        }
        if (g->factor == 1.0) e->factor_identity_mask |= UINT64_C(1) << (unsigned)i;
        if (s->derive == VP_DERIVE_NONE) {
            uint8_t k;
            bool has_request = false;
            for (k = 0; k < s->src_count; k++) {
                uint16_t si = (uint16_t)(s->src_first + k);
                if (e->src[si].kind == VP_SRC_REQ) {
                    req_index_append(e, e->src[si].ref, si);
                    has_request = true;
                }
            }
            if (has_request && e->n_poll < VP_MAX_SIGNALS) {
                e->poll_sig[e->n_poll++] = (uint8_t)i;
            }
        }
        if (s->rate == VP_RATE_EVERY && s->derive == VP_DERIVE_NONE &&
            e->n_every < VP_MAX_EVERY_SIGNALS) {
            e->every_sig[e->n_every++] = (uint8_t)i;
        }
    }
    reset_runtime(e, sink->now_ms(sink->ctx));
    return e;
}

void vp_engine_destroy(vp_engine_t *e)
{
    vp_allocator_t a;

    if (!e) return;
    if (e->sink.veh_clear) e->sink.veh_clear(e->sink.ctx);
    a = e->alloc;
    a.free(a.ctx, e);
}

void vp_engine_link_up(vp_engine_t *e)
{
    uint32_t now;

    if (!e) return;
    now = e->sink.now_ms(e->sink.ctx);
    reset_runtime(e, now);
    set_link(e, SIG_LINK_NO_DATA);
    if (e->sink.on_elm_link) e->sink.on_elm_link(e->sink.ctx, true);
    e->phase = VP_ENGINE_PH_CHECK;
    e->st.phase = e->phase;
    q_push(e, K_DELAY, "", ENG_ELM_CHECK_DELAY);
    q_push(e, K_ELM_CHECK, "ATZ", 0);
}

void vp_engine_link_down(vp_engine_t *e)
{
    int i;

    if (!e) return;
    for (i = 0; i < e->p->n_sig; i++) {
        if (e->sigs[i].slot != SIG_SLOT_NONE) e->sink.invalidate(e->sink.ctx, e->sigs[i].slot);
    }
    e->rpm_valid = e->speed_valid = e->gear_valid = false;
    q_clear(e);
    e->busy = false;
    e->phase = VP_ENGINE_PH_DOWN;
    e->st.phase = e->phase;
    set_link(e, SIG_LINK_NONE);
    if (e->sink.on_elm_link) e->sink.on_elm_link(e->sink.ctx, false);
}

void vp_engine_set_obd_demand_mask(vp_engine_t *e, uint16_t mask)
{
    if (!e) return;
    e->obd_demand_mask = (uint16_t)(mask | (UINT16_C(1) << SIG_OBD_RPM));
}

void vp_engine_on_rx(vp_engine_t *e, const uint8_t *data, size_t len)
{
    size_t i;
    bool line = false;
    size_t used;

    if (!e || !data) return;
    used = e->rx_len;
    for (i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\0') continue;
        if (c == '>') e->prompt = true;
        if (c == '\r' || c == '\n' || c == '>') line = true;
        if (used < ENG_RX_CAP) e->rxbuf[used++] = c;
    }
    e->rx_len = (uint16_t)used;
    e->rxbuf[e->rx_len] = '\0';
    if (e->busy && e->cur.kind == K_MA && (line || e->rx_len >= ENG_RX_CAP)) {
        mon_consume(e, e->sink.now_ms(e->sink.ctx), false);
        e->rxbuf[e->rx_len] = '\0';
    }
}

uint32_t vp_engine_run(vp_engine_t *e)
{
    uint32_t now;
    int guard;

    if (!e) return ENG_WAIT_CAP;
    now = e->sink.now_ms(e->sink.ctx);
    if (e->phase == VP_ENGINE_PH_DOWN || e->phase == VP_ENGINE_PH_RECONNECT) return ENG_WAIT_CAP;
    if (e->link == SIG_LINK_DATA && elapsed(now, e->last_data_ms, link_hold(e))) {
        set_link(e, SIG_LINK_NO_DATA);
    }
    for (guard = 0; guard < 16; guard++) {
        eng_step_t s;

        if (e->busy) {
            if (e->cur.kind == K_MA) {
                uint32_t w = 1;
                if (!ma_poll(e, now, &w)) return w;
                continue;
            }
            if (e->prompt) {
                step_done(e, now, DONE_PROMPT);
            } else {
                uint32_t to = e->cur.timeout_ms ? e->cur.timeout_ms : resp_timeout(e);
                uint32_t el = now - e->sent_ms;
                if (el < to) return cap_wait(to - el);
                step_done(e, now, DONE_TIMEOUT);
            }
            continue;
        }
        if (e->phase == VP_ENGINE_PH_DOWN || e->phase == VP_ENGINE_PH_RECONNECT) return ENG_WAIT_CAP;
        if ((int32_t)(e->next_ms - now) > 0) return cap_wait(e->next_ms - now);
        if (e->q_n == 0) {
            plan(e, now);
            continue;
        }
        if ((e->q[e->q_head].kind == K_REQ || e->q[e->q_head].kind == K_ISO_FC) && e->req_sent &&
            !elapsed(now, e->last_req_ms, e->p->min_req_ms)) {
            return cap_wait(e->p->min_req_ms - (now - e->last_req_ms));
        }
        q_pop(e, &s);
        if (s.kind == K_DELAY) {
            e->next_ms = now + s.delay_ms;
            continue;
        }
        step_send(e, &s, now);
    }
    return 1;
}

void vp_engine_get_stats(const vp_engine_t *e, vp_engine_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!e) return;
    *out = e->st;
    out->proto = e->run_proto;
    out->phase = e->phase;
    out->link = e->link;
    out->mon_ok = e->mon_ok;
}
