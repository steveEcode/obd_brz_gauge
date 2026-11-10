#pragma once
// ============================================================
//  tr_types.h — theme rules core: shared types
//
//  Pure C99, no ESP-IDF / LVGL dependency, host-compilable.
//  Rule semantics follow the editor's ruleEngine.ts bit for bit
//  (obd_theme_store@5a7484e, frontend/src/lib/ruleEngine.ts,
//  sha256 d8288789...). See REFACTOR_PLAN.md 5.4.
//
//  A compiled program is ONE allocation from the injected
//  allocator (firmware: PSRAM), holding the rules, the string
//  pool, the per-page runtime state and the result buffers.
//  Nothing here uses static storage.
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Editor limits (themeContract.ts THEME_LIMITS). Extra entries are
// dropped at compile time and counted in tr_diag_t.
#define TR_MAX_RULES        16
#define TR_MAX_CONDS        4
#define TR_MAX_ACTIONS      8
#define TR_MAX_KEYFRAMES    6
// Legacy tr_eval()/tr_rule_active() change triggers use a 64-bit mask.
// tr_eval_extended() additionally accepts sources 64..127 in a high word.
#define TR_MASK_SOURCES     64

#define TR_SRC_NONE         0xFFFFu   // unknown source key (JS `undefined`)
#define TR_ELEM_NONE        0xFFFFu   // target not in the element table

// Preview tints images with BlendColor alpha 0.75 → LVGL recolor_opa.
#define TR_RECOLOR_OPA      191

typedef enum {
    TR_OK = 0,
    TR_ERR_ARG,
    TR_ERR_NOMEM,
    TR_ERR_JSON,
} tr_err_t;

typedef enum {
    TR_KIND_UNKNOWN = 0,   // unknown kind string or target missing
    TR_KIND_ARC,
    TR_KIND_BAR,
    TR_KIND_LABEL,
    TR_KIND_IMAGE,
    TR_KIND_POINTER,
} tr_kind_t;

typedef enum {
    TR_OP_NONE = 0,        // missing / unknown op: always false
    TR_OP_GT,
    TR_OP_GE,
    TR_OP_LT,
    TR_OP_LE,
    TR_OP_EQ,
    TR_OP_NE,
} tr_op_t;

typedef enum {
    TR_TRIG_CONDITION = 0, // anything but exactly "timer" / "change"
    TR_TRIG_TIMER,
    TR_TRIG_CHANGE,
} tr_trigger_t;

// Override property bits. A `color` action maps by target kind:
// arc/bar → indicator COLOR, image/pointer → TINT, others → COLOR.
// BG_COLOR is reserved for future explicit track-color actions.
enum {
    TR_PROP_COLOR    = 1u << 0,   // arc/bar indicator, label text color
    TR_PROP_BG_COLOR = 1u << 1,   // arc/bar track
    TR_PROP_TINT     = 1u << 2,   // image/pointer recolor (TR_RECOLOR_OPA)
    TR_PROP_TEXT     = 1u << 3,   // static labels only (apply layer decides)
    TR_PROP_VISIBLE  = 1u << 4,
    TR_PROP_ASSET    = 1u << 5,
    TR_PROP_OPACITY  = 1u << 6,
};
#define TR_PROP_ANY_COLOR (TR_PROP_COLOR | TR_PROP_BG_COLOR | TR_PROP_TINT)

// JS type of a compiled `value`. `===` must tell these apart:
// undefined === undefined is true, NaN === NaN is false, null equals nothing.
typedef enum {
    TR_VAL_NUM = 0,
    TR_VAL_UNDEF,          // key absent
    TR_VAL_NULL,           // relational: 0
    TR_VAL_OTHER,          // bool/string/object: relational ToNumber, never ===
} tr_val_tag_t;

// Injectable allocator. NULL calloc_fn → stdlib calloc/free.
typedef struct {
    void *(*calloc_fn)(size_t n, size_t size, void *ctx);
    void (*free_fn)(void *ptr, void *ctx);
    void *ctx;
} tr_alloc_t;

// Element table entry supplied by the caller (id → kind).
typedef struct {
    const char *id;
    tr_kind_t kind;
} tr_element_t;

// Source key → index. Return < 0 for unknown keys (evaluated as JS undefined).
typedef int (*tr_resolve_fn)(const char *key, void *ctx);

typedef struct {
    uint16_t src;          // TR_SRC_NONE: unknown key
    uint16_t cmp_src;      // used when has_cmp
    uint8_t  op;           // tr_op_t
    uint8_t  has_cmp;      // compareSource was truthy: threshold = cmp_src
    uint8_t  value_tag;    // tr_val_tag_t
    double   value;        // `value` coerced for relational ops
} tr_cond_t;

typedef struct {
    uint8_t mode;          // 0=none, 1=fade, 2=keyframes, 3=strobe
    uint8_t n_frames;
    double duration_ms;
    double frequency_hz;
    double frame_at[TR_MAX_KEYFRAMES];
    double frame_value[TR_MAX_KEYFRAMES];
} tr_animation_t;

typedef struct {
    uint16_t target;       // index into tr_program_t.targets
    uint8_t  props;        // TR_PROP_* this action writes
    uint8_t  visible;      // set.visible !== false
    uint32_t color;        // 0xRRGGBB
    const char *color_str; // editor string, e.g. "#ff3158"
    const char *text;
    const char *asset;
    double opacity;          // 0..100
    tr_animation_t animation;
} tr_action_t;

typedef struct {
    const char *id;        // non-string ids compile to ""
    uint8_t  trigger;      // tr_trigger_t
    uint8_t  disabled;     // enabled === false
    uint8_t  any;          // combine === "any"
    uint8_t  slot;         // runtime slot; rules sharing an id share it (editor Map keys)
    uint8_t  n_conds;
    uint8_t  n_actions;
    uint16_t change_src;   // trigger.source ?? when.source ?? "obd.rpm"
    double   interval_ms;  // Math.max(100, intervalMs ?? 1000)
    double   hysteresis;   // when.hysteresis ?? 0
    double   debounce_ms;  // Math.max(0, debounceMs ?? 0)
    double   priority;     // priority ?? 0
    double   transient_hold_ms;
    const tr_cond_t   *conds;
    const tr_action_t *actions;
} tr_rule_t;

typedef struct {
    const char *id;
    uint16_t elem;         // element table index, TR_ELEM_NONE if absent
    uint8_t  kind;         // tr_kind_t of that element
} tr_target_t;

// Desired state of one target after a tick, recomputed from base.
typedef struct {
    uint8_t  touched;      // some active action named this target
    uint8_t  props;        // TR_PROP_* present
    uint8_t  visible;      // valid with TR_PROP_VISIBLE
    uint32_t color;        // valid with any TR_PROP_ANY_COLOR bit
    const char *color_str;
    const char *text;      // valid with TR_PROP_TEXT
    const char *asset;     // valid with TR_PROP_ASSET
    double opacity;        // valid with TR_PROP_OPACITY, 0..100
    double opacity_progress; // 0=base opacity, 1=action opacity
    double animation_progress; // 0..1 for the winning animated color action
    uint8_t animated_color;
    uint8_t animated_opacity;
} tr_override_t;

typedef struct {
    const double *values;  // by source index; NaN = invalid
    uint16_t n_values;     // indices >= n_values read as NaN
    uint64_t invalid_mask; // bit i set → source i reads as NaN
    uint64_t changed_mask; // bit i set → source i inside its 280 ms change window
    double   elapsed_ms;   // monotonic ms since page build, no wrap
} tr_inputs_t;

typedef struct {
    uint8_t  n_active;
    uint8_t  active[TR_MAX_RULES];  // rule indices in apply order
    uint16_t n_targets;
    const tr_override_t *ovr;       // [n_targets], parallel to program targets
} tr_result_t;

typedef struct {
    int8_t   latch[TR_MAX_RULES][TR_MAX_CONDS];  // -1 unset, else 0/1
    uint8_t  deb_armed[TR_MAX_RULES];
    double   deb_start[TR_MAX_RULES];
    double   animation_start[TR_MAX_RULES];
    double   hold_until[TR_MAX_RULES];
    uint8_t  animation_armed[TR_MAX_RULES];
    uint8_t  active_armed[TR_MAX_RULES];
    uint8_t  opacity_exit_armed[TR_MAX_RULES];
    double   opacity_exit_start[TR_MAX_RULES];
} tr_state_t;

typedef struct {
    uint16_t rules_in;         // length of rules[]
    uint16_t rules_dropped;    // beyond TR_MAX_RULES
    uint16_t conds_dropped;    // beyond TR_MAX_CONDS
    uint16_t actions_dropped;  // beyond TR_MAX_ACTIONS
    uint16_t actions_invalid;  // action without a string target
    uint16_t bad_values;       // set.color unparseable, text/asset not a string
    uint16_t unknown_sources;  // keys the resolver did not know
    uint16_t unknown_targets;  // targets absent from the element table
    uint16_t shared_ids;       // rules whose id repeats an earlier rule's
    uint16_t change_unmasked;  // change source index >= TR_MASK_SOURCES
} tr_diag_t;

typedef struct tr_program {
    uint8_t  n_rules;
    uint16_t n_targets;
    const tr_rule_t   *rules;    // page order
    const tr_target_t *targets;  // first-reference order
    tr_override_t     *ovr;      // result storage
    tr_state_t   state;
    tr_result_t  result;
    tr_alloc_t   alloc;
    // Compiled rules/actions are immutable. Cache only the resolved output;
    // latches, debounce and triggers still advance on every evaluation.
    uint32_t resolved_mask;
    uint32_t resolved_exit_mask;
    bool     have_resolved;
} tr_program_t;

#ifdef __cplusplus
}
#endif
