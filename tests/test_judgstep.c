/* Differential golden test for JUDGMENTS-IN-STEP: a laned numeric transition that
 * coexists with read-side judgment rules (queried by the host, not gating the
 * transition) — including a numeric-guard judgment like the spellbook's
 * `down: hp<=0`. L routes the transition bit-parallel while its judgments stay on
 * jfam; N adds a 2-var action to force the N=1 oracle. Both are stepped in
 * lockstep and EVERY read agrees: hp, the boolean fluent, AND both judgments
 * (world_query down / smoldering). Proves a judgment never blocks a laned step and
 * its query answer is unchanged by routing (I1: judgments never touch fluents). */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#define BODY \
    "entity ( u0, u1, u2, u3 : unit )\n" \
    "state ( hp(unit) : int in 0 .. 20   on_fire(unit) )\n" \
    "init ( hp(u0)=6 hp(u1)=6 hp(u2)=6 hp(u3)=6  on_fire(u1) on_fire(u3) )\n" \
    "action tick(X: unit):   causes hp(X) -= 1\n" \
    "action ignite(X: unit): causes on_fire(X)\n" \
    "rule burn(X: unit): on_fire(X) causes hp(X) -= 5\n" \
    "rule down(X: unit):       hp(X) <= 0  -> down(X)\n" \
    "rule smoldering(X: unit): on_fire(X)  => smoldering(X)\n"

static const char *SRC_L = "sort unit\n" BODY;
static const char *SRC_N = "sort unit\n" BODY
    "action pin(A: unit, B: unit): causes on_fire(A)\n";   /* 2-var -> N=1 (never cast) */

static world *compile(const char *src, intern *sy)
{
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

static const char *U[] = { "u0", "u1", "u2", "u3" };

static int q(world *w, intern *sy, const char *fmt, const char *u)
{ char b[64]; snprintf(b, sizeof b, fmt, u); return world_query(w, dl_pos(intern_id(sy, b))) == DL_PROVED; }

static int step_both(world *L, intern *sl, world *N, intern *sn, const char *act)
{
    char err[128];
    uint32_t aL = intern_id(sl, act), aN = intern_id(sn, act);
    CHECK(world_step(L, &aL, 1, err, sizeof err) == 0);
    CHECK(world_step(N, &aN, 1, err, sizeof err) == 0);
    for (int u = 0; u < 4; u++) {
        char b[64];
        snprintf(b, sizeof b, "hp(%s)", U[u]);
        if (world_get_num(L, intern_id(sl, b)) != world_get_num(N, intern_id(sn, b))) {
            fprintf(stderr, "hp(%s) mismatch after %s\n", U[u], act); return 1; }
        if (q(L, sl, "on_fire(%s)", U[u]) != q(N, sn, "on_fire(%s)", U[u])) {
            fprintf(stderr, "on_fire(%s) mismatch after %s\n", U[u], act); return 1; }
        /* the judgments: queried on the routed world vs the N=1 world */
        if (q(L, sl, "down(%s)", U[u]) != q(N, sn, "down(%s)", U[u])) {
            fprintf(stderr, "down(%s) mismatch after %s\n", U[u], act); return 1; }
        if (q(L, sl, "smoldering(%s)", U[u]) != q(N, sn, "smoldering(%s)", U[u])) {
            fprintf(stderr, "smoldering(%s) mismatch after %s\n", U[u], act); return 1; }
    }
    return 0;
}

int main(void)
{
    intern *sl = intern_new(), *sn = intern_new();
    world *L = compile(SRC_L, sl), *N = compile(SRC_N, sn);
    CHECK(L && N);
    CHECK(world_routes_numeric(L) == true);    /* judgments no longer block the step */
    CHECK(world_routes_numeric(N) == false);

    const char *traj[] = { "tick(u0)", "tick(u0)", "ignite(u0)", "tick(u0)", "tick(u2)" };
    for (size_t i = 0; i < sizeof traj / sizeof traj[0]; i++)
        if (step_both(L, sl, N, sn, traj[i])) return 1;

    /* sanity: burn drove some unit to 0 and `down` fired on the routed world */
    CHECK(q(L, sl, "down(%s)", "u1") == 1);

    world_free(L); world_free(N);
    intern_free(sl); intern_free(sn);
    printf("test_judgstep: all passed\n");
    return 0;
}
