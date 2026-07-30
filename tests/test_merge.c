/* Golden test for declared merge algebras on numeric fluents (#85):
 * `state f(...) : int [in lo..hi] merge min|max`.
 *
 * The ASSIGN class of a numeric fluent is a CRDT: the default is a
 * conflict-detecting register (two differing `:=`s contest the step — §5.8
 * forbids picking an order among rules), and `merge min|max` swaps in an
 * extreme-taking algebra: multiple firing `:=` contributions merge
 * commutatively and idempotently. Pinned here:
 *  - max of two contributing rules vs one (armor floor: 14 armored, 10 not);
 *  - declaration ORDER does not affect the result (commutativity);
 *  - the same value contributed twice changes nothing, and equal values
 *    never contest (idempotence);
 *  - deltas still sum ON TOP of the merged base; the range clamp stays
 *    outermost; inertia holds when no definition fires;
 *  - the receipt's winning-assign row names the EXTREME's rule;
 *  - the register default still contests (unchanged semantics);
 *  - the routed LANE path agrees with N=1 (differential, test_numlane style).
 */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* ---- engine-level: N=1 pipeline semantics ---- */

static int test_engine_max(void)
{
    intern *sy = intern_new();
    uint32_t ac = intern_id(sy, "ac"), go = intern_id(sy, "go");
    world *w = world_new(sy);
    world_declare_num(w, ac, 0, 0, false);
    world_set_num_merge(w, ac, WORLD_MERGE_MAX);
    world_set_num(w, ac, 1);

    expr_ins ten[] = {{EXPR_CONST, 10}}, fourteen[] = {{EXPR_CONST, 14}};
    int r1 = world_add_step_rule(w, "base_ac", go, NULL, 0, NULL, 0);
    world_add_num_effect(w, r1, ac, WORLD_OP_ASSIGN, ten, 1);
    int r2 = world_add_step_rule(w, "plate", go, NULL, 0, NULL, 0);
    world_add_num_effect(w, r2, ac, WORLD_OP_ASSIGN, fourteen, 1);
    /* a delta lands on top of the merged base */
    expr_ins one[] = {{EXPR_CONST, 1}};
    int r3 = world_add_step_rule(w, "shield", go, NULL, 0, NULL, 0);
    world_add_num_effect(w, r3, ac, WORLD_OP_ADD, one, 1);

    char err[128];
    uint32_t acts[] = { go };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac) == 15);             /* max(10,14) + 1 */

    /* the receipt's assign row names the extreme's rule */
    long base;
    world_contrib items[8];
    int n = world_num_receipt(w, ac, &base, items, 8);
    CHECK(n >= 2);
    CHECK(items[0].op == WORLD_OP_ASSIGN && items[0].amount == 14);
    CHECK(items[0].rule && strcmp(items[0].rule, "plate") == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* min, with the two contributing rules added in the OPPOSITE order — the
 * result must not care (commutativity), and contributing the same extreme
 * twice must change nothing (idempotence). */
static int test_engine_min_order_idem(void)
{
    for (int order = 0; order < 2; order++) {
        intern *sy = intern_new();
        uint32_t bid = intern_id(sy, "bid"), go = intern_id(sy, "go");
        world *w = world_new(sy);
        world_declare_num(w, bid, 0, 0, false);
        world_set_num_merge(w, bid, WORLD_MERGE_MIN);
        world_set_num(w, bid, 99);

        expr_ins three[] = {{EXPR_CONST, 3}}, seven[] = {{EXPR_CONST, 7}},
                 three2[] = {{EXPR_CONST, 3}};
        const expr_ins *first = order ? seven : three;
        const expr_ins *second = order ? three : seven;
        int r1 = world_add_step_rule(w, "offer_a", go, NULL, 0, NULL, 0);
        world_add_num_effect(w, r1, bid, WORLD_OP_ASSIGN, first, 1);
        int r2 = world_add_step_rule(w, "offer_b", go, NULL, 0, NULL, 0);
        world_add_num_effect(w, r2, bid, WORLD_OP_ASSIGN, second, 1);
        int r3 = world_add_step_rule(w, "offer_c", go, NULL, 0, NULL, 0);
        world_add_num_effect(w, r3, bid, WORLD_OP_ASSIGN, three2, 1);  /* dup 3 */

        char err[128];
        uint32_t acts[] = { go };
        CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w, bid) == 3);
        world_free(w);
        intern_free(sy);
    }
    return 0;
}

/* ---- surface: armor floor through .story, inertia, clamp ---- */

static int test_story_floor(void)
{
    const char *src =
        "sort unit\n"
        "entity ( u0, u1 : unit )\n"
        "state (\n"
        "    alive(unit)  armored(unit)\n"
        "    ac(unit) : int in 0 .. 12 merge max\n"
        ")\n"
        "init ( alive(u0) alive(u1) armored(u1)  ac(u0)=1 ac(u1)=1 )\n"
        "// competing definitions of AC — the extreme wins, per entity\n"
        "rule base_ac(X: unit): alive(X)   causes ac(X) := 10\n"
        "rule plate(X: unit):   armored(X) causes ac(X) := 14\n"
        "action doff(X: unit): causes ~armored(X)\n"
        "action slay(X: unit): causes ~alive(X)\n";

    intern *sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    CHECK(w != NULL && dg.nerrors == 0);
    uint32_t ac0 = intern_id(sy, "ac(u0)"), ac1 = intern_id(sy, "ac(u1)");

    char err[128];
    uint32_t a = intern_id(sy, "doff(u0)");        /* a no-op-ish tick driver */
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac0) == 10);            /* base only */
    CHECK(world_get_num(w, ac1) == 12);            /* max(10,14), CLAMPED to 12 */

    a = intern_id(sy, "doff(u1)");                 /* armor comes off (next state) */
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac1) == 12);            /* this step still saw armored */

    a = intern_id(sy, "doff(u0)");                 /* now unarmored: floor remains */
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac1) == 10);

    a = intern_id(sy, "slay(u1)");                 /* dead NEXT state; 10 this step */
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac1) == 10);
    a = intern_id(sy, "doff(u0)");                 /* nothing fires for u1: inertia */
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, ac1) == 10);            /* held, not re-derived */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* the register default is UNCHANGED: two differing `:=`s still contest */
static int test_register_still_contests(void)
{
    intern *sy = intern_new();
    uint32_t x = intern_id(sy, "x"), go = intern_id(sy, "go");
    world *w = world_new(sy);
    world_declare_num(w, x, 0, 0, false);

    expr_ins a[] = {{EXPR_CONST, 3}}, b[] = {{EXPR_CONST, 7}};
    int r1 = world_add_step_rule(w, "ra", go, NULL, 0, NULL, 0);
    world_add_num_effect(w, r1, x, WORLD_OP_ASSIGN, a, 1);
    int r2 = world_add_step_rule(w, "rb", go, NULL, 0, NULL, 0);
    world_add_num_effect(w, r2, x, WORLD_OP_ASSIGN, b, 1);

    char err[128];
    uint32_t acts[] = { go };
    CHECK(world_step(w, acts, 1, err, sizeof err) == -1);
    CHECK(strstr(err, "conflicting") != NULL);
    CHECK(world_get_num(w, x) == 0);               /* nothing mutated */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- the routed lane path agrees with N=1 (test_numlane pattern) ---- */

static const char *LANE_BASE =
    "sort unit\n"
    "entity ( u0, u1, u2 : unit )\n"
    "state (\n"
    "    angry(unit)  scary(unit)\n"
    "    threat(unit) : int in 0 .. 99 merge max\n"
    ")\n"
    "init ( threat(u0)=1 threat(u1)=1 threat(u2)=1\n"
    "       angry(u1)  scary(u1) scary(u2) )\n"
    "rule a(X: unit): angry(X) causes threat(X) := 10\n"
    "rule s(X: unit): scary(X) causes threat(X) := 7\n"
    "action provoke(X: unit): causes angry(X)\n"
    "action calm(X: unit):    causes ~angry(X)\n"
    "exclusive provoke(X), calm(X)\n";

static int test_lane_vs_n1(void)
{
    char srcN[1024];
    /* the never-cast 2-var action bails emit_step_lanes -> N=1 oracle */
    snprintf(srcN, sizeof srcN, "%s%s", LANE_BASE,
             "action pin(A: unit, B: unit): causes scary(A)\n");

    intern *sl = intern_new(), *sn = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *L = story_compile(LANE_BASE, "l.story", sl, &dg);
    CHECK(L != NULL && dg.nerrors == 0);
    dg.count = dg.nerrors = 0;
    world *N = story_compile(srcN, "n.story", sn, &dg);
    CHECK(N != NULL && dg.nerrors == 0);

    static const char *SCRIPT[] = {
        "provoke(u2)", "calm(u1)", "provoke(u0)", "calm(u2)", "calm(u0)",
    };
    static const char *UNITS[] = { "u0", "u1", "u2" };
    for (size_t s = 0; s < sizeof SCRIPT / sizeof SCRIPT[0]; s++) {
        char err[128];
        uint32_t aL = intern_id(sl, SCRIPT[s]), aN = intern_id(sn, SCRIPT[s]);
        CHECK(world_step(L, &aL, 1, err, sizeof err) == 0);
        CHECK(world_step(N, &aN, 1, err, sizeof err) == 0);
        for (int u = 0; u < 3; u++) {
            char b[32];
            snprintf(b, sizeof b, "threat(%s)", UNITS[u]);
            long vL = world_get_num(L, intern_id(sl, b));
            long vN = world_get_num(N, intern_id(sn, b));
            if (vL != vN) {
                fprintf(stderr, "MISMATCH after %s: %s lane=%ld n1=%ld\n",
                        SCRIPT[s], b, vL, vN);
                return 1;
            }
        }
    }
    /* and the values are the interesting ones, not degenerate */
    CHECK(world_get_num(L, intern_id(sl, "threat(u1)")) == 7);   /* calmed: scary only */
    intern_free(sl); intern_free(sn);
    world_free(L); world_free(N);
    return 0;
}

/* ---- misuse ---- */

static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "state x : int merge avg\n",
          "expected `min` or `max` after `merge`" },
        { "value v : int merge max\nrule d: => v = 3\n",
          "no merge algebra" },
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
    if (test_engine_max()) return 1;
    if (test_engine_min_order_idem()) return 1;
    if (test_story_floor()) return 1;
    if (test_register_still_contests()) return 1;
    if (test_lane_vs_n1()) return 1;
    if (test_errors()) return 1;
    printf("test_merge: all passed\n");
    return 0;
}
