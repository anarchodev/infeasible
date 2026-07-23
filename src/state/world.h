#ifndef INF_STATE_WORLD_H
#define INF_STATE_WORLD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/intern.h"
#include "logic/dl.h"
#include "logic/dl_col.h"    /* lane families are dl_col N-entity schemas */

/* World = base facts (the only mutable state) + rules.
 *
 * - Fluents are ground boolean atoms, closed-world: every declared fluent is
 *   asserted as f or ~f in each evaluation.
 * - Judgment rules derive conclusions from the current state; conclusions are
 *   never stored (invariant I1 in DESIGN.md).
 * - Step rules (actions and ramifications) are the only way facts change.
 *   A step builds a primed-atom theory with generated inertia rules and
 *   reads off the next state (DESIGN.md 5.3). */

typedef struct world world;

world *world_new(intern *syms);
void   world_free(world *w);

void world_declare_fluent(world *w, uint32_t atom);
/* Where a fluent was declared (a "srcname:line" span, §6.3), attached to its
 * generated inertia rules' provenance. Copied; NULL clears it. */
void world_set_fluent_prov(world *w, uint32_t atom, const char *at);
void world_set(world *w, uint32_t atom, bool value);   /* initial state / loading */
bool world_get(const world *w, uint32_t atom);

/* Numeric fluents (DESIGN.md §5.8): an integer value store, kept separate from
 * the boolean closed-world fluents — scalars never become atoms. Values are
 * read only through comparison *guard atoms* (`hp<=0`), which the world asserts
 * closed-world from the stored value on every evaluation (strict inputs, never
 * UNDECIDED, never concluded by a rule). `min`/`max` are the declared clamp
 * range (`state hp : int in 0..20`), the outermost stage of the commit
 * pipeline below. */
typedef enum {
    WORLD_CMP_LE, WORLD_CMP_LT, WORLD_CMP_GE, WORLD_CMP_GT, WORLD_CMP_EQ
} world_cmp;

void world_declare_num(world *w, uint32_t atom, long min, long max, bool has_range);
void world_set_num(world *w, uint32_t atom, long value);
long world_get_num(const world *w, uint32_t atom);

/* Register a guard atom: `guard` is proved exactly when the numeric fluent
 * `num` satisfies `<op> threshold` for its current value. */
void world_add_guard(world *w, uint32_t guard, uint32_t num,
                     world_cmp op, long threshold);

/* Numeric effects — the write side (§5.8). Attached to a step rule (below);
 * they fire when that rule fires and run in the *commit* phase, after the
 * boolean fixpoint has settled. Effects are a closed operator set:
 *   - WORLD_OP_ASSIGN (`:=`) is a value conclusion — two firing assigns with
 *     distinct values on one fluent is a contested step (world_step returns -1);
 *   - WORLD_OP_ADD/SUB (`+=`/`-=`) are relative and combine by summation,
 *     order-free (addition commutes). A defeated effect contributes nothing.
 * The pipeline per fluent is: base (winning `:=`, else inertia = current value)
 * -> sum of undefeated deltas -> clamp to the declared range. */
typedef enum { WORLD_OP_ASSIGN, WORLD_OP_ADD, WORLD_OP_SUB } world_numop;

/* Effect right-hand-side bytecode: a stack VM over `long`. Integer/fixed-point
 * only — §5.8 keeps floats on the renderer side of the I4 replay wall. Evaluated
 * at commit time against the *pre-step* value store (all effects read current
 * values; the store double-buffers and swaps once). */
typedef enum {
    EXPR_CONST,   /* push arg */
    EXPR_LOAD,    /* push current value of the numeric fluent whose atom = arg */
    EXPR_ADD, EXPR_SUB, EXPR_MUL, EXPR_NEG, EXPR_MIN, EXPR_MAX
} expr_op;
typedef struct { expr_op op; long arg; } expr_ins;

/* Judgment rules (query layer). Returns rule handle for world_add_sup. */
int  world_add_rule(world *w, const char *name, dl_rule_kind kind,
                    dl_lit head, const dl_lit *body, int nbody);
void world_add_sup(world *w, int winner, int loser);

/* Attach a provenance suffix (source span + generation reason, §6.3) to a rule
 * by its world_add_rule / world_add_step_rule handle; rendered by world_why /
 * the step trace after the rule kind. Copied; NULL clears it. */
void world_set_rule_prov(world *w, int rule, const char *prov);
void world_set_step_prov(world *w, int rule, const char *prov);

/* Step rules: fire when `action` occurs (INTERN_NONE = ramification, fires
 * in every step whose state matches). Body literals may reference the next
 * state via primed=true; effects are always about the next state. */
typedef struct {
    dl_lit lit;
    bool primed;
} step_cond;

/* Returns a handle for world_add_num_effect (its index among step rules). */
int  world_add_step_rule(world *w, const char *name, uint32_t action,
                         const step_cond *body, int nbody,
                         const dl_lit *effects, int neffects);

/* Attach a numeric effect to a step rule (its world_add_step_rule handle).
 * `code`/`ncode` is the RHS bytecode; the world copies it. */
void world_add_num_effect(world *w, int rule, uint32_t num_atom,
                          world_numop op, const expr_ins *code, int ncode);

/* Query the current state (facts + judgment rules). */
dl_verdict world_query(world *w, dl_lit q);
void       world_why(world *w, dl_lit q, FILE *out);

/* Lane families (the DoD thesis: entities as bit-parallel lanes, not named
 * atoms). A per-sort N-lane dl_col family for the homogeneous single-variable
 * judgment rules over that sort — the same rules run once across 64 entities per
 * word instead of grounded per entity into distinct atoms. The grounder emits
 * them; they are validated against the N=1 query path but not yet routed to (the
 * dl_col-was-prototyped-before-adopted playbook). `ground` is a flat
 * natoms*nent array of the equivalent named ground atom for each
 * (predicate-local-id, lane); `is_fluent` flags which locals take base facts.
 * `is_import` (may be NULL = none) flags locals that are DERIVED elsewhere and
 * imported: their per-cell verdict is queried and injected each solve, rather
 * than concluded here — the join matcher's derived-body case (§5.5 import).
 * The world copies all and takes ownership of `fam`. */
void world_add_lane_family(world *w, dlcol *fam, int natoms, int nent, int niter,
                           const uint32_t *ground, const bool *is_fluent,
                           const bool *is_import);
int  world_lane_family_count(const world *w);

/* Differential pin (mirrors test_col's dl-vs-dl_col): load current state into
 * every lane family, solve, and compare each (predicate, entity) verdict to
 * world_query on the equivalent ground atom. Returns the number of comparisons;
 * sets *ok false on any mismatch (both polarities checked). */
int  world_lanes_check(world *w, bool *ok);

/* Trace how a fluent reached its value in the *last* step (causal rules,
 * ramifications, generated inertia). `next` true reads q's atom in the next
 * state (its primed form — the usual "why did it end up this way?"), false the
 * current-state value the step saw. Valid only after a successful world_step,
 * until the next step or a rule/fluent edit. */
void world_step_why(world *w, dl_lit q, bool next, FILE *out);

/* Advance one step given occurring action atoms. Returns 0 on success and
 * commits the new state; returns -1 and leaves the state untouched if some
 * fluent's next value is contested/undecided (err gets the fluent name) —
 * for numerics that is two `:=` effects claiming distinct values. */
int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz);

/* The commit receipt (§5.8): after a successful world_step, how a numeric
 * fluent reached its value. `*base` is the pipeline base (winning `:=`, else
 * the pre-step value); `out` receives the undefeated contributions in
 * application order (the winning assign first, if any, then each delta). The
 * final value is base + sum(deltas) clamped to the declared range. Returns the
 * contribution count (may exceed `cap`; only the first `cap` are written).
 * Valid only immediately after a step that returned 0. */
typedef struct {
    const char *rule;     /* source step-rule name (borrowed from the world) */
    world_numop op;
    long        amount;   /* the assigned value, or the signed delta applied */
} world_contrib;

int world_num_receipt(const world *w, uint32_t atom, long *base,
                      world_contrib *out, int cap);

#endif
