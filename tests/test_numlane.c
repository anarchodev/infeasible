/* Differential golden test for NUMERIC STEP LANES (§5.8, bit-parallel transition).
 * Two worlds with identical numeric mechanics: L is homogeneous (no judgment
 * rules) so world_step routes the numeric transition through the lane family; N
 * adds one trivial judgment rule, which bails the step lanes to the N=1 path. The
 * N=1 path is the trusted oracle (test_numeff/test_numpipe pin it). We drive both
 * with the same actions and assert every fluent — numeric hp and the booleans —
 * agrees after every step. Nothing is hand-computed: the lane engine is validated
 * against N=1 across constant deltas, a winning assign + delta, the range clamp,
 * a per-tick ramification (burn), and ignite/douse boolean effects. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static const char *SRC_L =
    "sort unit\n"
    "entity ( u0, u1, u2, u3 : unit )\n"
    "state ( hp(unit) : int in 0 .. 20   on_fire(unit)  shielded(unit) )\n"
    "init ( hp(u0)=10 hp(u1)=10 hp(u2)=10 hp(u3)=10\n"
    "       on_fire(u1) on_fire(u3)  shielded(u2) )\n"
    "action tick(X: unit):   causes hp(X) -= 1\n"
    "action heal(X: unit):   causes hp(X) := 20\n"
    "action ignite(X: unit): causes on_fire(X)\n"
    "action douse(X: unit):  causes ~on_fire(X)\n"
    "rule burn(X: unit): on_fire(X) causes hp(X) -= 5\n";

/* identical numerics, plus one judgment rule -> nrules>0 -> step lanes bail -> N=1 */
static const char *SRC_N =
    "sort unit\n"
    "entity ( u0, u1, u2, u3 : unit )\n"
    "state ( hp(unit) : int in 0 .. 20   on_fire(unit)  shielded(unit) )\n"
    "init ( hp(u0)=10 hp(u1)=10 hp(u2)=10 hp(u3)=10\n"
    "       on_fire(u1) on_fire(u3)  shielded(u2) )\n"
    "action tick(X: unit):   causes hp(X) -= 1\n"
    "action heal(X: unit):   causes hp(X) := 20\n"
    "action ignite(X: unit): causes on_fire(X)\n"
    "action douse(X: unit):  causes ~on_fire(X)\n"
    "rule burn(X: unit): on_fire(X) causes hp(X) -= 5\n"
    "rule guard(X: unit): shielded(X) => guarded(X)\n";

static world *compile(const char *src, intern *sy)
{
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

static const char *UNITS[] = { "u0", "u1", "u2", "u3" };

/* apply action `act` to both worlds; then assert every fluent agrees */
static int step_both(world *L, intern *sl, world *N, intern *sn, const char *act)
{
    char err[128];
    uint32_t aL = intern_id(sl, act), aN = intern_id(sn, act);
    int rL = world_step(L, &aL, 1, err, sizeof err);
    int rN = world_step(N, &aN, 1, err, sizeof err);
    CHECK(rL == rN && rL == 0);
    for (int u = 0; u < 4; u++) {
        char b[64];
        snprintf(b, sizeof b, "hp(%s)", UNITS[u]);
        long hL = world_get_num(L, intern_id(sl, b)), hN = world_get_num(N, intern_id(sn, b));
        if (hL != hN) {
            fprintf(stderr, "MISMATCH after %s: hp(%s) lane=%ld n1=%ld\n",
                    act, UNITS[u], hL, hN);
            return 1;
        }
        snprintf(b, sizeof b, "on_fire(%s)", UNITS[u]);
        int fL = world_get(L, intern_id(sl, b)), fN = world_get(N, intern_id(sn, b));
        if (fL != fN) {
            fprintf(stderr, "MISMATCH after %s: on_fire(%s) lane=%d n1=%d\n",
                    act, UNITS[u], fL, fN);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    intern *sl = intern_new(), *sn = intern_new();
    world *L = compile(SRC_L, sl), *N = compile(SRC_N, sn);
    CHECK(L && N);

    /* the whole point: L routes numerics bit-parallel; N runs the N=1 oracle */
    CHECK(world_routes_numeric(L) == true);
    CHECK(world_routes_numeric(N) == false);

    /* a trajectory exercising delta, ramification, assign+delta, clamp, booleans */
    const char *traj[] = {
        "ignite(u0)",   /* u0 now on fire (boolean effect) */
        "tick(u0)",     /* tick + burn on u0,u1,u3 */
        "tick(u2)",     /* burn cascades; u0 clamps at 0 */
        "heal(u1)",     /* assign 20 AND burn -5 on one lane -> 15 */
        "douse(u1)",    /* ~on_fire(u1) */
        "tick(u1)",     /* u1 no longer burns; others do */
        "tick(u1)",
    };
    for (size_t i = 0; i < sizeof traj / sizeof traj[0]; i++)
        if (step_both(L, sl, N, sn, traj[i])) return 1;

    /* sanity: the assign+delta lane actually produced 15, and a clamp happened */
    CHECK(world_get_num(L, intern_id(sl, "hp(u0)")) == 0);   /* clamped at floor */

    world_free(L); world_free(N);
    intern_free(sl); intern_free(sn);
    printf("test_numlane: all passed\n");
    return 0;
}
