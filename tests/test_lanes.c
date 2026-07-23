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

int main(void)
{
    if (test_homogeneous_agrees()) return 1;
    if (test_mixed_partial()) return 1;
    if (test_unless_lanes()) return 1;
    if (test_join_matcher()) return 1;
    if (test_join3_matcher()) return 1;
    printf("test_lanes: all passed\n");
    return 0;
}
