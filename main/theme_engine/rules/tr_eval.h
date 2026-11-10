#pragma once
// ============================================================
//  tr_eval.h — evaluate a compiled rules program
//
//  Mirrors evaluatePageRules() / isRuleActive() of the editor's
//  ruleEngine.ts with a persistent runtime (the editor's sticky
//  Map): per-condition directional hysteresis latch, debounce as
//  on-delay and priority-ascending stable order. Overrides are resolved
//  from base when the active rule set changes; unchanged sets reuse output.
// ============================================================

#include "tr_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Clear latches, debounce timers and the output cache (page build / theme reload).
void tr_reset(tr_program_t *prog);

// One tick. Evaluates every rule in page order, updates the runtime and
// returns the override table (owned by prog, valid until the next call).
// Rules/actions stay immutable between resets; callers must not edit results.
const tr_result_t *tr_eval(tr_program_t *prog, const tr_inputs_t *in);
/* Firmware extension: change triggers for sources 64..127. High sources use
 * NAN for invalidity; legacy tr_inputs_t / tr_eval semantics stay unchanged. */
const tr_result_t *tr_eval_extended(tr_program_t *prog, const tr_inputs_t *in,
                                     uint64_t changed_high);

// isRuleActive() for one rule: trigger/conditions only. Updates that rule's
// latches, never its debounce state.
bool tr_rule_active(tr_program_t *prog, uint8_t rule_idx, const tr_inputs_t *in);

// Override of a target, or NULL when no active action touched it.
const tr_override_t *tr_result_override(const tr_program_t *prog, uint16_t target);

#ifdef __cplusplus
}
#endif
