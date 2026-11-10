#include "tr_eval.h"
#include <math.h>
#include <string.h>

#if TR_MAX_RULES > 32
#error "resolved rule mask requires TR_MAX_RULES <= 32"
#endif

// A JS operand: current is always a number (NaN when invalid) or undefined
// (unknown key); a threshold may also be null or another JSON type.
typedef struct {
    uint8_t tag;   // tr_val_tag_t
    double  num;   // relational value
} operand_t;

static operand_t fetch(uint16_t src, const tr_inputs_t *in)
{
    operand_t o;
    if (src == TR_SRC_NONE) {
        o.tag = TR_VAL_UNDEF;
        o.num = NAN;
        return o;
    }
    o.tag = TR_VAL_NUM;
    if (!in->values || src >= in->n_values ||
        (src < TR_MASK_SOURCES && ((in->invalid_mask >> src) & 1u))) {
        o.num = NAN;
    } else {
        o.num = in->values[src];
    }
    return o;
}

// `===`
static bool strict_eq(operand_t a, operand_t b)
{
    if (a.tag != b.tag) return false;
    if (a.tag == TR_VAL_NUM) return a.num == b.num;
    return a.tag == TR_VAL_UNDEF || a.tag == TR_VAL_NULL;
}

static bool raw_cmp(uint8_t op, operand_t c, operand_t t)
{
    switch (op) {
    case TR_OP_GT: return c.num > t.num;
    case TR_OP_GE: return c.num >= t.num;
    case TR_OP_LT: return c.num < t.num;
    case TR_OP_LE: return c.num <= t.num;
    case TR_OP_EQ: return strict_eq(c, t);
    case TR_OP_NE: return !strict_eq(c, t);
    default:       return false;
    }
}

// conditionState(): first evaluation is raw; ==/!= ignore the band;
// h <= 0 returns raw without touching the latch (NaN h does not).
static bool cond_state(const tr_cond_t *k, const tr_inputs_t *in, int8_t *latch, double h)
{
    operand_t c = fetch(k->src, in);
    operand_t t;
    if (k->has_cmp) {
        t = fetch(k->cmp_src, in);
    } else {
        t.tag = k->value_tag;
        t.num = k->value;
    }
    bool raw = raw_cmp(k->op, c, t);
    if (h <= 0) return raw;

    if (*latch < 0) {
        *latch = raw ? 1 : 0;
        return raw;
    }

    bool dir = k->op == TR_OP_GT || k->op == TR_OP_GE || k->op == TR_OP_LT || k->op == TR_OP_LE;
    bool up = k->op == TR_OP_GT || k->op == TR_OP_GE;
    if (*latch == 1) {
        bool released = dir ? (up ? c.num < t.num - h : c.num > t.num + h) : !raw;
        *latch = released ? 0 : 1;
    } else {
        bool engaged = dir ? (up ? c.num > t.num + h : c.num < t.num - h) : raw;
        *latch = engaged ? 1 : 0;
    }
    return *latch == 1;
}

static bool rule_active(tr_program_t *p, const tr_rule_t *r, const tr_inputs_t *in, uint64_t high)
{
    if (r->disabled) return false;
    if (r->trigger == TR_TRIG_TIMER) {
        double e = in->elapsed_ms;
        return e > 0 && fmod(e, r->interval_ms) < 120.0;
    }
    if (r->trigger == TR_TRIG_CHANGE) {
        if (r->change_src < 64) return (in->changed_mask >> r->change_src) & 1u;
        return r->change_src < 128 && ((high >> (r->change_src - 64)) & 1u);
    }
    if (!r->n_conds) return false;

    // every condition is evaluated (no short-circuit) so all latches advance
    bool acc = !r->any;
    for (uint8_t k = 0; k < r->n_conds; k++) {
        bool s = cond_state(&r->conds[k], in, &p->state.latch[r->slot][k], r->hysteresis);
        acc = r->any ? (acc || s) : (acc && s);
    }
    return acc;
}

// SortCompare: a NaN comparator result counts as +0 (ECMA-262, and V8's
// SortCompareUserFn), e.g. a string priority or Infinity - Infinity.
static double prio_cmp(const tr_program_t *p, uint8_t a, uint8_t b)
{
    double v = p->rules[a].priority - p->rules[b].priority;
    return isnan(v) ? 0.0 : v;
}

// Array.prototype.sort as V8 runs it for n < 64 (one TimSort run:
// CountAndMakeRun + BinaryInsertionSort) with the editor's comparator
// (a.priority ?? 0) - (b.priority ?? 0). Stable ascending for numeric
// priorities; also reproduces V8's order when the comparator yields NaN.
static void sort_active(const tr_program_t *p, uint8_t *v, uint8_t n)
{
    if (n < 2) return;

    uint8_t run = 2;
    bool desc = prio_cmp(p, v[1], v[0]) < 0;
    uint8_t prev = v[1];
    for (uint8_t i = 2; i < n; i++) {
        double order = prio_cmp(p, v[i], prev);
        if (desc ? (order >= 0) : (order < 0)) break;
        prev = v[i];
        run++;
    }
    if (desc) {
        for (uint8_t lo = 0, hi = (uint8_t)(run - 1); lo < hi; lo++, hi--) {
            uint8_t tmp = v[lo];
            v[lo] = v[hi];
            v[hi] = tmp;
        }
    }

    for (uint8_t start = run; start < n; start++) {
        uint8_t left = 0, right = start, pivot = v[start];
        while (left < right) {
            uint8_t mid = (uint8_t)(left + ((right - left) >> 1));
            if (prio_cmp(p, pivot, v[mid]) < 0) right = mid;
            else left = (uint8_t)(mid + 1);
        }
        for (uint8_t q = start; q > left; q--) v[q] = v[q - 1];
        v[left] = pivot;
    }
}

void tr_reset(tr_program_t *prog)
{
    if (!prog) return;
    memset(prog->state.latch, 0xFF, sizeof(prog->state.latch));
    memset(prog->state.deb_armed, 0, sizeof(prog->state.deb_armed));
    for (int i = 0; i < TR_MAX_RULES; i++) prog->state.deb_start[i] = 0.0;
    memset(prog->state.animation_start, 0, sizeof(prog->state.animation_start));
    memset(prog->state.hold_until, 0, sizeof(prog->state.hold_until));
    memset(prog->state.animation_armed, 0, sizeof(prog->state.animation_armed));
    memset(prog->state.active_armed, 0, sizeof(prog->state.active_armed));
    memset(prog->state.opacity_exit_armed, 0, sizeof(prog->state.opacity_exit_armed));
    memset(prog->state.opacity_exit_start, 0, sizeof(prog->state.opacity_exit_start));
    if (prog->n_targets) memset(prog->ovr, 0, prog->n_targets * sizeof(tr_override_t));
    prog->resolved_mask = 0;
    prog->resolved_exit_mask = 0;
    prog->have_resolved = false;
    memset(&prog->result, 0, sizeof(prog->result));
    prog->result.n_targets = prog->n_targets;
    prog->result.ovr = prog->ovr;
}

static double anim_progress(const tr_animation_t *a, double elapsed)
{
    double duration = a->duration_ms > 0.0 ? a->duration_ms : 1000.0;
    if (a->mode == 3) {
        double phase = fmod(fmax(0.0, elapsed) * a->frequency_hz / 1000.0, 1.0);
        return phase < 0.5 ? 1.0 : 0.0;
    }
    double t = fmax(0.0, fmin(1.0, elapsed / duration));
    if (a->mode != 2 || a->n_frames == 0) return t;
    if (t <= a->frame_at[0]) return a->frame_value[0];
    for (uint8_t i = 1; i < a->n_frames; i++) {
        if (t <= a->frame_at[i]) {
            double span = a->frame_at[i] - a->frame_at[i - 1];
            if (span <= 0.0) return a->frame_value[i];
            double part = (t - a->frame_at[i - 1]) / span;
            return a->frame_value[i - 1] + (a->frame_value[i] - a->frame_value[i - 1]) * part;
        }
    }
    return a->frame_value[a->n_frames - 1];
}

static bool rule_has_animated_opacity(const tr_rule_t *rule)
{
    for (uint8_t i = 0; i < rule->n_actions; i++) {
        const tr_action_t *action = &rule->actions[i];
        if ((action->props & TR_PROP_OPACITY) && action->animation.mode && action->animation.mode != 3)
            return true;
    }
    return false;
}

bool tr_rule_active(tr_program_t *prog, uint8_t rule_idx, const tr_inputs_t *in)
{
    if (!prog || !in || rule_idx >= prog->n_rules) return false;
    return rule_active(prog, &prog->rules[rule_idx], in, 0);
}

const tr_result_t *tr_eval(tr_program_t *prog, const tr_inputs_t *in)
{
    return tr_eval_extended(prog, in, 0);
}

const tr_result_t *tr_eval_extended(tr_program_t *prog, const tr_inputs_t *in, uint64_t changed_high)
{
    if (!prog || !in) return NULL;
    tr_state_t *st = &prog->state;
    tr_result_t *res = &prog->result;

    // page order; a miss clears only the debounce timer, never the latches
    uint32_t active_mask = 0;
    uint32_t opacity_exit_mask = 0;
    bool animated_active = false;
    for (uint8_t i = 0; i < prog->n_rules; i++) {
        const tr_rule_t *r = &prog->rules[i];
        uint8_t s = r->slot;
        bool matched = rule_active(prog, r, in, changed_high);
        if (matched && r->transient_hold_ms > 0.0 &&
            (r->trigger == TR_TRIG_TIMER || r->trigger == TR_TRIG_CHANGE)) {
            st->hold_until[s] = in->elapsed_ms + r->transient_hold_ms;
        }
        bool candidate = matched ||
            (r->transient_hold_ms > 0.0 &&
             (r->trigger == TR_TRIG_TIMER || r->trigger == TR_TRIG_CHANGE) &&
             st->hold_until[s] > 0.0 &&
             in->elapsed_ms < st->hold_until[s]);
        if (!candidate) {
            st->deb_armed[s] = 0;
            st->animation_armed[s] = 0;
            if (st->active_armed[s]) {
                st->active_armed[s] = 0;
                if (rule_has_animated_opacity(r)) {
                    st->opacity_exit_armed[s] = 1;
                    st->opacity_exit_start[s] = in->elapsed_ms;
                }
            }
            if (st->opacity_exit_armed[s]) {
                double duration = 0.0;
                for (uint8_t a = 0; a < r->n_actions; a++) {
                    const tr_action_t *action = &r->actions[a];
                    if ((action->props & TR_PROP_OPACITY) && action->animation.mode && action->animation.mode != 3 &&
                        action->animation.duration_ms > duration)
                        duration = action->animation.duration_ms;
                }
                if (in->elapsed_ms - st->opacity_exit_start[s] < duration) {
                    opacity_exit_mask |= UINT32_C(1) << i;
                    animated_active = true;
                } else {
                    st->opacity_exit_armed[s] = 0;
                }
            }
            continue;
        }
        if (!st->deb_armed[s]) {
            st->deb_armed[s] = 1;
            st->deb_start[s] = in->elapsed_ms;
        }
        if (in->elapsed_ms - st->deb_start[s] >= r->debounce_ms) {
            active_mask |= UINT32_C(1) << i;
            st->active_armed[s] = 1;
            st->opacity_exit_armed[s] = 0;
            bool rule_animated = false;
            for (uint8_t a = 0; a < r->n_actions; a++) {
                if (r->actions[a].animation.mode &&
                    ((r->actions[a].props & TR_PROP_ANY_COLOR) ||
                     ((r->actions[a].props & TR_PROP_OPACITY) && r->actions[a].animation.mode != 3))) {
                    rule_animated = true;
                    break;
                }
            }
            if (rule_animated) {
                if (!st->animation_armed[s]) {
                    st->animation_start[s] = in->elapsed_ms;
                    st->animation_armed[s] = 1;
                }
                animated_active = true;
            }
        }
    }

    // The final active set uniquely determines the sort input and immutable
    // actions. Keep evaluating state above even when the resolved output can
    // be reused (shared IDs, timer windows and debounce depend on each tick).
    if (!animated_active && prog->have_resolved && prog->resolved_mask == active_mask &&
        prog->resolved_exit_mask == opacity_exit_mask) return res;

    uint8_t n = 0;
    for (uint8_t i = 0; i < prog->n_rules; i++) {
        if (active_mask & (UINT32_C(1) << i)) res->active[n++] = i;
    }
    res->n_active = n;
    sort_active(prog, res->active, n);

    uint8_t apply_order[TR_MAX_RULES];
    uint8_t n_apply = 0;
    for (uint8_t i = 0; i < prog->n_rules; i++) {
        if ((active_mask | opacity_exit_mask) & (UINT32_C(1) << i)) apply_order[n_apply++] = i;
    }
    sort_active(prog, apply_order, n_apply);

    if (prog->n_targets) memset(prog->ovr, 0, prog->n_targets * sizeof(tr_override_t));
    for (uint8_t k = 0; k < n_apply; k++) {
        uint8_t ri = apply_order[k];
        const tr_rule_t *r = &prog->rules[ri];
        bool exiting = (opacity_exit_mask & (UINT32_C(1) << ri)) != 0;
        for (uint8_t j = 0; j < r->n_actions; j++) {
            const tr_action_t *a = &r->actions[j];
            if (exiting && !(a->props & TR_PROP_OPACITY)) continue;
            tr_override_t *o = &prog->ovr[a->target];
            o->touched = 1;
            if (!exiting && (a->props & TR_PROP_ANY_COLOR)) {
                o->props |= (uint8_t)(a->props & TR_PROP_ANY_COLOR);
                o->color = a->color;
                o->color_str = a->color_str;
                if (a->animation.mode) {
                    o->animated_color = 1;
                    o->animation_progress = anim_progress(&a->animation,
                        in->elapsed_ms - st->animation_start[r->slot]);
                } else {
                    o->animated_color = 0;
                    o->animation_progress = 1.0;
                }
            }
            if (!exiting && (a->props & TR_PROP_TEXT)) {
                o->props |= TR_PROP_TEXT;
                o->text = a->text;
            }
            if (!exiting && (a->props & TR_PROP_VISIBLE)) {
                o->props |= TR_PROP_VISIBLE;
                o->visible = a->visible;
            }
            if (!exiting && (a->props & TR_PROP_ASSET)) {
                o->props |= TR_PROP_ASSET;
                o->asset = a->asset;
            }
            if (a->props & TR_PROP_OPACITY) {
                double progress = 1.0;
                if (a->animation.mode && a->animation.mode != 3) {
                    if (exiting) {
                        double initial = anim_progress(&a->animation,
                            st->opacity_exit_start[r->slot] - st->animation_start[r->slot]);
                        double exit = anim_progress(&a->animation,
                            in->elapsed_ms - st->opacity_exit_start[r->slot]);
                        progress = initial * (1.0 - exit);
                    } else {
                        progress = anim_progress(&a->animation,
                            in->elapsed_ms - st->animation_start[r->slot]);
                    }
                }
                o->props |= TR_PROP_OPACITY;
                o->opacity = a->opacity;
                o->opacity_progress = progress;
                o->animated_opacity = (a->animation.mode && a->animation.mode != 3 &&
                                       (exiting || progress < 1.0));
            }
        }
    }
    res->n_targets = prog->n_targets;
    res->ovr = prog->ovr;
    prog->resolved_mask = active_mask;
    prog->resolved_exit_mask = opacity_exit_mask;
    prog->have_resolved = true;
    return res;
}

const tr_override_t *tr_result_override(const tr_program_t *prog, uint16_t target)
{
    if (!prog || target >= prog->n_targets || !prog->ovr[target].touched) return NULL;
    return &prog->ovr[target];
}
