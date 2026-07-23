/* Golden test for lane grounding (the DoD thesis, increment 2a). When the whole
 * judgment program is homogeneous over one sort, the grounder emits it as an
 * N-lane dl_col family — entities as bit-parallel lanes, the rule schema shared
 * rather than grounded per entity. This pins that the lane family's per-entity
 * verdicts are IDENTICAL to the proven N=1 query path (world_lanes_check), the
 * same differential-oracle discipline test_col applies to dl vs dl_col. It also
 * checks a non-homogeneous program produces NO lane family (the conservative
 * bail), so the fallback stays honest. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* A per-actor conditions program: single sort, all unary, superiority between
 * two lane rules (blessed beats poisoned), one global input in a body. Every
 * atom is over `actor` — the homogeneous case. thug is both poisoned and
 * blessed, so the superiority edge must resolve, in lanes. */
static const char *HOMO =
    "sort actor\n"
    "entity (guard, thug, mage, priest : actor)\n"
    "state (poisoned(actor) blessed(actor) alert(actor) danger)\n"
    "rule weakens(X: actor):  poisoned(X)          => weak(X)\n"
    "rule holds_up(X: actor): blessed(X)           => ~weak(X)\n"
    "holds_up > weakens\n"
    "rule staggers(X: actor): weak(X)              => slow(X)\n"  /* reads a lane conclusion */
    "rule engages(X: actor):  alert(X) & danger    => acts(X)\n"
    "init (poisoned(guard) poisoned(thug) blessed(thug) blessed(priest)"
    "      alert(mage) alert(guard) danger)\n";

static int test_homogeneous_agrees(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(HOMO, "conds.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* the whole program is homogeneous over `actor`: exactly one lane family */
    CHECK(world_lane_family_count(w) == 1);

    /* every (predicate, entity) verdict in the lane family matches world_query
     * on the equivalent named ground atom — the N=1 path is the oracle */
    bool ok = false;
    int checks = world_lanes_check(w, &ok);
    CHECK(checks > 0);
    CHECK(ok);

    /* spot-check the semantics the lanes had to reproduce: thug is poisoned AND
     * blessed, and holds_up > weakens, so ~weak(thug) wins */
    CHECK(world_query(w, dl_neg(intern_id(sy, "weak(thug)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "acts(mage)")))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "acts(guard)"))) == DL_PROVED);
    /* a two-level chain within the family: slow derives from weak (itself a lane
     * conclusion), so guard (weak) is slow but thug (~weak wins) is not */
    CHECK(world_query(w, dl_pos(intern_id(sy, "slow(guard)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "slow(thug)")))  != DL_PROVED);

    /* a state edit must invalidate the lane solve-cache: un-poison guard, and
     * weak(guard) — routed through the lane family — must stop being proved,
     * while the differential against the N=1 path still holds at the new state */
    world_set(w, intern_id(sy, "poisoned(guard)"), false);
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)"))) != DL_PROVED);
    CHECK(world_query(w, dl_neg(intern_id(sy, "weak(thug)"))) == DL_PROVED);
    bool ok2 = false;
    CHECK(world_lanes_check(w, &ok2) > 0);
    CHECK(ok2);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Partial coverage: a mixed program lanes the clean part and leaves the rest on
 * the N=1 path. `weak` is lane-clean (a plain defeasible rule over a base
 * fluent); `dead` depends on a numeric guard, so it taints and stays on jfam.
 * The lane family forms for `weak`, and both answers stay correct. */
static const char *MIXED =
    "sort actor\n"
    "entity guard : actor\n"
    "state (poisoned(actor) hp(actor) : int in 0..20)\n"
    "rule weakens(X: actor): poisoned(X) => weak(X)\n"
    "rule dying(X: actor):   hp(X) <= 0  -> dead(X)\n"
    "init poisoned(guard)\n";

static int test_mixed_partial(void)
{
    intern *sy = intern_new();
    world *w = story_compile(MIXED, "mixed.story", sy, NULL);
    CHECK(w != NULL);

    /* the clean slice lanes; the numeric slice does not — a family still forms */
    CHECK(world_lane_family_count(w) == 1);
    bool ok = false;
    CHECK(world_lanes_check(w, &ok) > 0);
    CHECK(ok);

    /* weak routes through the lane family; dead (numeric-guarded) through jfam.
     * hp defaults to 0, so hp <= 0 holds and dead(guard) is proved. */
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "dead(guard)"))) == DL_PROVED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* An `unless` guard — the engine's exception mechanism — lowered into lanes as a
 * defeater `immune ~> ~weak`, run bit-parallel. poisoned actors weaken unless
 * immune; the defeater must block per lane, not globally. */
static const char *UNLESS =
    "sort actor\n"
    "entity (guard, thug, mage : actor)\n"
    "state (poisoned(actor) immune(actor))\n"
    "rule weakens(X: actor): poisoned(X) => weak(X) unless immune(X)\n"
    "init (poisoned(guard) poisoned(thug) immune(thug))\n";

static int test_unless_lanes(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(UNLESS, "unless.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* the guarded rule lanes (unary guard) — one family, agreeing with jfam */
    CHECK(world_lane_family_count(w) == 1);
    bool ok = false;
    CHECK(world_lanes_check(w, &ok) > 0);
    CHECK(ok);

    /* guard: poisoned, not immune -> weak; thug: poisoned but immune, the
     * defeater blocks weak (undecided), so not proved; mage: not poisoned */
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(thug)")))  != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(mage)")))  != DL_PROVED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* The join matcher: a two-variable rule over actor x item. Lane over the first
 * variable (actor), iterate the second (item); the binary `holding` slices per
 * item. Each item-slice is solved bit-parallel over actors. */
static const char *JOIN =
    "sort actor, item\n"
    "entity (guard, thug : actor)\n"
    "entity (torch, vase : item)\n"
    "state (holding(actor, item) fragile(item) careless(actor))\n"
    "rule breaks(X: actor, T: item):"
    "     holding(X, T) & fragile(T) & careless(X) => broken(X, T)\n"
    "init (holding(guard, vase) holding(thug, torch)"
    "      fragile(vase) careless(guard) careless(thug))\n";

static int test_join_matcher(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(JOIN, "join.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* the two-variable rule forms a join family (lane=actor, iterate item) */
    CHECK(world_lane_family_count(w) == 1);
    /* every (predicate, actor) verdict, at every item slice, matches N=1 */
    bool ok = false;
    CHECK(world_lanes_check(w, &ok) > 0);
    CHECK(ok);

    /* the join semantics, now ROUTED through the lane family: the relational
     * head broken(X,T) mentions both variables, so each ground atom names one
     * (lane, iteration) cell — world_query answers from the bit-parallel slice,
     * re-solving per iteration on demand. guard holds the fragile vase and is
     * careless -> broken; thug holds a torch that is not fragile -> not; the
     * guard+torch pair was never held -> not. The three queries hit two distinct
     * item slices (vase, torch), exercising the per-iteration solve cache. */
    CHECK(world_query(w, dl_pos(intern_id(sy, "broken(guard,vase)")))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "broken(thug,torch)")))  != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "broken(guard,torch)"))) != DL_PROVED);

    /* a base-fact edit must invalidate the cached iteration solve: drop the
     * vase's fragility and guard no longer breaks it, while the differential
     * against the N=1 path still holds at the new state */
    world_set(w, intern_id(sy, "fragile(vase)"), false);
    CHECK(world_query(w, dl_pos(intern_id(sy, "broken(guard,vase)"))) != DL_PROVED);
    bool ok2 = false;
    CHECK(world_lanes_check(w, &ok2) > 0);
    CHECK(ok2);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* The join matcher generalized to THREE variables: actor x item x place. Lane
 * over the first variable (actor), iterate the other two as one flattened
 * cartesian product (item x place) — the family's niter. The relational head
 * drops(X,T,L) names all three variables, so each ground atom is one
 * (lane, iteration) cell and routes; the binary bodies each drop a variable, so
 * they repeat across cells and stay on the N=1 path. */
static const char *JOIN3 =
    "sort actor, item, place\n"
    "entity (guard, thug : actor)\n"
    "entity (torch, vase : item)\n"
    "entity (hall, cellar : place)\n"
    "state (holding(actor, item) at(actor, place) dark(place))\n"
    "rule fumbles(X: actor, T: item, L: place):"
    "     holding(X, T) & at(X, L) & dark(L) => drops(X, T, L)\n"
    "init (holding(guard, torch) at(guard, cellar) dark(cellar))\n";

static int test_join3_matcher(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(JOIN3, "join3.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* one join family (lane=actor, iterate item x place = 4 slices) */
    CHECK(world_lane_family_count(w) == 1);
    bool ok = false;
    CHECK(world_lanes_check(w, &ok) > 0);
    CHECK(ok);

    /* routed through the lane family across the flattened iteration: guard holds
     * the torch, is at the cellar, and the cellar is dark -> drops it there;
     * every other actor/item/place assignment fails one conjunct */
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,torch,cellar)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,torch,hall)")))   != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,vase,cellar)")))  != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(thug,torch,cellar)")))  != DL_PROVED);

    /* a fact edit invalidates the cached slice: light the cellar, guard keeps it */
    world_set(w, intern_id(sy, "dark(cellar)"), false);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,torch,cellar)"))) != DL_PROVED);
    bool ok2 = false;
    CHECK(world_lanes_check(w, &ok2) > 0);
    CHECK(ok2);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* A derived-body join: the two-variable rule reads `weak(X)`, which is itself a
 * conclusion of a single-variable rule — not a base fluent. `weak` lanes as its
 * own single-var family; the join family imports its per-cell verdict (§5.5)
 * rather than concluding it, querying it at solve time. Two families form. */
static const char *DERIVED_JOIN =
    "sort actor, item\n"
    "entity (guard, thug : actor)\n"
    "entity (torch, vase : item)\n"
    "state (poisoned(actor) holding(actor, item) fragile(item))\n"
    "rule weakens(X: actor): poisoned(X) => weak(X)\n"
    "rule fumbles(X: actor, T: item):"
    "     weak(X) & holding(X, T) & fragile(T) => drops(X, T)\n"
    "init (poisoned(guard) holding(guard, vase) holding(thug, torch)"
    "      fragile(vase) fragile(torch))\n";

static int test_derived_body_join(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(DERIVED_JOIN, "derived.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* two families: the single-var `weak` lane, and the join importing it */
    CHECK(world_lane_family_count(w) == 2);
    bool ok = false;
    CHECK(world_lanes_check(w, &ok) > 0);
    CHECK(ok);

    /* guard is poisoned -> weak, holds the fragile vase -> drops it (the join
     * read `weak(guard)` by importing the single-var family's verdict); thug is
     * not poisoned so never weak -> never drops; guard never held the torch */
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)")))        == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,vase)")))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(thug,torch)")))  != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,torch)"))) != DL_PROVED);

    /* editing the base fact that feeds the IMPORTED conclusion must ripple: cure
     * the poison, weak(guard) collapses, and the join answer follows it */
    world_set(w, intern_id(sy, "poisoned(guard)"), false);
    CHECK(world_query(w, dl_pos(intern_id(sy, "weak(guard)")))       != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "drops(guard,vase)"))) != DL_PROVED);
    bool ok2 = false;
    CHECK(world_lanes_check(w, &ok2) > 0);
    CHECK(ok2);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* The transition layer on lanes: a homogeneous single-sort boolean step world.
 * `arm` is an action (causal, beats inertia on the acted lane only); `wary` is a
 * ramification reading the NEXT state (`armed(X)'`) — inertia, causal, and a
 * primed-body ramification, all solved bit-parallel across actors. Validated
 * against the N=1 step family by world_step_lanes_check (not yet routed). */
static const char *STEP =
    "sort actor\n"
    "entity (guard, thug, mage : actor)\n"
    "state (armed(actor) alert(actor))\n"
    "action arm(X: actor): causes armed(X)\n"
    "action disarm(X: actor): causes ~armed(X)\n"
    "rule wary(X: actor): armed(X)' causes alert(X)\n"
    "init (armed(thug))\n";

static int test_step_lanes(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(STEP, "step.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);

    /* the step theory lanes into one step family */
    CHECK(world_step_lane_family_count(w) == 1);

    /* the bit-parallel transition must agree with the N=1 step family for every
     * fluent's next-state verdict, across a range of action sets */
    uint32_t arm_guard = intern_id(sy, "arm(guard)");
    uint32_t arm_mage  = intern_id(sy, "arm(mage)");
    uint32_t both[2] = { arm_guard, arm_mage };

    bool ok = false;
    CHECK(world_step_lanes_check(w, NULL, 0, &ok) > 0);   /* pure inertia (no-op) */
    CHECK(ok);
    ok = false;
    CHECK(world_step_lanes_check(w, &arm_guard, 1, &ok) > 0); /* causal on one lane */
    CHECK(ok);
    ok = false;
    CHECK(world_step_lanes_check(w, both, 2, &ok) > 0);   /* causal on two lanes */
    CHECK(ok);

    /* vary the current state (the check commits nothing) and re-validate: the
     * lane transition must still track N=1 at the new state */
    world_set(w, intern_id(sy, "armed(mage)"), true);
    ok = false;
    CHECK(world_step_lanes_check(w, &arm_guard, 1, &ok) > 0);
    CHECK(ok);

    /* world_step is now ROUTED through the step lanes: a real step commits the
     * expected next state (arm(guard): guard armed and, via the ramification,
     * alert; mage stays armed from the edit above; thug held by inertia) */
    char err[128];
    CHECK(world_step(w, &arm_guard, 1, err, sizeof err) == 0);
    CHECK(world_get(w, intern_id(sy, "armed(guard)")));
    CHECK(world_get(w, intern_id(sy, "alert(guard)")));
    CHECK(world_get(w, intern_id(sy, "armed(mage)")));
    CHECK(world_get(w, intern_id(sy, "armed(thug)")));           /* inertia held it */

    /* world_step_why must still work after a routed step: w->fam did not hold the
     * transition, so the trace is replayed from the snapshot. Ask why guard ends
     * up armed next — the trace should name the causal rule that fired. */
    {
        char *buf = NULL; size_t n = 0;
        FILE *m = open_memstream(&buf, &n);
        world_step_why(w, dl_pos(intern_id(sy, "armed(guard)")), true, m);
        fclose(m);
        CHECK(buf && strstr(buf, "arm") != NULL);
        free(buf);
    }

    /* contested transition on the routed path: arm and disarm the same entity in
     * one step — two causal rules concluding armed(guard)' and ~armed(guard)',
     * neither superior, so the fluent is undecided. world_step must reject it
     * (-1) without mutating, naming the offending fluent, exactly as N=1 does. */
    bool was_armed = world_get(w, intern_id(sy, "armed(guard)"));
    uint32_t disarm_guard = intern_id(sy, "disarm(guard)");
    uint32_t clash[2] = { arm_guard, disarm_guard };
    err[0] = '\0';
    CHECK(world_step(w, clash, 2, err, sizeof err) == -1);
    CHECK(strstr(err, "armed(guard)") != NULL);
    CHECK(world_get(w, intern_id(sy, "armed(guard)")) == was_armed);  /* unmutated */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Step lanes with a GLOBAL fluent read in a requires: `alarm` is arity-0, so it
 * broadcasts to every actor lane — a per-unit action gated by a shared flag. The
 * global is a read-only broadcast input (no primed/inertia); the transition must
 * still match N=1 at both flag states, and world_step must gate correctly. */
static const char *STEP_GLOBAL =
    "sort actor\n"
    "entity (guard, thug, mage : actor)\n"
    "state (armed(actor) alert(actor) alarm)\n"
    "action arm(X: actor): requires alarm causes armed(X)\n"
    "rule wary(X: actor): armed(X)' causes alert(X)\n"
    "init (alarm)\n";

static int test_step_lanes_global(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(STEP_GLOBAL, "stepg.story", sy, &d);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", d.count ? d.items[0].msg : "?");
        intern_free(sy);
        return 1;
    }
    CHECK(d.nerrors == 0);
    CHECK(world_step_lane_family_count(w) == 1);   /* globals didn't force a bail */

    uint32_t arm_guard = intern_id(sy, "arm(guard)");
    uint32_t armed_guard = intern_id(sy, "armed(guard)");
    uint32_t alarm = intern_id(sy, "alarm");

    /* the broadcast global read must be loaded right at BOTH flag states — the
     * differential compares every lane's next-state verdict to N=1 */
    bool ok = false;
    CHECK(world_step_lanes_check(w, &arm_guard, 1, &ok) > 0);   /* alarm = true */
    CHECK(ok);
    world_set(w, alarm, false);
    ok = false;
    CHECK(world_step_lanes_check(w, &arm_guard, 1, &ok) > 0);   /* alarm = false */
    CHECK(ok);

    /* gate on: alarm true -> arm(guard) fires */
    world_set(w, alarm, true);
    world_set(w, armed_guard, false);
    char err[128];
    CHECK(world_step(w, &arm_guard, 1, err, sizeof err) == 0);
    CHECK(world_get(w, armed_guard));
    CHECK(world_get(w, intern_id(sy, "alert(guard)")));        /* ramification too */

    /* gate off: alarm false -> arm(guard) blocked, inertia keeps armed false */
    world_set(w, alarm, false);
    world_set(w, armed_guard, false);
    CHECK(world_step(w, &arm_guard, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, armed_guard));

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_homogeneous_agrees()) return 1;
    if (test_mixed_partial()) return 1;
    if (test_unless_lanes()) return 1;
    if (test_join_matcher()) return 1;
    if (test_join3_matcher()) return 1;
    if (test_derived_body_join()) return 1;
    if (test_step_lanes()) return 1;
    if (test_step_lanes_global()) return 1;
    printf("test_lanes: all passed\n");
    return 0;
}
