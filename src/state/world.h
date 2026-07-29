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

/* Structured view of a boolean fluent, for the tick-time matcher's extension
 * index (§5.2 item 4, #28): records the ground atom's (pred, arg-entity) shape
 * so world_fact_index can rebuild predicate -> its true tuples from w->vals.
 * Only base boolean fluents carry this; the grounder attaches it right after
 * world_declare_fluent. A fluent with no structure is absent from the index. */
void world_set_fluent_struct(world *w, uint32_t atom, uint32_t pred,
                             const uint32_t *args, int nargs);

/* The fact-store extension index over the CURRENT boolean state (the inverse of
 * "is this atom true?": a predicate -> its true argument tuples). Refreshed
 * lazily from w->vals on any state edit, in fluent-declaration order — under
 * the sparse universe (#92) that is FIRST-TOUCH order, a pure function of the
 * source text plus the host's call sequence, so enumeration stays
 * deterministic and replay-exact (I4). The tick-time matcher scans this
 * instead of the sort cross product. Owned by the world; the returned
 * pointer is valid until the next state edit. */
struct factindex;
const struct factindex *world_fact_index(world *w);

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

/* §5.8 stratification (#87). A PRIMED guard reads the NEXT value of a numeric
 * fluent ("if hp' <= 0 then dead" — the dying trigger). The tick then runs in
 * STRATA: the compiler assigns each step rule a stratum (0 = today's rules)
 * such that everything that can write a primed-guarded fluent settles below
 * every rule reading that guard; world_step solves the step theory once per
 * stratum, commit-computes the numeric pipeline for the fluents each stratum
 * owns, mints their primed-guard atoms as strict facts, and re-solves for the
 * strata above. The state write stays atomic at the end and the action log
 * records ONE step (I4 — replay never sees a half-tick). Commit-time verdict
 * reads (the #84 response, EXPR_TEST) see the solve of their own stratum —
 * the latest settled state, never a partially-propagated one; with no primed
 * guards the world is the degenerate one-stratum case, byte-identical to
 * today's tick. Primed-numeric CYCLES (a writer of `hp` gated on `hp'`)
 * genuinely oscillate and are rejected at compile time, in the .story front
 * end. NOT on the routed lane path — stratified worlds step N=1. */
void world_add_primed_guard(world *w, uint32_t guard, uint32_t num,
                            world_cmp op, long threshold);
void world_set_step_stratum(world *w, int rule, int stratum);

/* Providers (§5.2/§5.6): a computed relation answered host-side (adjacency,
 * range, line-of-sight), consulted at solve time and never stored. The callback
 * answers whether ground `pred(args)` holds; args are entity atoms. Set once by
 * the host; the grounder registers each ground provider atom it emits. A provider
 * atom with no callback set reads false (closed-world). */
typedef bool (*world_provider_fn)(void *ctx, uint32_t pred,
                                  const uint32_t *args, int nargs);
void world_set_provider_fn(world *w, world_provider_fn fn, void *ctx);
void world_declare_provider_atom(world *w, uint32_t atom, uint32_t pred,
                                 const uint32_t *args, int nargs);
/* Ask the provider callback directly (no callback -> closed-world false) —
 * the match-time filter evaluation for island rules (#80), identical to what
 * the solver's provider fact-load would conclude for the same state. */
bool world_provider_holds_at(const world *w, uint32_t pred,
                             const uint32_t *args, int nargs);
/* Does `num_atom <op> threshold` hold for the numeric fluent's current value
 * (undeclared reads 0, like world_get_num)? The match-time analog of a
 * registered guard atom's closed-world fact (#80). */
bool world_num_cmp_holds(const world *w, uint32_t num_atom,
                         world_cmp op, long threshold);

/* Value-returning function providers (§5.6): a host function that returns a
 * value (a cell handle / int), not a relation — e.g. `neighbor(at(X), dir)` for
 * directional movement. Consulted from the effect-VM (EXPR_CALL) at commit time;
 * `pred` names the function, `args` are the already-evaluated argument values (a
 * cell handle read via EXPR_LOAD, an int, …). Must be deterministic and seedless
 * (I4) — geometry, not judgment; randomness goes through roll() (§5.10). With no
 * callback set a call reads 0, the closed-world analog of a false provider. */
typedef long (*world_fn_provider_fn)(void *ctx, uint32_t pred,
                                     const long *args, int nargs);
void world_set_fn_provider_fn(world *w, world_fn_provider_fn fn, void *ctx);

/* Seeded randomness (§5.10): a roll is a keyed lookup, not a stream. `seed` is
 * save state; `tick` is a monotone step counter the engine bumps each successful
 * world_step. A roll site (die `sides`, a precomputed `site` key folding the
 * calling rule-instance + an author tag) reads 1..sides from hash(seed, tick,
 * site) — idempotent within a tick, fresh the next. Returns the site's index for
 * the effect/guard bytecode (EXPR_ROLL). */
void world_set_seed(world *w, uint64_t seed);
uint64_t world_tick(const world *w);
int  world_add_roll_site(world *w, int sides, uint64_t site);

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

/* Declared merge algebra for the ASSIGN class of one numeric fluent (#85).
 * The fluent is already a CRDT — `+=`/`-=` a PN-counter, `:=` a
 * conflict-detecting register (deliberately not last-write-wins: LWW picks an
 * order among rules, which §5.8 forbids). MIN/MAX extend the closed algebra
 * set: multiple firing `:=` contributions merge by the extreme instead of
 * contesting. Commutative and idempotent by construction, so order-free and
 * non-stacking. Governs the ASSIGN class only — deltas still sum on top of
 * the merged base, and the declared range clamp stays outermost. Motivating
 * cases: armor floors, damage caps, "highest bid this tick". Host-supplied
 * merge functions are deliberately excluded (I4: the engine cannot verify a
 * host merge commutes, and a non-commutative merge diverges replay silently;
 * a closed engine-known set stays lanable and `why?`-traceable). */
typedef enum { WORLD_MERGE_REGISTER, WORLD_MERGE_MIN, WORLD_MERGE_MAX } world_merge;
void world_set_num_merge(world *w, uint32_t atom, world_merge m);

/* Effect right-hand-side bytecode: a stack VM over `long`. Integer/fixed-point
 * only — §5.8 keeps floats on the renderer side of the I4 replay wall. Evaluated
 * at commit time against the *pre-step* value store (all effects read current
 * values; the store double-buffers and swaps once). */
typedef enum {
    EXPR_CONST,   /* push arg */
    EXPR_LOAD,    /* push current value of the numeric fluent whose atom = arg */
    EXPR_ROLL,    /* push a seeded die face 1..sides for roll-site index = arg (§5.10) */
    EXPR_CALL,    /* call a value-returning fn provider (§5.6): arg packs the
                   * function pred (<<8) and its argument count (low 8 bits); the
                   * top `nargs` stack cells are the (already-evaluated) call args,
                   * replaced by the returned value */
    EXPR_ADD, EXPR_SUB, EXPR_MUL, EXPR_NEG, EXPR_MIN, EXPR_MAX,
    EXPR_DIV,     /* floored: quotient rounds toward -inf (5e "round down", so
                   * -7/2 = -4 — NOT C truncation), pinned by golden test (I4).
                   * Division by zero yields 0 (defined; a constant-0 divisor is
                   * rejected at compile time by the .story front end). */
    EXPR_TEST     /* push a literal's solved verdict as 0/1 (#86): arg packs
                   * (atom << 1 | neg); 1 iff the literal is defeasibly PROVED
                   * in the settled step theory, else 0 — the verdict of the
                   * literal exactly as a body atom reads it, NOT negation-as-
                   * failure: an UNDECIDED derived atom tests 0 both ways, and
                   * a REFUTED derived atom still tests 0 under neg unless
                   * something concludes the negation (base fluents are closed-
                   * world, so their neg flips 0/1 as expected). Branch-free
                   * modifiers: `base + test(flag) * delta`. Base fluents and
                   * provider atoms answer from the store/callback (closed-
                   * world, load-time inputs); derived judgments read the
                   * solved family. In EFFECTS this runs commit-side, strictly
                   * downstream; in expression GUARDS each solve runs two-phase
                   * (solve without test-guards, evaluate them against the
                   * settled result, reload, solve) — sound because the .story
                   * front end rejects a tested judgment whose cone contains a
                   * test-bearing guard (nesting), and still rejects test() in
                   * clamp bounds and value definitions. */
    ,
    /* Layered derived-value chains (#82/#94): a value's definitions compile to
     * one branch-free program — base expr, then per layer (in superiority
     * order) the evaluate-and-mask blend v' = v + test(marker)·(f(v) − v),
     * where `prior` inside f reads the running value below. The prior STACK
     * (not a register) lets a nested value's chain evaluate inside f without
     * clobbering the outer layer's prior. */
    EXPR_PPUSH,   /* pop the running value onto the prior stack (enter a layer) */
    EXPR_P,       /* push the top of the prior stack (`prior` reads) */
    EXPR_PPOP     /* drop the top prior slot (leave a layer) */
} expr_op;
typedef struct { expr_op op; long arg; } expr_ins;

/* Register an expression guard: `guard` is proved when `lhs <op> rhs` holds, where
 * both sides are effect-expression bytecode (roll/fluent reads/const). Bytecode is
 * copied. Consulted at solve time (§5.8/§5.10) — e.g. the d20 `roll(20)+atk >= ac`. */
void world_add_expr_guard(world *w, uint32_t guard,
                          const expr_ins *lhs, int nlhs,
                          const expr_ins *rhs, int nrhs, world_cmp op);

/* Attach dynamic clamp bounds to a numeric fluent (§5.8): `int in 0..hp_max(X)`
 * compiles each bound to effect-VM bytecode, evaluated per-commit against the
 * value store. NULL/0 on a side leaves it on the constant min/max from
 * world_declare_num. Bytecode is copied. Requires the fluent to have a range. */
void world_set_num_clamp(world *w, uint32_t atom,
                         const expr_ins *lo, int nlo,
                         const expr_ins *hi, int nhi);

/* Judgment rules (query layer). Returns rule handle for world_add_sup. */
int  world_add_rule(world *w, const char *name, dl_rule_kind kind,
                    dl_lit head, const dl_lit *body, int nbody);
void world_add_sup(world *w, int winner, int loser);

/* Tick-time matcher (#28): checkpoint the current judgment-rule count as the
 * boundary between statically-ground rules and re-materialized *matched* rules,
 * then drop everything above the checkpoint before each re-ground; world_query
 * rebuilds the judgment family from what remains. Matched-kernel rules carry no
 * superiority, so only the rule array is watermarked — the static `>` relation is
 * left intact. */
void world_matched_checkpoint(world *w);
void world_matched_reset(world *w);

/* Auto re-ground hook (#45): register a callback the world fires — lazily, once
 * per state change, right before it (re)builds its solve families — to refresh
 * the matched judgment layer against the current facts. This is dependency
 * inversion (like world_set_provider_fn): the state tier declares the interface;
 * the .story compiler supplies the matcher as `ctx`. With it set, a host just
 * calls world_step / world_query as usual and the matched layer stays fresh — no
 * host-driven re-grounding. `ctx` must outlive the world (free the world first).
 * Registering marks the layer stale so the first solve re-grounds. */
typedef void (*world_reground_fn)(void *ctx, world *w);
void world_set_reground_fn(world *w, world_reground_fn fn, void *ctx);

/* Fluent schema hook (#92, the sparse fluent universe): under tick-time
 * compilation the grounder no longer declares the sort cross-product of every
 * boolean state predicate — ground fluents come into existence when TOUCHED.
 * The hook answers, for a bare atom id, "is this a well-formed ground instance
 * of a declared boolean state predicate?" and returns its (pred, args)
 * decomposition so the fact index can be maintained. Dependency inversion as
 * above: the state tier declares the interface, the .story compiler supplies
 * the retained schema as `ctx` (the world never learns the term grammar).
 * MUST be pure and deterministic over the world's lifetime (I4) — a schema
 * answer is part of replay. With it set:
 *   - world_set on an unknown-but-recognized atom lazily declares it (with
 *     structure) and proceeds — on ANY touch, true or false, since a false
 *     assertion still means "closed-world false, and judgments are stale";
 *   - a query on a never-touched recognized atom answers closed-world false
 *     WITHOUT declaring (the pure lazy path);
 *   - world_why on such an atom declares it first, so the rendered trace is
 *     byte-identical to a dense world's (base-fact line included). */
typedef bool (*world_schema_fn)(void *ctx, uint32_t atom, uint32_t *pred,
                                uint32_t *args, int *nargs);
void world_set_schema_fn(world *w, world_schema_fn fn, void *ctx);
/* Introspection (bench/tests): declared boolean fluent count — under #92 this
 * is O(touched), where the dense universe was the full cross-product. */
int  world_fluent_count(const world *w);
bool world_has_fluent(const world *w, uint32_t atom);

/* Matched views (#80): an ISLAND judgment — one matchable rule whose head
 * predicate is concluded by no other rule, attacked by nothing, and read
 * nowhere (no rule body, unless-guard, step-rule body, or superiority mentions
 * it) — has no defeat structure: every matched instance is an all-true-body
 * rule with no competitors, so its head's verdict is a pure function of match
 * membership. Its matched layer is therefore a SET, not rules: the re-ground
 * fills rows of (head atom, variable bindings); world_query answers from
 * membership without touching the judgment family at all.
 *   present        -> PROVED for the head polarity, REFUTED for the complement
 *   seen-but-absent-> REFUTED both polarities (a dropped match, exactly like a
 *                     located rule-less atom in the family)
 *   never seen     -> not in the theory (UNDECIDED via the normal fallthrough)
 * A view creator MUST register a materialize hook: world_why on a present view
 * atom re-emits just that atom's instances as ordinary matched rules (the hook
 * is called once per row, in insertion order) so the one shared trace renderer
 * runs — traces stay byte-identical to full emission. Rows are cleared by
 * world_views_reset (allocations kept, #48-style bounded memory); the ever-seen
 * registry is append-only. `bind` in world_view_add may have at most
 * WORLD_VIEW_MAXBIND entries. */
#define WORLD_VIEW_MAXBIND 6
typedef void (*world_materialize_fn)(void *ctx, world *w, uint32_t atom,
                                     int view, const uint32_t *bind, int nvars);
int  world_view_new(world *w, uint32_t head_pred, bool head_neg,
                    dl_rule_kind kind);
void world_views_reset(world *w);
void world_view_add(world *w, int view, uint32_t atom,
                    const uint32_t *bind, int nvars);
void world_set_materialize_fn(world *w, world_materialize_fn fn, void *ctx);
/* Introspection (bench/tests): rows currently present; bytes held by the view
 * store (rows + maps) — the space the matched layer occupies instead of rules. */
int    world_view_row_count(const world *w);
size_t world_view_bytes(const world *w);

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

/* Typed contributions + the per-type response (#83/#84, DESIGN.md §5.8).
 *
 * The commit pipeline gains one stage:
 *     base → Σ deltas PER TYPE → per-type response → clamp
 * That ordering is 5e's: the response applies after all other modifiers and
 * before the floor. A typed delta (`hp(T) -= roll(6)+3 as fire`) accumulates
 * in its type's bucket; per fluent and type the pipeline consults three
 * boolean judgments — the response atoms registered below, solved in the same
 * step theory over the PRE-step state — and scales the summed bucket:
 * immune → 0; resistant XOR vulnerable → magnitude halved (floored, 5e round
 * down) / doubled; both → they cancel (×1, PHB p.197). Multiple rules
 * concluding one response atom is just `proved` — non-stacking by construction.
 * The scaling lands in the receipt as an extra contribution named after the
 * response atom, so base + Σ items still equals the committed value.
 *
 * Cost discipline (#84): buckets are commit-time scratch, never stored state —
 * they exist only because `ndtypes` was declared (a CLOSED enum, which is what
 * keeps the accumulator a fixed-width cell). `dtype` indexes that enum;
 * `dtype < 0` is an untyped delta (today's behaviour, no response). An
 * unregistered response leaves a typed delta unscaled. NOT yet on the routed
 * lane path — the compiler keeps typed-effect worlds on the N=1 step. */
void world_set_dtypes(world *w, int ndtypes);
void world_add_num_effect_typed(world *w, int rule, uint32_t num_atom,
                                world_numop op, const expr_ins *code, int ncode,
                                int dtype);
void world_set_num_response(world *w, uint32_t num_atom, int dtype,
                            uint32_t resist, uint32_t vuln, uint32_t immune);

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

/* Grounded-theory size introspection (bench/debug — §5.2 grounding cost). A
 * judgment/step rule count *below* what the source rules should ground to
 * signals the grounder hit its MAX_INSTANCES cliff and dropped instances. */
int  world_judgment_rule_count(const world *w);
int  world_step_rule_count(const world *w);

/* Total bytes handed out by the world's arenas (static + matched-rule region).
 * A re-ground releases the matched region, so this stays BOUNDED across
 * story_matcher_reground calls rather than growing per tick (#48). Leak test. */
size_t world_arena_bytes(const world *w);

/* The dense schema location assigned to `atom`, or ~0u if it has none yet. Valid
 * after a query/step has built a family. Append-only (#67): once assigned, an
 * atom's location never changes across re-grounds — pinned by test_locstable. */
uint32_t world_atom_loc(const world *w, uint32_t atom);

/* Differential pin (mirrors test_col's dl-vs-dl_col): load current state into
 * every lane family, solve, and compare each (predicate, entity) verdict to
 * world_query on the equivalent ground atom. Returns the number of comparisons;
 * sets *ok false on any mismatch (both polarities checked). */
int  world_lanes_check(world *w, bool *ok);

/* Step lane family (the DoD thesis for the *transition* layer). One dl_col over
 * one lane sort holding the whole step theory bit-parallel: per fluent a current
 * and a primed local, per action an action local, plus generated inertia and
 * causal/ramification rules. `kind[loc]` classifies each local; `ground[loc*nent
 * + e]` is the named ground atom for that (local, lane) — the primed local's
 * ground atom is the fluent's `f'` twin. The world copies both and owns `fam`.
 * Prototype-before-adopt: built and validated (world_step_lanes_check) but not
 * yet routed through world_step, mirroring how the judgment lanes landed. */
/* WORLD_STEP_BCAST: a broadcast cast trigger (a `for each` binder's action) —
 * one signal ANDed into every target-lane, true when any of the action's ground
 * cast atoms occurred this step (see world_step_lane_set_bcast). */
enum { WORLD_STEP_CUR, WORLD_STEP_PRIMED, WORLD_STEP_ACTION, WORLD_STEP_BCAST };
void world_add_step_lane_family(world *w, dlcol *fam, int nloc, int nent,
                                const uint32_t *ground, const uint8_t *kind);
/* Attach the numeric lane extension (§5.8) to the last-added step lane family:
 * `num_atom_cell` [numsc*nent] gives each (schema, lane)'s ground numeric atom
 * (resolved to a w->nums index internally); the effect arrays give each fired
 * numeric effect its schema, op, constant RHS, and the family-local id of its
 * fired marker. Marks the family covers_numeric (routable). */
void world_step_lane_set_numeric(world *w, int numsc, const uint32_t *num_atom_cell,
                                 int nnumeff, const int *eff_schema,
                                 const int *eff_op, const long *eff_konst,
                                 const uint32_t *eff_marker);
/* Register a `for each` binder's broadcast cast triggers on the last-added step
 * lane family: each ground cast atom drives its WORLD_STEP_BCAST local (all lanes)
 * when it occurs — the discrete cast fans out over the target lanes. */
void world_step_lane_set_bcast(world *w, int ncast, const uint32_t *cast_atom,
                               const int *cast_local);
int  world_step_lane_family_count(const world *w);
/* True iff world_step routes the numeric transition through the lane family. */
bool world_routes_numeric(const world *w);

/* Differential pin for the transition layer: solve both the N=1 step family and
 * the step lane family from the current state + the given actions, and compare
 * each fluent's next-state (primed) verdict per lane. Commits nothing. Returns
 * the number of comparisons; sets *ok false on any mismatch (both polarities). */
int  world_step_lanes_check(world *w, const uint32_t *actions, int nactions,
                            bool *ok);

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
