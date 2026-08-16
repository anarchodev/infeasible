/* Pin for the judgment family's OBSERVABLE query contract (#77) — written
 * against the dense jfam and kept passing verbatim across the sparse-location
 * switch. Two halves:
 *
 *  (a) host-API world: one atom per "class" the fact-load distinguishes —
 *      fluents in/out of rules, guard/provider/expr-guard atoms that are
 *      judgment-referenced / step-only / registered-but-unreferenced, primed
 *      and action atoms, superiority, and never-declared atoms — each asserted
 *      to its EXACT verdict, both polarities. The PROVED-only sweeps in
 *      test_matcher/test_ticktime cannot see the REFUTED-vs-UNDECIDED line
 *      this pins (a sparse family that dropped an unreferenced fluent would
 *      silently flip it to UNDECIDED).
 *
 *  (b) story-based, eager AND tick-time matcher: for every interned atom,
 *      both polarities, query -> why (which may materialize the atom into the
 *      family) -> query again must agree — the lazy answer and the
 *      materialized answer are one contract. Re-checked across a world_step
 *      (a matcher re-ground). */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } \
    } while (0)

/* provider truth by args: sees(a,b) and sees(a,c) hold, sees(b,c) does not */
static uint32_t ent_a, ent_b, ent_c;
static bool sees_cb(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)ctx; (void)pred;
    if (nargs < 2 || args[0] != ent_a) return false;
    return args[1] == ent_b || args[1] == ent_c;
}

/* world_why into the void, so a why-triggered materialization can be forced
 * without caring about the text. */
static void why_void(world *w, dl_lit q)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    world_why(w, q, f);
    fclose(f);
    free(buf);
}

/* For every interned atom, both polarities: query -> why -> query must agree. */
static int consistency_diffs(world *w, intern *sy)
{
    int diffs = 0;
    uint32_t n = intern_count(sy);
    for (uint32_t id = 1; id < n; id++) {
        for (int neg = 0; neg < 2; neg++) {
            dl_lit q = neg ? dl_neg(id) : dl_pos(id);
            dl_verdict v1 = world_query(w, q);
            why_void(w, q);
            dl_verdict v2 = world_query(w, q);
            if (v1 != v2) {
                fprintf(stderr, "query/why/query differ: %s%s  %d -> %d\n",
                        neg ? "~" : "", intern_name(sy, id), v1, v2);
                diffs++;
            }
        }
    }
    return diffs;
}

/* exact verdicts, both polarities */
static int check_lit(world *w, intern *sy, const char *name,
                     dl_verdict pos, dl_verdict neg)
{
    uint32_t id = intern_id(sy, name);
    if (world_query(w, dl_pos(id)) != pos) {
        fprintf(stderr, "verdict %s: got %d want %d\n", name,
                world_query(w, dl_pos(id)), pos);
        return 0;
    }
    if (world_query(w, dl_neg(id)) != neg) {
        fprintf(stderr, "verdict ~%s: got %d want %d\n", name,
                world_query(w, dl_neg(id)), neg);
        return 0;
    }
    return 1;
}

static int host_api_half(void)
{
    intern *sy = intern_new();
    world *w = world_new(sy);
    ent_a = intern_id(sy, "a"); ent_b = intern_id(sy, "b"); ent_c = intern_id(sy, "c");

    /* fluents: p feeds rules; u_f/u_t referenced by NO rule; tgl only a step effect */
    uint32_t p   = intern_id(sy, "p");
    uint32_t p2  = intern_id(sy, "p2");
    uint32_t u_f = intern_id(sy, "u_f");
    uint32_t u_t = intern_id(sy, "u_t");
    uint32_t tgl = intern_id(sy, "tgl");
    world_declare_fluent(w, p);   world_set(w, p, true);
    world_declare_fluent(w, p2);  world_set(w, p2, true);
    world_declare_fluent(w, u_f);                         /* stays false */
    world_declare_fluent(w, u_t); world_set(w, u_t, true);
    world_declare_fluent(w, tgl);                         /* stays false until the step */

    /* numeric + guards: gj judgment-referenced, gu registered-only, gs step-only */
    uint32_t hp = intern_id(sy, "hp");
    world_declare_num(w, hp, 0, 10, true);
    world_set_num(w, hp, 3);
    uint32_t gj = intern_id(sy, "hp<=5");
    uint32_t gu = intern_id(sy, "hp<=2");
    uint32_t gs = intern_id(sy, "hp>=1");
    world_add_guard(w, gj, hp, WORLD_CMP_LE, 5);          /* holds: 3 <= 5 */
    world_add_guard(w, gu, hp, WORLD_CMP_LE, 2);          /* would fail; unreferenced */
    world_add_guard(w, gs, hp, WORLD_CMP_GE, 1);          /* holds: 3 >= 1 */

    /* providers: pj judgment-referenced, pu registered-only, ps step-only */
    uint32_t sees = intern_id(sy, "sees");
    uint32_t pj = intern_id(sy, "sees(a,b)");
    uint32_t pu = intern_id(sy, "sees(b,c)");
    uint32_t ps = intern_id(sy, "sees(a,c)");
    uint32_t ab[2] = { ent_a, ent_b }, bc[2] = { ent_b, ent_c }, ac[2] = { ent_a, ent_c };
    world_declare_provider_atom(w, pj, sees, ab, 2);      /* holds via callback */
    world_declare_provider_atom(w, pu, sees, bc, 2);      /* callback false; unreferenced */
    world_declare_provider_atom(w, ps, sees, ac, 2);      /* holds via callback */
    world_set_provider_fn(w, sees_cb, NULL);

    /* expr guards: ej judgment-referenced (7>=5), eu registered-only, es step-only (2<=5) */
    uint32_t ej = intern_id(sy, "e:7>=5");
    uint32_t eu = intern_id(sy, "e:1>=5");
    uint32_t es = intern_id(sy, "e:2<=5");
    expr_ins k7 = { EXPR_CONST, 7 }, k5 = { EXPR_CONST, 5 };
    expr_ins k1 = { EXPR_CONST, 1 }, k2 = { EXPR_CONST, 2 };
    world_add_expr_guard(w, ej, &k7, 1, &k5, 1, WORLD_CMP_GE);
    world_add_expr_guard(w, eu, &k1, 1, &k5, 1, WORLD_CMP_GE);
    world_add_expr_guard(w, es, &k2, 1, &k5, 1, WORLD_CMP_LE);

    /* judgment rules + a superiority pair */
    uint32_t q_  = intern_id(sy, "q");
    uint32_t s_  = intern_id(sy, "s");
    uint32_t low = intern_id(sy, "low");
    uint32_t seen = intern_id(sy, "seen");
    uint32_t big = intern_id(sy, "big");
    dl_lit b;
    b = dl_pos(p);
    world_add_rule(w, "r1", DL_DEFEASIBLE, dl_pos(q_), &b, 1);
    int r2 = world_add_rule(w, "r2", DL_DEFEASIBLE, dl_pos(s_), &b, 1);
    b = dl_pos(p2);
    int r3 = world_add_rule(w, "r3", DL_DEFEASIBLE, dl_neg(s_), &b, 1);
    world_add_sup(w, r2, r3);                             /* r2 beats r3: s wins */
    b = dl_pos(gj);
    world_add_rule(w, "r4", DL_DEFEASIBLE, dl_pos(low), &b, 1);
    b = dl_pos(pj);
    world_add_rule(w, "r5", DL_DEFEASIBLE, dl_pos(seen), &b, 1);
    b = dl_pos(ej);
    world_add_rule(w, "r6", DL_DEFEASIBLE, dl_pos(big), &b, 1);

    /* a step rule so action / primed / step-only-referenced atoms exist */
    uint32_t act = intern_id(sy, "act");
    step_cond body[4] = {
        { { p,  false }, false }, { { gs, false }, false },
        { { ps, false }, false }, { { es, false }, false },
    };
    dl_lit eff = dl_pos(tgl);
    world_add_step_rule(w, "do", act, body, 4, &eff, 1);

    uint32_t ghost = intern_id(sy, "ghost");              /* interned, never declared */

    /* ---- the exact contract, class by class ---- */
    CHECK(check_lit(w, sy, "q",    DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "s",    DL_PROVED,    DL_REFUTED));   /* superiority decides */
    CHECK(check_lit(w, sy, "low",  DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "seen", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "big",  DL_PROVED,    DL_REFUTED));
    /* closed-world fluents, referenced or not */
    CHECK(check_lit(w, sy, "p",    DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "u_f",  DL_REFUTED,   DL_PROVED));    /* NOT undecided */
    CHECK(check_lit(w, sy, "u_t",  DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "tgl",  DL_REFUTED,   DL_PROVED));
    /* guard atoms: judgment-referenced / step-only load; registered-only has no
     * location anywhere, so it stays undecided (the landmark carve-out) */
    CHECK(check_lit(w, sy, "hp<=5", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "hp>=1", DL_PROVED,    DL_REFUTED));  /* step-only, still loaded */
    CHECK(check_lit(w, sy, "hp<=2", DL_UNDECIDED, DL_UNDECIDED));
    /* provider atoms, same three-way split */
    CHECK(check_lit(w, sy, "sees(a,b)", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "sees(a,c)", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "sees(b,c)", DL_UNDECIDED, DL_UNDECIDED));
    /* expr-guard atoms, same three-way split */
    CHECK(check_lit(w, sy, "e:7>=5", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "e:2<=5", DL_PROVED,    DL_REFUTED));
    CHECK(check_lit(w, sy, "e:1>=5", DL_UNDECIDED, DL_UNDECIDED));
    /* located but rule-less and fact-less in the judgment family: refuted, both
     * polarities (an empty rule set discards every proof attempt) */
    CHECK(check_lit(w, sy, "act", DL_REFUTED, DL_REFUTED));
    CHECK(check_lit(w, sy, "p'",  DL_REFUTED, DL_REFUTED));
    /* interned but never declared/mentioned: not in the theory at all */
    CHECK(check_lit(w, sy, "ghost", DL_UNDECIDED, DL_UNDECIDED));

    /* lazy answer == materialized answer, for everything */
    CHECK(consistency_diffs(w, sy) == 0);

    /* across a step (state edit + solve invalidation): the contract holds on the
     * new state, and tgl's closed-world verdict tracks its new value */
    char err[64];
    CHECK(world_step(w, &act, 1, err, sizeof err) == 0);
    CHECK(world_get(w, tgl) == true);
    CHECK(check_lit(w, sy, "tgl", DL_PROVED,  DL_REFUTED));
    CHECK(check_lit(w, sy, "u_f", DL_REFUTED, DL_PROVED));
    CHECK(check_lit(w, sy, "hp>=1", DL_PROVED, DL_REFUTED));
    CHECK(consistency_diffs(w, sy) == 0);

    world_free(w);
    intern_free(sy);
    return 1;
}

/* ---- (b) story-based: eager and tick-time matcher agree with themselves ---- */

static const char *STORY =
    "scene pin\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "host provider sees(actor, actor)\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    "  marked(actor)\n"
    "  hp(actor) : int in 0 .. 10\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  adj(b, c)\n"
    "  awake(a)\n"
    "  awake(b)\n"
    "  hp(a) = 3\n"
    ")\n"
    "rule threat(X: actor, Y: actor): adj(X, Y) & awake(X) => threat(X, Y)\n"
    "rule ready(X: actor): awake(X) & ~marked(X) => ready(X)\n"
    "rule critical(X: actor): awake(X) & hp(X) <= 5 => critical(X)\n"
    "rule spot(X: actor, Y: actor): awake(X) & awake(Y) & sees(X, Y) => spotted(X, Y)\n"
    "action mark(X: actor): requires awake(X) causes marked(X)\n";

static bool story_sees_cb(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)pred;
    const uint32_t *pair = ctx;                 /* sees(a,b) only */
    return nargs >= 2 && args[0] == pair[0] && args[1] == pair[1];
}

static int story_half(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    world *A = story_compile(STORY, "pin.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(STORY, "pin.story", sy, &dgb, &B);
    CHECK(A && M && B);
    CHECK(dga.nerrors == 0 && dgb.nerrors == 0);

    uint32_t pair[2] = { intern_id(sy, "a"), intern_id(sy, "b") };
    world_set_provider_fn(A, story_sees_cb, pair);
    world_set_provider_fn(B, story_sees_cb, pair);

    CHECK(world_query(A, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);

    CHECK(consistency_diffs(A, sy) == 0);
    CHECK(consistency_diffs(B, sy) == 0);

    /* across a step — the matcher re-grounds; both contracts must still hold */
    char err[64];
    uint32_t mark_a = intern_id(sy, "mark(a)");
    CHECK(world_step(A, &mark_a, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &mark_a, 1, err, sizeof err) == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "ready(a)"))) != DL_PROVED);

    CHECK(consistency_diffs(A, sy) == 0);
    CHECK(consistency_diffs(B, sy) == 0);

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    return 1;
}

int main(void)
{
    if (!host_api_half()) return 1;
    if (!story_half()) return 1;
    printf("test_sparsejfam: all passed\n");
    return 0;
}
