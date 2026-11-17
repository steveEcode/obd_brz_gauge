#pragma once
/*
 * vp_engine - runs a compiled vehicle profile (vp_profile_t) against an
 * ELM327: AT bring-up, request scheduling, response matching, decoding and
 * signal writes.
 *
 * Pure C, no ESP-IDF / FreeRTOS: time, transport and outputs are injected
 * through vp_engine_sink_t, so the same code runs in the firmware (driven by
 * the ELM327 BLE client task) and in the host tests (test/host/engine).
 *
 * Threading: NOT thread-safe. One task owns the engine and calls every
 * function below; the ELM327 polling stays a single-threaded, prompt-gated
 * state machine (one command in flight, the next one only after '>' or a
 * timeout).
 *
 * Driving loop (firmware):
 *     link_up() once the adapter's notify subscription is ready;
 *     loop: feed received bytes with on_rx(), then wait = run();
 *           sleep up to `wait` ms or until more bytes arrive;
 *     link_down() when the BLE link drops.
 *
 * Behaviour (contracts/vehicle-rpn.md section 3 is the decode pipeline):
 *  - bring-up: legacy profiles use the Car Scanner-style common setup
 *    (ATZ ATD ATD0 ATE0 ATH1 ATSPx ATM0 ATS0 ATAT1 ATAL), followed by profile-specific
 *    bus.init commands and one configurable check_connection request (0100
 *    by default).  A translated InitSequence that
 *    contains a reset, ATSP and all common setup commands owns the complete
 *    setup order and is sent verbatim (with DELAY/WAIT tokens represented as
 *    local waits); partial reset-bearing sequences are completed after the
 *    profile commands;
 *  - protocol auto-detect probes the supported ELM protocol numbers in a
 *    deterministic order and reuses the accepted protocol on reconnect;
 *    Auto re-probes after sustained decode failures and retains the user's
 *    Automatic selection instead of persisting a detected value as a lock;
 *    ATSP0 in profile init retains an already selected concrete protocol;
 *    otherwise ATDPN resolves adapter-selected protocols before polling;
 *  - rounds: the current request of every due signal (round / slow every
 *    div rounds / period_ms), each distinct request once, in signal order;
 *    "every" requests run before each slot's regular request to minimize
 *    their latency; gap_ms after each request; responsive_polling instead
 *    services demanded RPM/speed/boost/throttle before background slots
 *    (RPM/speed default to 50/100 ms; pressure/throttle run every batch),
 *    with one bounded extra RPM refresh per slot;
 *    explicit periods and slow boost/throttle rates are retained; failed
 *    requests back off up to 16 s and shared requests are deduplicated;
 *  - standard 11-bit OBD request addresses are mapped to their 29-bit
 *    equivalents on protocols 7/9, together with standard response filters;
 *    legacy protocols 1..5 keep native addressing;
 *  - a Mode 01 NRC 0x11 / 0x12 on a standard physical CAN address retries
 *    once on the protocol's functional OBD address;
 *    on_demand is polled like round;
 *  - ATSH / ATCRA / ATCEA / ATCAF0 are sent only when the adapter state
 *    differs from what the next request needs (lazy restore);
 *  - monitor probe windows (ATS1 ATH1 ATMA ... CR, ATAR, ATH0 ATS0) after a round when
 *    every_ms has elapsed; continuous mode re-enters ATMA every 5 s;
 *  - NO DATA, ?, STOPPED, CAN ERROR, negative responses and timeouts are
 *    sample failures; fail_after failures move to the next source;
 *    timed-out steps interrupt and drain to '>' before queued work resumes;
 *    a failed transport write requests reconnect before sending another line;
 *  - obd.gear derive/fallback "ratio" from rpm and speed with hysteresis;
 *  - sys.obd_link: 1 from link_up, 2 right before the first decoded write,
 *    back to 1 after link_hold_ms without a decode, 0 on link_down;
 *  - self-heal: heal_ms without a decode re-runs the bring-up; the third
 *    heal in a row asks the transport to reconnect.
 * Every generated AT command and profile init command goes through the
 * corresponding vp_at_check context before it is sent; refused commands are
 * dropped.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vp_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vp_engine vp_engine_t;

typedef struct {
    void *ctx;
    /* Write one command line; the transport appends "\r". cmd is "" to stop
     * an ATMA monitor. false = could not send (counts as a failed command). */
    bool     (*send)(void *ctx, const char *cmd);
    uint32_t (*now_ms)(void *ctx);
    /* signal_store writes (slot = signal_store slot). obd.speed goes through
     * set_speed_kmh instead of set_signal. */
    void     (*set_signal)(void *ctx, int slot, int32_t internal);
    void     (*invalidate)(void *ctx, int slot);
    void     (*set_speed_kmh)(void *ctx, uint8_t kmh);
    /* sys.obd_link value (SIG_LINK_*). */
    void     (*on_link)(void *ctx, uint8_t state);
    /* veh.* registration; returns the slot or SIG_SLOT_NONE. */
    int      (*veh_register)(void *ctx, const char *key, int32_t min, int32_t max,
                             uint32_t stale_ms);
    void     (*veh_clear)(void *ctx);
    /* vp_manager verification hooks. */
    void     (*on_response)(void *ctx, int slot);
    void     (*on_voltage)(void *ctx, float volts);
    void     (*on_elm_link)(void *ctx, bool up);
    /* Optional (may be NULL). */
    void     (*on_proto_detected)(void *ctx, uint8_t proto); /* session result; must not overwrite user's Auto selection */
    void     (*reconnect)(void *ctx);                        /* drop and reconnect the transport */
    void     (*log)(void *ctx, const char *msg);
} vp_engine_sink_t;

typedef struct {
    uint8_t  proto;              /* ATSP when the profile's bus.proto is 0: 1..12; 0 = auto-detect */
    const uint8_t *detect_protocols; /* optional caller-owned ATSP candidates; NULL = engine default */
    uint8_t  detect_protocol_count;  /* number of entries in detect_protocols */
    uint16_t resp_timeout_ms;    /* 0 = 5000 */
    uint16_t detect_timeout_ms;  /* connection probe/warm check; 0 = 5000 for all protocols */
    uint16_t link_hold_ms;       /* 0 = 3000 */
    uint16_t vbat_every_ms;      /* ATRV period, 0 = 5000 */
    uint16_t heal_ms;            /* 0 = 10000 */
    bool responsive_polling;    /* responsive-input deadlines + failed-request backoff; enabled by firmware */
} vp_engine_cfg_t;

typedef struct {
    uint32_t tx_cmds;      /* every command written */
    uint32_t tx_req;       /* OBD requests written */
    uint32_t rx_ok;        /* requests answered with a matching response */
    uint32_t rx_fail;      /* NO DATA / no match / negative / timeout */
    uint32_t timeouts;
    uint32_t heals;
    uint32_t mon_windows;
    uint32_t can_frames;   /* monitor lines that parsed as frames */
    uint32_t rounds;
    uint8_t  proto;        /* protocol in use (after detection) */
    uint8_t  phase;        /* VP_ENGINE_PH_* */
    uint8_t  link;         /* SIG_LINK_* */
    bool     mon_ok;       /* false after ATMA answered '?' */
} vp_engine_stats_t;

enum {
    VP_ENGINE_PH_DOWN = 0,
    VP_ENGINE_PH_CHECK,    /* verify ELM327 prompt after transport connect */
    VP_ENGINE_PH_DETECT,
    VP_ENGINE_PH_INIT,
    VP_ENGINE_PH_RUN,
    VP_ENGINE_PH_RECONNECT,   /* waiting for the transport after a reconnect request */
};

/*
 * Bind a profile. Registers the profile's veh.* signals through the sink.
 * All state (including the 5120-byte response buffer) is one allocation from
 * alloc. Returns NULL on bad arguments or out of memory. p must outlive the
 * engine.
 */
vp_engine_t *vp_engine_create(const vp_profile_t *p, const vp_engine_sink_t *sink,
                              const vp_engine_cfg_t *cfg, const vp_allocator_t *alloc);
/* veh_clear() through the sink, then free. NULL is a no-op. */
void vp_engine_destroy(vp_engine_t *e);

/* Adapter reachable (notify ready) / gone. */
void vp_engine_link_up(vp_engine_t *e);
void vp_engine_link_down(vp_engine_t *e);

/* Limit standard obd.* request polling to these signal slots. RPM is always
 * enabled; registered veh.* signals remain enabled for multi-gauge broadcast.
 * The engine remains single-owner: call this from the task that runs it. */
void vp_engine_set_obd_demand_mask(vp_engine_t *e, uint16_t mask);

/* Bytes received from the adapter (any chunking). */
void vp_engine_on_rx(vp_engine_t *e, const uint8_t *data, size_t len);

/* Advance the state machine (may call sink->send). Returns the number of ms
 * until the next call is useful (1..100); call earlier when bytes arrive. */
uint32_t vp_engine_run(vp_engine_t *e);

void vp_engine_get_stats(const vp_engine_t *e, vp_engine_stats_t *out);

/* Gear from rpm and speed (exposed for tests). total = rpm * 0.377 * tire_m /
 * speed is compared with gears[i] * fd * (1 +- tol); the current gear is kept
 * while it still matches. Returns 0 (N) for rpm/speed <= 0 or rpm > 800 with
 * speed < 5 and no match, cur when nothing matches, else 1..gear_count. */
int8_t vp_engine_gear_from_ratio(const vp_profile_t *p, float rpm, float speed_kmh, int8_t cur);

#ifdef __cplusplus
}
#endif
