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

/* #159 exclusivity groups (§5.13, EPIC #154): a checked action-exclusivity
 * PROTOCOL. A declared group admits at most one of its member action
 * instances per (group, key) per step — members registered with the same
 * key conflict; distinct keys are independent; the same atom submitted
 * twice is one action, not a violation. Checked at the top of world_step
 * before any solve, state untouched — the same host-protocol posture as the
 * #119 unknown-action check, and the row the §6.3 typed binding retires (a
 * bound host cannot construct a violating action set). `label` names the
 * group and `prov` its declaration span in the rejection message (both
 * copied; prov may be NULL). */
int  world_new_excl_group(world *w, const char *label, const char *prov);
void world_add_excl_member(world *w, int group, uint32_t action_atom,
                           uint32_t key);

/* #109 cycle rule (§5.2): scan the ground JUDGMENT support graph (body ->
 * head over strict/defeasible rules) for a cyclic SCC some rule — defeaters
 * included — attacks (concludes a member's complement into). Returns true
 * and fills `err` (the loop and the attacker, human-readable) and `*prov`
 * (the attacker's provenance string, or NULL) when one exists. The .story
 * compiler turns this into a located compile error; the engine itself leaves
 * attacked cycles honestly UNDECIDED (the Datalog completion never touches
 * them), so hand-built worlds keep today's behavior. */
bool world_attacked_cycle(world *w, char *err, size_t errsz, const char **prov);

/* Numeric fluents (DESIGN.md §5.8): an integer value store, kept separate from
 * the boolean closed-world fluents — scalars never become atoms. Values are
 * read only through comparison *guard atoms* (`hp<=0`). A guard over a STORED
 * numeric is asserted closed-world from the stored value on every evaluation
 * (a strict input, never UNDECIDED, never concluded by a rule). A guard over
 * a DERIVED value (an expression guard whose bytecode inlines a value chain)
 * is only closed-world while the value is total: over a PARTIAL value (#116 —
 * one with no unconditional base definition) it is genuinely tri-valued, and
 * when no definition applies the guard asserts NEITHER fact, leaving it
 * UNDECIDED — the honest "the question does not apply". `min`/`max` are the
 * declared clamp range (`state hp : int in 0..20`), the outermost stage of
 * the commit pipeline below. */
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
 * reads (EXPR_TEST) see the solve of their own stratum —
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

/* Trace-time provider rendering (#178). A host-answered relation contributes
 * nothing to a why-trace but its own verdict: `los(a,b) [PROVED]` is the whole
 * story, while WHAT the ray hit — the crate at (3,7), the 3.2 metres — sits in
 * the host's answer and is discarded. In an engine whose product is the trace,
 * that leaves a black box in the middle of it.
 *
 * The callback writes a short phrase for one ground provider atom; return the
 * number of bytes written (0 = nothing to say, and the trace renders exactly as
 * it does today, so this is additive and costs nothing to skip):
 *
 *   los(a,b) [blocked by crate at (3,7)] [PROVED]
 *
 * Consulted ONLY while rendering a trace, never during a solve — it exists to
 * explain a verdict already reached, so it cannot perturb one. That keeps it
 * outside I4 entirely: a render callback that is slow, allocating, or
 * nondeterministic in its PHRASING cannot affect replay. `holds` is the verdict
 * being explained, so one callback can phrase both the yes and the no. */
typedef int (*world_provider_render_fn)(void *ctx, uint32_t pred,
                                        const uint32_t *args, int nargs,
                                        bool holds, char *buf, size_t cap);
void world_set_provider_render_fn(world *w, world_provider_render_fn fn,
                                  void *ctx);

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

/* The value-provider call log (#178, the other half of provider opacity). A
 * value-returning provider hands the effect VM a number and no account of where
 * it came from: a receipt says `-3` and cannot say `neighbor(at(x), north)`
 * answered `3`. With the log on, every call a step makes is recorded in order —
 * a debugging facility, off by default because the provider boundary is the
 * hot path (§8.1) and a step should not pay for an explanation nobody asked
 * for. Recording is a side-channel, never a semantic one: it cannot change what
 * a call returns, so a replay with the log on and one with it off are the same
 * run (I4). Cleared at the top of every world_step; valid until the next one. */
typedef struct { uint32_t pred; int nargs; long args[4]; long result; } world_fn_call;
void world_set_fn_call_log(world *w, bool on);
const world_fn_call *world_fn_calls(const world *w, int *count);

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
    EXPR_PPOP,    /* drop the top prior slot (leave a layer) */
    EXPR_LOADN,   /* primed numeric READ (#87 extension, for #84's modeled
                   * pipeline): push the NEXT value of the numeric fluent whose
                   * atom = arg — the value a lower stratum already committed
                   * this tick. Legal only in ramification effect expressions
                   * whose rule the stratifier placed above every writer of the
                   * fluent; a fluent with no writers reads its current value
                   * (inertia: next == current). The .story front end rejects
                   * it everywhere else. */
    EXPR_REQDEF   /* partial-value definedness check (#116): pop x; if x <= 0,
                   * flag the evaluation UNDEFINED (out-of-band, never an
                   * in-band sentinel; evaluation continues, so every roll
                   * site is still visited — §5.10 replay stability). Emitted
                   * at the end of a partial value's inlined chain with
                   * x = OR(prior-free layer markers) + Σ(1 − test(enclosing
                   * layer marker)): a nested read only demands definedness
                   * when every enclosing layer actually fired, so the
                   * evaluate-all-and-mask shape never poisons a masked-off
                   * branch. An undefined GUARD is UNDECIDED (asserts neither
                   * fact — genuinely tri-valued, see the numeric-fluent note
                   * above); an undefined effect RHS or clamp bound is
                   * unreachable from a clean compile (#116's static safety
                   * rule) and reports an internal step error — defense in
                   * depth, outside the language contract. */
} expr_op;
typedef struct { expr_op op; long arg; } expr_ins;

/* Register an expression guard: `guard` is proved when `lhs <op> rhs` holds, where
 * both sides are effect-expression bytecode (roll/fluent reads/const). Bytecode is
 * copied. Consulted at solve time (§5.8/§5.10) — e.g. the d20 `roll(20)+atk >= ac`. */
void world_add_expr_guard(world *w, uint32_t guard,
                          const expr_ins *lhs, int nlhs,
                          const expr_ins *rhs, int nrhs, world_cmp op);

/* The author's spelling of an expression guard (#132), for the why-trace: a
 * guard compiles to a synthetic marker atom, and a trace that names the marker
 * ("eg14[A=grunk,T=vera]") tells the reader nothing about the comparison that
 * failed. With a source string set, world_why / world_step_why render the guard
 * as it was written, followed by the operands THIS solve evaluated —
 * `atk_die(grunk) + atk_mod(grunk) >= ac(vera) [19 >= 19]` — which is the
 * difference between a debugger and an invitation to go digging. Copied. */
void world_set_expr_guard_src(world *w, uint32_t guard, const char *src);

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
/* Hand the re-ground context's lifetime to the world: `free_fn(ctx)` runs at
 * world_free. Used when the COMPILER installs a matcher the caller never asked
 * for and cannot see (#59 over-cap routing) — a caller that builds a matcher
 * explicitly keeps ownership and must not call this. */
void world_own_reground_ctx(world *w, void (*free_fn)(void *));

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
/* Register a predicate some matchable rule READS (#45), so a base-fact edit
 * that cannot change the match set does not force a re-ground. The first call
 * switches the world from staling the matched layer on EVERY edit to staling it
 * only for registered predicates — so a caller that registers must register
 * every predicate its matchable rules read (body atoms of any kind, negated
 * ones and guards included). Failing to register costs a missed re-ground and
 * therefore stale matches: register conservatively. Registering nothing keeps
 * the always-stale behaviour. */
void world_matcher_watch(world *w, uint32_t pred);

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

/* `split` (#121): designate ONE finite-domain fluent family (its MV-erased
 * value atoms, all previously declared) for per-value step-schema
 * specialization. Zero semantic content — a compilation hint (same species
 * as `merge`): each step selects a cached schema by the PRE-step value,
 * omitting rules statically dead under it (an unprimed positive body cond on
 * another value atom) and the inertia of fluents no live rule writes or
 * reads primed; excluded fluents commit by copy. Detection is syntactic and
 * dumb: negated or primed reads of the split fluent never filter (the rule
 * stays in every schema — the .story compiler rejects primed reads of a
 * split fluent outright). No unique value true -> the full schema. Also
 * upgrades #119's loudness: an action whose every rule is dead under the
 * current value is a -1-with-err ("matches no live step rule while
 * '<value>'"), state untouched. Returns 0; -1 if a split is already set,
 * nvals is outside 2..31, or a value atom is not a declared fluent. */
int  world_set_split(world *w, const uint32_t *value_atoms, int nvals);

/* Attach a numeric effect to a step rule (its world_add_step_rule handle).
 * `code`/`ncode` is the RHS bytecode; the world copies it. */
void world_add_num_effect(world *w, int rule, uint32_t num_atom,
                          world_numop op, const expr_ins *code, int ncode);

/* Typed damage (#83/#84) has NO engine stage. The configured per-type
 * response pipeline (base → Σ per type → response → clamp, PHB p.197 algebra
 * in C) was removed by the #84 decision once the modeled form was pinned
 * equivalent: per-type transient accumulators, response rules as ordinary
 * defeasible layers, one committing effect (DESIGN.md §5.8). Typed damage is
 * content, not engine. */

/* Transient emissions — burst cues (#11, DESIGN.md §12). A one-shot event a
 * step FIRES, never a fact it stores: an emit atom is an effect head like any
 * other, but it takes no base fact, generates no inertia, and is read by
 * nothing. It holds for exactly the step that concluded it and vanishes.
 *
 * - I1/I2-clean: an emission is an OUTPUT of a step (the symmetric twin of an
 *   action, which is a transient input), never stored, never queried, never
 *   re-derived. Nothing in the theory may read one — the .story front end
 *   rejects a body that mentions an emit predicate.
 * - I4: emissions are a pure function of (state, actions) — the same solve the
 *   next state is read from — so a replay reproduces the exact stream.
 * - Positive only: an emit head is never negated, so emissions have no defeat
 *   structure of their own. Defeasibility lives upstream, in the firing rule's
 *   body (which reads defeated judgments like any step rule). Two rules firing
 *   the same cue emit it ONCE — an emission is a proposition about the
 *   transition, not a count; parameterize the atom to distinguish sources.
 *
 * `world_emits` returns the flat per-tick buffer: the emit atoms concluded by
 * the last successful world_step, in DECLARATION order (deterministic, I4).
 * Cleared at the top of every world_step, so a rejected step emits nothing.
 * The pointer is owned by the world and valid until the next step. */
void world_declare_emit(world *w, uint32_t atom);
bool world_has_emit(const world *w, uint32_t atom);
const uint32_t *world_emits(const world *w, int *count);

/* Query the current state (facts + judgment rules). */
dl_verdict world_query(world *w, dl_lit q);
void       world_why(world *w, dl_lit q, FILE *out);

/* ---- Applicable actions: what a client may offer, and why not -------------
 *
 * Every client that presents choices asks the same question — which of these
 * actions can be taken right now, and for the ones that cannot, why? Without
 * this the client re-derives it: a judgment per action mirroring the action's
 * own `requires`, which is a second copy of the rule that can drift from the
 * first. The engine already knows: an action's guards are step-rule body
 * conditions it evaluates every step anyway.
 *
 * `world_actions` enumerates the GROUND action atoms the world has step rules
 * for (`force_door(hero)` and `force_door(guard)` are two), in rule order.
 *
 * `world_action_applies` is true when some step rule triggered by that action
 * has all of its current-state conditions satisfied — i.e. submitting it alone
 * would move something. Conditions about the NEXT state (`primed`) are not
 * decidable without taking the step, so a rule carrying one is reported
 * through `world_action_status` rather than being silently judged.
 *
 * `world_action_blockers` fills the unsatisfied conditions of the rule that
 * came CLOSEST to firing — the fewest failures — because that is the one an
 * author meant when they ask why a command is greyed out. Each is an ordinary
 * literal, so `world_why` on it prints the argument that refused it. */
typedef enum {
    WORLD_ACTION_APPLIES,     /* some rule's conditions all hold */
    WORLD_ACTION_BLOCKED,     /* every rule has an unsatisfied condition */
    WORLD_ACTION_SPECULATIVE, /* only rules with primed conditions remain */
    WORLD_ACTION_UNKNOWN,     /* no step rule is triggered by this atom */
} world_action_status;

int  world_actions(const world *w, uint32_t *out, int cap);
world_action_status world_action_status_of(world *w, uint32_t action);
bool world_action_applies(world *w, uint32_t action);
int  world_action_blockers(world *w, uint32_t action, dl_lit *out, int cap);

/* Lane families (the DoD thesis: entities as bit-parallel lanes, not named
 * atoms). A per-sort N-lane dl_col family for the homogeneous single-variable
 * judgment rules over that sort — the same rules run once across 64 entities per
 * word instead of grounded per entity into distinct atoms. The grounder emits
 * them; world_query answers from the family when the queried atom is a lane
 * cell, with world_lanes_check the differential pin against the N=1 query
 * path. `ground` is a flat
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
 * world_step routes the transition here when one family covers the whole step
 * world — no primed guards, no value split, numerics only if covers_numeric;
 * world_step_lanes_check is the differential pin against the N=1 path. */
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
 * fired marker. Marks the family covers_numeric (routable).
 *
 * #165: an RHS may also read `test()` verdicts. A verdict is 0/1, so an RHS over
 * `k` of them takes 2^k compile-time-known values. Per effect, `eff_ntest[i]`
 * gives k, `eff_tloc`/`eff_tneg` (both [nnumeff][tstride]) the family-local
 * column and polarity of each read, and `eff_table[i]` points at the 1<<k folded
 * values, indexed by those verdicts as bits. k=0 means the RHS is a plain
 * constant and `eff_konst` is used — the table is then redundant but valid. */
#define WORLD_LANE_MAXTEST 6           /* max test() reads in a laned numeric RHS */
void world_step_lane_set_numeric(world *w, int numsc, const uint32_t *num_atom_cell,
                                 int nnumeff, const int *eff_schema,
                                 const int *eff_op, const long *eff_konst,
                                 const uint32_t *eff_marker,
                                 const int *eff_ntest, const uint32_t *eff_tloc,
                                 const uint8_t *eff_tneg,
                                 const long *const *eff_table, int tstride);
/* Attach receipt provenance to the last-added step lane family (#88): which
 * entity each lane stands for, and per numeric effect the authored rule name,
 * its predicate atom, and the variable the lane binds. A laned effect has no
 * ground instance name — one schema rule runs for every lane — so a commit
 * receipt reconstructs the binding per lane from these instead of formatting a
 * name per commit. Names are copied. */
void world_step_lane_set_prov(world *w, const uint32_t *lane_ents, int nent,
                              const char *const *eff_name,
                              const uint32_t *eff_pred, const uint32_t *eff_var,
                              int nnumeff);

/* Register a `for each` binder's broadcast cast triggers on the last-added step
 * lane family: each ground cast atom drives its WORLD_STEP_BCAST local (all lanes)
 * when it occurs — the discrete cast fans out over the target lanes. */
void world_step_lane_set_bcast(world *w, int ncast, const uint32_t *cast_atom,
                               const int *cast_local);
/* #121 mixed routing: bind the last-added step lane family to one split value
 * (the grounder emits one family per value, its split guards erased), and mark
 * a step rule as covered by those families under the given value bitmask (bit
 * v set = the value-v family carries this rule, so the residue schema omits
 * it). world_step then solves the current value's family FIRST and feeds its
 * fluents' next-state into the N=1 residue solve as strict primed facts — the
 * lane half is a stratum under the residue — committing those fluents from
 * the lane result. Unbound values (or a value with no family) step plain N=1. */
void world_step_lane_bind_value(world *w, int value_index);
void world_step_rule_set_lane_cover(world *w, int rule, uint32_t value_mask);
int  world_step_lane_family_count(const world *w);
/* True iff world_step routes the numeric transition through the lane family. */
bool world_routes_numeric(const world *w);

/* Differential pin for the transition layer: solve both the N=1 step family and
 * the step lane family from the current state + the given actions, and compare
 * each fluent's next-state (primed) verdict per lane. Commits nothing. Returns
 * the number of comparisons; sets *ok false on any mismatch (both polarities).
 *
 * SCOPE — this covers BOOLEAN fluents only. Numeric next-values are produced by
 * the commit pipeline (base + Σ deltas, clamped) which runs outside the verdict
 * columns entirely, so a wrong numeric delta passes this check unseen. A laned
 * numeric effect needs a committed-state comparison as well; test_lanefront runs
 * both pins over the same shapes for exactly that reason. Lane-only synthetic
 * readouts (a numeric effect's fired marker) are skipped — they exist in no N=1
 * family, so they have no counterpart to be compared against. */
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
 * for numerics that is two `:=` effects claiming distinct values.
 *
 * Loud no-op actions (#119): an action atom that triggers ZERO step rules is
 * also -1-with-err ("matches no step rule"), state untouched — it can never
 * do anything in this world, so submitting it is a host bug (typo'd atom,
 * protocol drift), never a legal no-op. An action whose matching rules all
 * fail their guards is NOT an error: it steps normally and the turn is spent.
 * A speculative host should pre-check with world_query, not rely on silence.
 *
 * Burst cues fired by this step are read afterwards with world_emits. A world
 * that declares emissions steps N=1: the lane routes carry no emit columns, so
 * routing one would silently drop the stream (the .story grounder builds no
 * step lane family for such a world; this is the engine-side backstop). */
int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz);

/* Subscriptions (§11 M2): the client's reactive channel, and deliberately ONE
 * primitive over facts AND judgments. Register interest in a literal; each step
 * reports the subscribed literals whose verdict flipped.
 *
 * Base facts are the free leaf case — the step already computes its changeset —
 * while a derived judgment is the cone-recompute case (§4.1 wake-ups), so the
 * two differ in COST, not in call shape. That is the point: a fluent refactored
 * into a judgment (or back) never touches a client call site, because a
 * subscription names a CONCLUSION, not a storage decision. A subscription to a
 * large-cone judgment is a cardinality-style warning, not a different primitive.
 *
 * Two readouts, the pair a client loop actually wants (§12): the LEVEL —
 * world_sub_verdict, the current answer — and the EDGES, which flipped this
 * step (the `btnp` to level's `btn`). An edge is a per-step difference: the
 * verdict at the end of this step against the end of the previous one, or
 * against the moment of subscription. Seeding state between steps with
 * world_set therefore folds into the next step's edges, which is the honest
 * reading — state is constant within a tick, and a load is not a tick.
 *
 * Handles are stable: unsubscribing never renumbers another subscription.
 * Edges are reported in subscription order and cleared at the top of every
 * step, so a rejected step reports none (I4: the same actions on the same state
 * produce the same edge stream).
 *
 * The demand-cone optimization §4.1 describes — maintaining only what is both
 * reachable and demanded — is M4, and invisible here: it changes what a
 * subscription COSTS, never what it says. */
int        world_subscribe(world *w, dl_lit q);
void       world_unsubscribe(world *w, int sub);
dl_verdict world_sub_verdict(world *w, int sub);

typedef struct {
    int        sub;              /* the world_subscribe handle */
    dl_lit     lit;              /* the literal it names */
    dl_verdict from, to;         /* rose = to PROVED; fell = from PROVED */
} world_sub_edge;

const world_sub_edge *world_sub_edges(const world *w, int *count);

/* ---- the step readout: what changed, and why (#88) -----------------------
 *
 * Everything a step tells its client, past the next state itself. Three
 * surfaces, all valid only until the next world_step and all owned by the
 * world: the CHANGESET (what moved), the RECEIPT (how a number got there), and
 * world_emits above (what fired). Together they are the structured form of a
 * combat log line — "12 fire damage to grik from vera's fireball, 5 absorbed"
 * — so a client never parses a rendered string to recover what happened.
 *
 * The changeset is the §11 M2 delta's leaf case: the base facts that actually
 * moved this step, enumerated rather than polled. A client that knows which
 * atoms to ask about can still ask (world_get / world_get_num); one that wants
 * "what happened?" reads these. */
typedef struct { uint32_t atom; bool value; } world_bool_delta;
typedef struct { uint32_t atom; long from, to; } world_num_delta;

const world_bool_delta *world_bool_deltas(const world *w, int *count);
const world_num_delta  *world_num_deltas(const world *w, int *count);

/* One contribution to a numeric fluent's commit.
 *
 * Provenance is kept STRUCTURED, not formatted: `pred` is the authored action /
 * rule name and `vars`/`ents` its binding (`C=vera, T=grik`), so a client
 * renders its own sentence instead of parsing "fireball[C=vera,T=grik]" back
 * apart. `rule` remains the engine's own name for the ground rule — the ground
 * instance name on the N=1 path, the authored name on the routed lane path,
 * where the per-lane binding lives in `ents` instead. `pred` is INTERN_NONE
 * when nothing registered a binding (a hand-built world); then `rule` is all
 * there is.
 *
 * `defeated` marks a contribution that did NOT apply: an effect whose action
 * was submitted this step but whose rule failed its guards, carrying what it
 * WOULD have contributed — the "Immune — 0" row. Ramifications are excluded (a
 * ramification that does not fire is not a thwarted attempt, it is every other
 * rule in the world), and so is the routed lane path, whose effect columns
 * carry no link back to the action that triggered them. */
typedef struct {
    const char     *rule;     /* step-rule name (borrowed from the world) */
    uint32_t        pred;     /* authored action/rule name atom, or INTERN_NONE */
    const uint32_t *vars;     /* [nbind] variable-name atoms (borrowed) */
    const uint32_t *ents;     /* [nbind] what each variable was bound to */
    int             nbind;
    world_numop     op;
    long            amount;   /* the assigned value, or the signed delta */
    bool            defeated;
} world_contrib;

/* How a numeric fluent reached its value (§5.8): base (the winning `:=`, else
 * the pre-step value) -> `raw` = base + Σ undefeated deltas -> `applied`, the
 * committed value after the declared range retracts it. Reporting both ends is
 * what makes overkill and absorption renderable — a 12-damage hit on a 5 HP
 * target has raw -7 and applied 0, and `clamped` says the range spoke.
 * `lo`/`hi` are the bounds this step actually used (a dynamic bound is
 * evaluated per commit, so they are not the declaration's constants).
 *
 * Returns false for an atom that is not a declared numeric fluent. Valid only
 * immediately after a step that returned 0; `items` points into world memory. */
typedef struct {
    long base, raw, applied;
    long lo, hi;
    bool has_range;           /* false: lo/hi are meaningless, applied == raw */
    bool clamped;             /* the range moved the value (applied != raw) */
    const world_contrib *items;
    int  n;
} world_receipt;

bool world_num_receipt(const world *w, uint32_t atom, world_receipt *out);

/* Register a step rule's SOURCE identity (#88): the authored action/rule name
 * and this ground instance's binding, by its world_add_step_rule handle. The
 * state tier keeps them as ids — it never learns the term grammar — and hands
 * them back on every contribution the rule makes. Copied; `n` may be 0 (an
 * arity-0 action still names its pred). */
void world_set_step_binding(world *w, int rule, uint32_t pred,
                            const uint32_t *vars, const uint32_t *ents, int n);

#endif
