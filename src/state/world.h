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

void world_add_step_rule(world *w, const char *name, uint32_t action,
                         const step_cond *body, int nbody,
                         const dl_lit *effects, int neffects);

/* Query the current state (facts + judgment rules). */
dl_verdict world_query(world *w, dl_lit q);
void       world_why(world *w, dl_lit q, FILE *out);

/* Advance one step given occurring action atoms. Returns 0 on success and
 * commits the new state; returns -1 and leaves the state untouched if some
 * fluent's next value is contested/undecided (err gets the fluent name). */
int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz);

#endif
