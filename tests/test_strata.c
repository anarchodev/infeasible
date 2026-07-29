/* Golden test for §5.8 stratification (#87): primed NUMERIC guards, strata
 * within one tick, cycles rejected.
 *
 * Pinned:
 *  - the dying trigger: `hp(X)' <= 0 causes dead(X)` — dead lands in the SAME
 *    tick as the damage that killed, not one tick late;
 *  - a three-layer cascade (damage -> hp' -> dead + loot wiped -> gold' ->
 *    buried) resolves in ONE step, strata ordered by the compiler;
 *  - a primed BOOLEAN read chains inside the same stratum's solve (mourned);
 *  - commit-time visibility: a current-state atom read by an upper-stratum
 *    effect (via test()) stays PRE-step — dying now is not dead yet;
 *  - atomicity: a contested assign in an UPPER stratum aborts the whole tick
 *    with nothing mutated — replay never sees a half-tick (I4);
 *  - oscillating primed-numeric cycles (direct and through a primed boolean)
 *    are located compile errors;
 *  - a primed guard over a non-numeric fluent is a located error. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

static int step1(world *w, intern *sy, const char *action)
{
    uint32_t a = intern_id(sy, action);
    char err[128];
    int r = world_step(w, &a, 1, err, sizeof err);
    if (r) fprintf(stderr, "  step %s: %s\n", action, err);
    return r;
}

static dl_verdict q(world *w, intern *sy, const char *atom)
{
    return world_query(w, (dl_lit){ intern_id(sy, atom), false });
}

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

/* --- the dying trigger: dead in the SAME tick as the killing blow --- */
static int test_dying_trigger(void)
{
    const char *src =
        "sort unit\n"
        "entity ( u0, u1 : unit )\n"
        "state ( dead(unit)  hp(unit) : int in 0 .. 20 )\n"
        "init ( hp(u0) = 10  hp(u1) = 10 )\n"
        "rule dying(X: unit): hp(X)' <= 0 causes dead(X)\n"
        "action strike(T: unit): causes hp(T) -= 12\n"
        "action tap(T: unit):    causes hp(T) -= 1\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "dead(u0)") == DL_REFUTED);
    CHECK(step1(w, sy, "strike(u0)") == 0);
    CHECK(num(w, sy, "hp(u0)") == 0);
    CHECK(q(w, sy, "dead(u0)") == DL_PROVED);      /* same tick, not one late */
    CHECK(q(w, sy, "dead(u1)") == DL_REFUTED);

    CHECK(step1(w, sy, "tap(u1)") == 0);           /* above zero: no trigger */
    CHECK(num(w, sy, "hp(u1)") == 9);
    CHECK(q(w, sy, "dead(u1)") == DL_REFUTED);
    CHECK(q(w, sy, "dead(u0)") == DL_PROVED);      /* and death persists */

    /* the step-why trace still renders for the stratified transition */
    FILE *sink = fopen("/dev/null", "w");
    if (sink) {
        world_step_why(w, (dl_lit){ intern_id(sy, "dead(u1)"), false }, true, sink);
        fclose(sink);
    }

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- three strata in one step, plus a primed-boolean chain and the
 *     commit-time visibility rule --- */
static int test_cascade(void)
{
    const char *src =
        "sort unit\n"
        "entity ( u0, u1 : unit )\n"
        "state (\n"
        "    dead(unit)  buried(unit)  mourned(unit)\n"
        "    hp(unit)   : int in 0 .. 20\n"
        "    gold(unit) : int\n"
        "    loot : int\n"
        ")\n"
        "init ( hp(u0) = 10  hp(u1) = 10  gold(u0) = 7  gold(u1) = 7  loot = 50 )\n"
        "// stratum 1: gated on hp' (stratum-0 fluent)\n"
        "rule perish(X: unit): hp(X)' <= 0 causes dead(X) & gold(X) := 0\n"
        "// visibility: dead(X) unprimed is the PRE-step fact, even here\n"
        "rule scavenge(X: unit): hp(X)' <= 0 causes loot -= 2 + test(dead(X)) * 5\n"
        "// stratum 1 too: dead(X)' is a primed BOOLEAN — same solve as perish\n"
        "rule mourn(X: unit): dead(X)' causes mourned(X)\n"
        "// stratum 2: gated on gold' (a stratum-1 fluent)\n"
        "rule bury(X: unit): gold(X)' <= 0 & dead(X)' causes buried(X)\n"
        "action strike(T: unit): causes hp(T) -= 12\n"
        "action wait: causes ~buried(u1)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "strike(u0)") == 0);        /* ONE step: */
    CHECK(num(w, sy, "hp(u0)") == 0);              /* stratum 0 */
    CHECK(q(w, sy, "dead(u0)") == DL_PROVED);      /* stratum 1 */
    CHECK(num(w, sy, "gold(u0)") == 0);            /* stratum 1 numeric */
    CHECK(q(w, sy, "mourned(u0)") == DL_PROVED);   /* primed-boolean chain */
    CHECK(q(w, sy, "buried(u0)") == DL_PROVED);    /* stratum 2 */
    CHECK(num(w, sy, "loot") == 48);               /* 2 + 0: dying, not yet dead */

    /* untouched unit: gold(u1)=7 keeps bury off even though dead(u1)' is
     * refuted anyway; nothing leaked across bindings */
    CHECK(q(w, sy, "buried(u1)") == DL_REFUTED);
    CHECK(num(w, sy, "gold(u1)") == 7);

    /* next tick: u0 already dead — perish still fires (hp' still 0), and NOW
     * test(dead(u0)) reads the committed pre-step fact: 1 */
    CHECK(step1(w, sy, "wait") == 0);
    CHECK(num(w, sy, "loot") == 41);               /* 48 - (2 + 5) */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- atomicity: a contested assign in an upper stratum aborts the WHOLE
 *     tick — the stratum-0 damage must not have been committed --- */
static int test_atomic_abort(void)
{
    const char *src =
        "sort unit\n"
        "entity u0 : unit\n"
        "state ( dead(unit)  hp(unit) : int in 0 .. 20  gold(unit) : int )\n"
        "init ( hp(u0) = 10  gold(u0) = 7 )\n"
        "rule perish(X: unit):  hp(X)' <= 0 causes dead(X) & gold(X) := 0\n"
        "rule pilfer(X: unit):  hp(X)' <= 0 causes gold(X) := 3\n"
        "action strike(T: unit): causes hp(T) -= 12\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    uint32_t a = intern_id(sy, "strike(u0)");
    char err[128];
    CHECK(world_step(w, &a, 1, err, sizeof err) == -1);
    CHECK(strstr(err, "conflicting") != NULL);
    CHECK(num(w, sy, "hp(u0)") == 10);             /* stratum 0 NOT committed */
    CHECK(num(w, sy, "gold(u0)") == 7);
    CHECK(q(w, sy, "dead(u0)") == DL_REFUTED);
    CHECK(world_tick(w) == 0);                     /* no half-tick in the log */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- cycles oscillate and are rejected at compile time --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        /* direct: a writer of hp gated on hp' */
        { "sort unit\nentity u : unit\nstate hp(unit) : int in 0 .. 20\n"
          "init hp(u) = 10\n"
          "rule heal_low(X: unit): hp(X)' < 5 causes hp(X) += 3\n",
          "depends on itself through primed guards" },
        /* indirect: hp' -> dead (boolean) -> dead' -> writes hp */
        { "sort unit\nentity u : unit\n"
          "state ( dead(unit)  hp(unit) : int in 0 .. 20 )\n"
          "init hp(u) = 10\n"
          "rule perish(X: unit): hp(X)' <= 0 causes dead(X)\n"
          "rule thrash(X: unit): dead(X)' causes hp(X) += 1\n",
          "depends on itself through primed guards" },
        /* a primed guard needs a numeric fluent */
        { "sort unit\nentity u : unit\nstate ( alive(unit)  fell(unit) )\n"
          "rule r(X: unit): alive(X)' <= 3 causes fell(X)\n",
          "not a declared numeric fluent" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        intern *sy = intern_new();
        story_diag di[16];
        story_diags dg = { di, 16, 0, 0 };
        world *w = story_compile(BAD[i].src, "t.story", sy, &dg);
        if (w != NULL && dg.nerrors == 0) {
            fprintf(stderr, "FAIL %s:%d: case %zu compiled but should not\n",
                    __FILE__, __LINE__, i);
            return 1;
        }
        bool found = false;
        for (int k = 0; k < dg.count && !found; k++)
            found = strstr(dg.items[k].msg, BAD[i].msg) != NULL;
        if (!found) {
            fprintf(stderr, "FAIL %s:%d: case %zu missing \"%s\"; got \"%s\"\n",
                    __FILE__, __LINE__, i, BAD[i].msg,
                    dg.count ? dg.items[0].msg : "(none)");
            return 1;
        }
        if (w) world_free(w);
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_dying_trigger()) return 1;
    if (test_cascade()) return 1;
    if (test_atomic_abort()) return 1;
    if (test_errors()) return 1;
    printf("test_strata: all passed\n");
    return 0;
}
