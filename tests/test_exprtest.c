/* Golden test for EXPR_TEST (#86): `test([~]p(args))` — a literal's solved
 * verdict as 0/1 inside an effect expression. The branch-free modifier shape
 * `base + flag * delta` replaces the 2^k case-split grounding the epic (#79)
 * flags as the M3 risk.
 *
 * Pinned:
 *  - boolean state, negated literals, and DERIVED judgments all test 0/1
 *    against the pre-step solved theory;
 *  - test() is the literal's verdict, exactly as a body atom reads it — NOT
 *    negation-as-failure: an UNDECIDED derived atom tests 0 both ways, and
 *    even a REFUTED derived atom tests 0 under `~` (nothing concludes the
 *    negation); base fluents, being closed-world, flip 0/1 as expected;
 *  - replay stability (§5.10): because rolls are keyed lookups, a die drawn
 *    and multiplied by 0 perturbs nothing — the base draw is identical in a
 *    world where the modifier applies and one where it does not;
 *  - the stratification boundary (#87) holds as located errors: test(…) is
 *    rejected in expression guards, clamp bounds, and value definitions
 *    (where it could flow into a guard through an inlined read). */

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

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

static const char *SRC =
    "sort actor\n"
    "entity ( bran, grik : actor )\n"
    "state (\n"
    "    blessed(actor)  raging(actor)\n"
    "    hp(actor) : int in 0 .. 99\n"
    "    gold : int\n"
    ")\n"
    "init ( hp(bran) = 99  hp(grik) = 99 )\n"
    "rule fierce(X: actor): raging(X) => fierce(X)\n"
    "rule mk: raging(grik) => marked(grik)\n"
    "// branch-free modifiers: base + flag * delta\n"
    "action strike(A: actor, T: actor): causes hp(T) -= 5 + test(blessed(A)) * 3\n"
    "action smite(A: actor, T: actor):  causes hp(T) -= 5 + test(~blessed(A)) * 2\n"
    "action maul(A: actor, T: actor):   causes hp(T) -= 2 + test(fierce(A)) * 4\n"
    "// derived atoms are NOT closed-world: marked(bran) (mentioned by no rule)\n"
    "// tests 0 both ways, and ~fierce(bran) tests 0 too — refuting fierce does\n"
    "// not PROVE its negation (DL, not negation-as-failure); test() mirrors\n"
    "// body-literal semantics exactly\n"
    "action probe: causes gold += 10 + test(marked(bran)) * 3\n"
    "                          + test(~marked(bran)) * 5\n"
    "                          + test(~fierce(bran)) * 7\n"
    "action bless(X: actor): causes blessed(X)\n"
    "action curse(X: actor): causes ~blessed(X)\n"
    "action enrage(X: actor): causes raging(X)\n";

static int test_modifiers(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "strike(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 94);           /* unblessed: 5 */
    CHECK(step1(w, sy, "bless(bran)") == 0);
    CHECK(step1(w, sy, "strike(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 86);           /* blessed: 5 + 3 */

    CHECK(step1(w, sy, "smite(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 81);           /* blessed: ~blessed tests 0 */
    CHECK(step1(w, sy, "curse(bran)") == 0);
    CHECK(step1(w, sy, "smite(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 74);           /* unblessed: 5 + 2 */

    /* a DERIVED judgment, read pre-step */
    CHECK(step1(w, sy, "maul(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 72);           /* not fierce: 2 */
    CHECK(step1(w, sy, "enrage(bran)") == 0);
    CHECK(step1(w, sy, "maul(bran,grik)") == 0);
    CHECK(num(w, sy, "hp(grik)") == 66);           /* fierce: 2 + 4 */

    world_free(w);
    intern_free(sy);
    return 0;
}

static int test_tri_valued(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    /* marked(bran): no rule mentions it -> UNDECIDED -> 0 both ways.
     * ~fierce(bran): fierce is REFUTED, but nothing CONCLUDES ¬fierce, so the
     * negative literal is unprovable too -> 0. test() is the verdict of the
     * literal, exactly as a body atom would read it — not negation-as-failure.
     * (Base fluents differ: closed-world asserts f or ~f every step, so
     * test(~blessed(A)) in test_modifiers really flips 0/1.) */
    CHECK(step1(w, sy, "probe") == 0);
    CHECK(num(w, sy, "gold") == 10);               /* 10 + 0 + 0 + 0 */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Replay stability: identical stories and seeds, one world blessed and one
 * not, stepped in lockstep. The zap's base d8 draw must be IDENTICAL in both
 * (the discarded d4 in the unblessed world perturbs nothing — §5.10 keyed
 * lookup), so the per-tick damage difference is exactly the d4 modifier. */
static int test_replay_stability(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, grik : actor )\n"
        "state ( blessed(actor)  hp(actor) : int in 0 .. 999 )\n"
        "init ( hp(grik) = 999 )\n"
        "action zap(T: actor):\n"
        "    causes hp(T) -= roll(8, 1) + test(blessed(bran)) * roll(4, 2)\n"
        "action bless(X: actor): causes blessed(X)\n"
        "action curse(X: actor): causes ~blessed(X)\n";

    intern *sa = intern_new(), *sb = intern_new();
    world *A = compile_ok(src, sa), *B = compile_ok(src, sb);
    CHECK(A != NULL && B != NULL);
    world_set_seed(A, 424242uLL);
    world_set_seed(B, 424242uLL);
    /* one setup step each, keeping the tick counters aligned */
    CHECK(step1(A, sa, "bless(bran)") == 0);
    CHECK(step1(B, sb, "curse(bran)") == 0);

    long prevA = 999, prevB = 999;
    int distinct_base = 0; long lastbase = -1;
    for (int t = 0; t < 30; t++) {
        CHECK(step1(A, sa, "zap(grik)") == 0);
        CHECK(step1(B, sb, "zap(grik)") == 0);
        long dA = prevA - num(A, sa, "hp(grik)");
        long dB = prevB - num(B, sb, "hp(grik)");
        prevA -= dA; prevB -= dB;
        CHECK(dB >= 1 && dB <= 8);                 /* the shared base d8 */
        CHECK(dA - dB >= 1 && dA - dB <= 4);       /* exactly the d4 modifier */
        if (dB != lastbase) { distinct_base++; lastbase = dB; }
    }
    CHECK(distinct_base >= 3);                     /* the die actually varies */

    world_free(A); world_free(B);
    intern_free(sa); intern_free(sb);
    return 0;
}

static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "sort actor\nentity a : actor\nstate ( p  q(actor) )\n"
          "rule r(X: actor): 0 + test(p) >= 1 => q(X)\n",
          "needs the §5.8 stratifier" },
        { "state ( p  x : int in 0 .. 5 + test(p) * 2 )\n",
          "may not use test" },
        { "state p\nvalue v : int\nrule d: => v = test(p) * 2\n"
          "state gold : int\naction a: causes gold += v\n",
          "could flow into a guard" },
        { "sort actor\nentity a : actor\nstate hp(actor) : int\n"
          "action x(T: actor): causes hp(T) -= test(hp(T)) * 2\n",
          "takes a boolean literal" },
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
    if (test_modifiers()) return 1;
    if (test_tri_valued()) return 1;
    if (test_replay_stability()) return 1;
    if (test_errors()) return 1;
    printf("test_exprtest: all passed\n");
    return 0;
}
