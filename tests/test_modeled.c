/* Golden test for primed numeric READS (the modeled typed-damage pipeline /
 * #87 extension).
 *
 * `f(X)'` inside a RAMIFICATION's effect expression reads the next value a
 * lower stratum committed this tick (EXPR_LOADN). With it, the MODELED
 * typed-damage pipeline (DESIGN §5.8) is expressible in `.story`: actions
 * accumulate `inc_fire(X) +=` at stratum 0 — a per-tick TRANSIENT
 * accumulator built from the ordinary operator-class pipeline (an
 * always-firing `:= 0` ramification is the base, the deltas sum on top, so
 * every tick starts from zero with no reset machinery) — and a stratum-1
 * ramification applies the 5e response as branch-free test() arithmetic over
 * `inc_fire(X)'` and subtracts from hp.
 *
 * The hp trajectories below are ABSOLUTE goldens, preserved verbatim from
 * the configured engine stage at the moment of its removal (#84 decision):
 * this file was the modeled ≡ configured equivalence pin while both forms
 * existed, and the expected values are the configured stage's last word —
 * including the sum-then-halve case (two 3s resisted are floor(6/2)=3, not
 * 1+1) that distinguishes per-type summation from per-attack response. */

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

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

/* --- LOADN directly: a mirror fluent tracking hp' across strata --- */
static int test_mirror(void)
{
    const char *src =
        "sort unit\n"
        "entity u : unit\n"
        "state ( on  hp(unit) : int in 0 .. 20  mirror(unit) : int )\n"
        "init ( on  hp(u) = 10 )\n"
        "rule track(X: unit): on causes mirror(X) := hp(X)'\n"
        "action strike(T: unit): causes hp(T) -= 3\n"
        "action wait: causes on\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    char err[128];
    for (int t = 0; t < 3; t++) {
        uint32_t a = intern_id(sy, t == 1 ? "wait" : "strike(u)");
        CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
        CHECK(num(w, sy, "mirror(u)") == num(w, sy, "hp(u)"));   /* same tick */
    }
    CHECK(num(w, sy, "hp(u)") == 4);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the modeled pipeline, over one scripted history --- */

static const char *JUDGE =
    "rule r_f(X: unit): raging(X) => resistant(X, fire)\n"
    "rule v_f(X: unit): soaked(X) => vulnerable(X, fire)\n"
    "rule i_f(X: unit): stoned(X) => immune(X, fire)\n";

static const char *MODELED =
    "sort unit\n"
    "entity ( u0, u1, u2, u3 : unit )\n"
    "enum damage_type { fire, cold }\n"
    "state (\n"
    "    on  raging(unit)  soaked(unit)  stoned(unit)\n"
    "    hp(unit) : int in 0 .. 400\n"
    "    inc_fire(unit) : int\n"
    "    inc_cold(unit) : int\n"
    ")\n"
    "init ( on  hp(u0)=400 hp(u1)=400 hp(u2)=400 hp(u3)=400\n"
    "       raging(u1)  soaked(u2)  stoned(u3) )\n"
    "%JUDGE%"
    "// per-tick transient accumulators: the := 0 base always fires, the\n"
    "// actions' deltas sum on top — set-before-add IS the reset\n"
    "rule zf(X: unit): on causes inc_fire(X) := 0\n"
    "rule zc(X: unit): on causes inc_cold(X) := 0\n"
    "action burn(T: unit):   causes inc_fire(T) += 3\n"
    "action burn2(T: unit):  causes inc_fire(T) += 3\n"
    "action chill(T: unit):  causes inc_cold(T) += 5\n"
    "action douse(X: unit):  causes soaked(X)\n"
    "// stratum 1: the 5e response over the tick's per-type SUM, branch-free\n"
    "rule apply_f(X: unit): inc_fire(X)' >= 1 causes\n"
    "    hp(X) -= (1 - test(immune(X, fire)))\n"
    "             * (inc_fire(X)'\n"
    "                + test(resistant(X, fire)) * (1 - test(vulnerable(X, fire)))\n"
    "                  * (inc_fire(X)' / 2 - inc_fire(X)')\n"
    "                + test(vulnerable(X, fire)) * (1 - test(resistant(X, fire)))\n"
    "                  * inc_fire(X)')\n"
    "// cold has no responses in this world: raw\n"
    "rule apply_c(X: unit): inc_cold(X)' >= 1 causes hp(X) -= inc_cold(X)'\n";

static char *subst_judge(const char *tmpl)
{
    static char buf[4096];
    const char *at = strstr(tmpl, "%JUDGE%");
    snprintf(buf, sizeof buf, "%.*s%s%s", (int)(at - tmpl), tmpl, JUDGE,
             at + strlen("%JUDGE%"));
    return buf;
}

static int test_trajectory(void)
{
    intern *sm = intern_new();
    world *M = compile_ok(subst_judge(MODELED), sm);
    CHECK(M != NULL);

    /* one scripted history; per-step hp goldens are the configured stage's
     * last output before its removal (see the header comment) */
    static const struct { const char *a, *b; long hp[4]; } SCRIPT[] = {
        { "burn(u0)", NULL,        { 397, 400, 400, 400 } },  /* no response: 3 */
        { "burn(u1)", "burn2(u1)", { 397, 397, 400, 400 } },  /* resist: floor(6/2)=3 */
        { "burn(u2)", NULL,        { 397, 397, 394, 400 } },  /* vulnerable: 6 */
        { "burn(u3)", "burn2(u3)", { 397, 397, 394, 400 } },  /* immune: 0 */
        { "chill(u1)", NULL,       { 397, 392, 394, 400 } },  /* no cold response: 5 */
        { "douse(u1)", NULL,       { 397, 392, 394, 400 } },  /* u1 raging AND soaked */
        { "burn(u1)", NULL,        { 397, 389, 394, 400 } },  /* cancel: raw 3 */
        { "burn(u0)", "chill(u0)", { 389, 389, 394, 400 } },  /* two types, one tick */
    };
    static const char *UNITS[] = { "u0", "u1", "u2", "u3" };
    for (size_t s = 0; s < sizeof SCRIPT / sizeof SCRIPT[0]; s++) {
        uint32_t am[2];
        int n = SCRIPT[s].b ? 2 : 1;
        am[0] = intern_id(sm, SCRIPT[s].a);
        if (n == 2) am[1] = intern_id(sm, SCRIPT[s].b);
        char err[128];
        CHECK(world_step(M, am, n, err, sizeof err) == 0);
        for (int u = 0; u < 4; u++) {
            char b[32];
            snprintf(b, sizeof b, "hp(%s)", UNITS[u]);
            long hm = num(M, sm, b);
            if (hm != SCRIPT[s].hp[u]) {
                fprintf(stderr, "MISMATCH step %zu: %s expected=%ld got=%ld\n",
                        s, b, SCRIPT[s].hp[u], hm);
                return 1;
            }
        }
    }
    world_free(M);
    intern_free(sm);
    return 0;
}

/* --- misuse and cycles --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        /* primed read in an ACTION effect (stratum 0 has nothing below) */
        { "sort unit\nentity u : unit\nstate ( hp(unit) : int  m(unit) : int )\n"
          "action a(X: unit): causes m(X) := hp(X)'\n",
          "only legal in a ramification's" },
        /* primed read in a judgment guard expression */
        { "sort unit\nentity u : unit\nstate hp(unit) : int\n"
          "rule r(X: unit): 0 + hp(X)' >= 1 => q(X)\n",
          "only legal in a ramification's" },
        /* self-feeding primed read: oscillates */
        { "sort unit\nentity u : unit\nstate ( on  acc(unit) : int )\n"
          "init on\n"
          "rule grow(X: unit): on causes acc(X) := acc(X)' + 1\n",
          "through a primed read" },
        /* a derived value has no primed form */
        { "sort unit\nentity u : unit\nstate ( on  m(unit) : int )\n"
          "value v(unit) : int\nrule d(X: unit): => v(X) = 3\n"
          "rule t(X: unit): on causes m(X) := v(X)'\n",
          "no primed form" },
        /* the removed `as <type>` surface refuses with a pointer at the
         * modeled idiom (#84) */
        { "sort unit\nentity u : unit\nenum damage_type { fire }\n"
          "state hp(unit) : int\n"
          "action burn(T: unit): causes hp(T) -= 3 as fire\n",
          "typed contributions were removed" },
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
    if (test_mirror()) return 1;
    if (test_trajectory()) return 1;
    if (test_errors()) return 1;
    printf("test_modeled: all passed\n");
    return 0;
}
