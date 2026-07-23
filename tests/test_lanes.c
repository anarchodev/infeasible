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

int main(void)
{
    if (test_homogeneous_agrees()) return 1;
    if (test_mixed_partial()) return 1;
    if (test_unless_lanes()) return 1;
    printf("test_lanes: all passed\n");
    return 0;
}
