#ifndef INF_STATE_WORLD_H
#define INF_STATE_WORLD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/intern.h"
#include "logic/dl.h"

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
