/* Differential golden test for test() READS IN A LANED NUMERIC RHS (#165, §5.8).
 *
 * A verdict is 0 or 1, so an effect RHS over k test() reads takes at most 2^k
 * values, all folded at compile time; the lane commit assembles a k-bit index
 * from those columns' verdicts and looks the delta up. This test pins that table
 * against the N=1 effect VM, which evaluates the same expression tree.
 *
 * The trajectory is not hand-computed except for one spot-check: L is the
 * laneable world, N adds a 2-var action so emit_step_lanes bails, and every
 * fluent must agree after every step. The eight units carry the eight distinct
 * (immune, resist, vuln) flag combinations, so ONE burn of all of them exercises
 * every entry of the 3-read table — including the resist AND vuln case, where the
 * two exceptions cancel, and the immune case, which zeroes a nonzero subtree.
 *
 * `zap` covers the other axis: a NEGATED read, `test(~immune(T))`, whose column
 * polarity is carried separately from the atom. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* u_i carries flags by bit: 1=immune, 2=resist, 4=vuln — all eight combinations */
#define VOCAB \
    "sort unit\n" \
    "entity ( u0, u1, u2, u3, u4, u5, u6, u7 : unit )\n" \
    "state ( hp(unit) : int in 0 .. 400  immune(unit) resist(unit) vuln(unit) )\n" \
    "init ( hp(u0)=400 hp(u1)=400 hp(u2)=400 hp(u3)=400\n" \
    "       hp(u4)=400 hp(u5)=400 hp(u6)=400 hp(u7)=400\n" \
    "       immune(u1) resist(u2) immune(u3) resist(u3)\n" \
    "       vuln(u4)   immune(u5) vuln(u5)   resist(u6) vuln(u6)\n" \
    "       immune(u7) resist(u7) vuln(u7) )\n"

/* the 5e response ladder: immunity zeroes, resistance halves the TOTAL, and
 * vulnerability doubles — each exception guarded against the other */
#define RULES \
    "action burn(T: unit): causes hp(T) -= (1 - test(immune(T)))\n" \
    "    * (7 + test(resist(T)) * (1 - test(vuln(T))) * (7 / 2 - 7)\n" \
    "         + test(vuln(T)) * (1 - test(resist(T))) * 7)\n" \
    "action zap(T: unit): causes hp(T) -= 5 * test(~immune(T))\n" \
    "action douse(T: unit): causes ~immune(T)\n" \
    "action ward(T: unit): causes immune(T)\n" \
    "exclusive douse(X), ward(X)\n"

static const char *SRC_L = VOCAB RULES;
/* identical mechanics + a 2-var action (never cast) -> emit_step_lanes bails */
static const char *SRC_N = VOCAB RULES
    "action pin(A: unit, B: unit): causes resist(A)\n";

static world *compile(const char *src, intern *sy)
{
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?");
    else if (dg.nerrors) fprintf(stderr, "errors: %s\n", di[0].msg);
    return (w && dg.nerrors == 0) ? w : NULL;
}

static const char *UNITS[] = { "u0", "u1", "u2", "u3", "u4", "u5", "u6", "u7" };
enum { NU = 8 };

/* drive both worlds with the same action set; assert every fluent agrees */
static int step_both(world *L, intern *sl, world *N, intern *sn,
                     const char **acts, int nacts)
{
    char err[128];
    uint32_t aL[NU], aN[NU];
    for (int i = 0; i < nacts; i++) {
        aL[i] = intern_id(sl, acts[i]);
        aN[i] = intern_id(sn, acts[i]);
    }
    int rL = world_step(L, aL, nacts, err, sizeof err);
    int rN = world_step(N, aN, nacts, err, sizeof err);
    CHECK(rL == rN && rL == 0);
    for (int u = 0; u < NU; u++) {
        char b[64];
        snprintf(b, sizeof b, "hp(%s)", UNITS[u]);
        long hL = world_get_num(L, intern_id(sl, b));
        long hN = world_get_num(N, intern_id(sn, b));
        if (hL != hN) {
            fprintf(stderr, "MISMATCH after %s...: hp(%s) lane=%ld n1=%ld\n",
                    acts[0], UNITS[u], hL, hN);
            return 1;
        }
        snprintf(b, sizeof b, "immune(%s)", UNITS[u]);
        int fL = world_get(L, intern_id(sl, b)), fN = world_get(N, intern_id(sn, b));
        if (fL != fN) {
            fprintf(stderr, "MISMATCH after %s...: immune(%s) lane=%d n1=%d\n",
                    acts[0], UNITS[u], fL, fN);
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

    /* the premise: a test()-reading RHS no longer bails the numeric transition */
    CHECK(world_routes_numeric(L) == true);
    CHECK(world_routes_numeric(N) == false);

    /* one burn of all eight units = every entry of the 3-read table */
    const char *all_burn[NU];
    for (int u = 0; u < NU; u++) {
        static char buf[NU][16];
        snprintf(buf[u], sizeof buf[u], "burn(%s)", UNITS[u]);
        all_burn[u] = buf[u];
    }
    if (step_both(L, sl, N, sn, all_burn, NU)) return 1;

    /* the one hand-computed check, so a shared bug in both paths cannot pass:
     * base 7; resist halves the total (7/2 = 3, floored); vuln doubles (14);
     * both cancel (7); immune zeroes regardless of the rest. */
    static const long DROP[NU] = { 7, 0, 3, 0, 14, 0, 7, 0 };
    for (int u = 0; u < NU; u++) {
        char b[64];
        snprintf(b, sizeof b, "hp(%s)", UNITS[u]);
        long got = world_get_num(L, intern_id(sl, b));
        if (got != 400 - DROP[u]) {
            fprintf(stderr, "FAIL table entry %d: hp(%s)=%ld expected %ld\n",
                    u, UNITS[u], got, 400 - DROP[u]);
            return 1;
        }
    }

    /* negated read: `test(~immune(T))` is 5 damage on the non-immune */
    const char *all_zap[NU];
    for (int u = 0; u < NU; u++) {
        static char buf[NU][16];
        snprintf(buf[u], sizeof buf[u], "zap(%s)", UNITS[u]);
        all_zap[u] = buf[u];
    }
    if (step_both(L, sl, N, sn, all_zap, NU)) return 1;
    CHECK(world_get_num(L, intern_id(sl, "hp(u0)")) == 400 - 7 - 5);
    CHECK(world_get_num(L, intern_id(sl, "hp(u1)")) == 400);       /* immune: neither */

    /* flip the columns underneath the table: the index must follow the state,
     * not the compile — u1 loses immunity and starts taking full damage. */
    const char *flip[] = { "douse(u1)", "ward(u0)" };
    if (step_both(L, sl, N, sn, flip, 2)) return 1;
    if (step_both(L, sl, N, sn, all_burn, NU)) return 1;
    CHECK(world_get_num(L, intern_id(sl, "hp(u1)")) == 400 - 7);   /* now hurt */
    CHECK(world_get_num(L, intern_id(sl, "hp(u0)")) == 400 - 7 - 5); /* now immune */

    if (step_both(L, sl, N, sn, all_zap, NU)) return 1;
    if (step_both(L, sl, N, sn, all_burn, NU)) return 1;

    world_free(L); world_free(N);
    intern_free(sl); intern_free(sn);
    printf("test_testlane: all passed\n");
    return 0;
}
