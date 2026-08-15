/* Golden test for binding a variable from a provider run (#257).
 *
 * A generator-capable provider (#254) can bind its free argument instead of
 * testing a pair the join already formed. That turns a spatially-anchored rule
 * from |F|^2 intermediate bindings into |F| x run.
 *
 * It is an OPTIMISATION INSIDE an already-matchable rule, never a widening of
 * what matches: enumerability is a runtime registration and the compile-time
 * eligibility check cannot see it, so a variable reachable only through a
 * provider still leaves the rule eager. What this file has to prove is
 * therefore narrow and total — the plan changed, the answers did not.
 *
 * So every case compares three worlds built from ONE source: eager, matched
 * with the generator withheld, and matched with it registered. All three must
 * agree on every verdict, before and after a state change; only the probe
 * count may differ. A faster plan that answers differently is not an
 * optimisation, and a plan that never engages is not a feature. */

#include "lang/story.h"
#include "state/world.h"
#include "stock/grid.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

enum { N = 60, SIDE = 7 };

/* A rule anchored on a provider, with both variables ALSO fluent-bound — the
 * shape the planner may improve without the eligibility check having to change.
 * `sleepy` gives the join a second, sparser predicate so the plan has a real
 * ordering decision to make rather than a symmetric one. */
static char *source(void)
{
    size_t cap = 1u << 18; char *s = malloc(cap); int o = 0;
    o += snprintf(s + o, cap - o,
        "sort actor\nprovider grid_adjacent(actor, actor)\nentity (");
    for (int i = 0; i < N; i++) o += snprintf(s + o, cap - o, "%su%d", i ? ", " : "", i);
    o += snprintf(s + o, cap - o, " : actor)\n"
        "state ( grid_x(actor) : int in 0..99  grid_y(actor) : int in 0..99\n"
        "        grid_blocks(actor)  awake(actor)  sleepy(actor) )\ninit (\n");
    for (int i = 0; i < N; i++) {
        o += snprintf(s + o, cap - o, "  awake(u%d)", i);
        if (i % 5 == 0) o += snprintf(s + o, cap - o, " sleepy(u%d)", i);
        o += snprintf(s + o, cap - o, " grid_x(u%d)=%d grid_y(u%d)=%d\n",
                      i, i % SIDE, i, i / SIDE);
    }
    o += snprintf(s + o, cap - o, ")\n"
        "rule beside(A: actor, B: actor):\n"
        "    awake(A) & awake(B) & grid_adjacent(A, B) => adjacent_to(A, B)\n"
        "rule dozing(A: actor, B: actor):\n"
        "    sleepy(A) & awake(B) & grid_adjacent(A, B) => watched(A, B)\n");
    return s;
}

typedef struct { world *w; intern *sy; stock_grid *g; story_matcher *m; } built;

static int build(built *b, const char *src, int matched, int with_gen)
{
    b->sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    b->m = NULL;
    if (matched) b->m = story_compile_matcher(src, "j.story", b->sy, &dg, &b->w);
    else         b->w = story_compile(src, "j.story", b->sy, &dg);
    if (!b->w || dg.nerrors) {
        fprintf(stderr, "FAIL compile: %s\n", dg.count ? di[0].msg : "?");
        return 1;
    }
    uint32_t ents[N];
    for (int i = 0; i < N; i++) {
        char nm[16]; snprintf(nm, sizeof nm, "u%d", i);
        ents[i] = intern_id(b->sy, nm);
    }
    b->g = stock_grid_install(b->w, b->sy, ents, N);
    if (!with_gen)      /* the same world, minus the ability to enumerate */
        world_set_provider_gen_fn(b->w, intern_id(b->sy, "grid_adjacent"), NULL, NULL);
    if (b->m) story_matcher_reground(b->m);
    return 0;
}

static void teardown(built *b)
{
    if (b->m) story_matcher_free(b->m);
    stock_grid_free(b->g); world_free(b->w); intern_free(b->sy);
}

/* Every ground instance of both heads, compared across two worlds — on the
 * PROVED set, not on the raw verdict. Eager grounds an inapplicable instance
 * and REFUTES it; the matcher never emits one, so the head stays UNDECIDED.
 * That is the documented provability-contract asymmetry, and it is what the
 * matcher's soundness argument already turns on: an omitted instance is one
 * that concludes nothing. Comparing verdicts would fail on the first
 * self-pair and would be testing the wrong thing. */
static int agree(const char *what, built *a, built *c, int *npos)
{
    static const char *HEADS[] = { "adjacent_to", "watched" };
    *npos = 0;
    for (unsigned h = 0; h < sizeof HEADS / sizeof HEADS[0]; h++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                char q[64];
                snprintf(q, sizeof q, "%s(u%d,u%d)", HEADS[h], i, j);
                dl_verdict va = world_query(a->w, dl_pos(intern_id(a->sy, q)));
                dl_verdict vc = world_query(c->w, dl_pos(intern_id(c->sy, q)));
                if ((va == DL_PROVED) != (vc == DL_PROVED)) {
                    fprintf(stderr, "FAIL %s: %s differs (%d vs %d)\n",
                            what, q, (int)va, (int)vc);
                    return 1;
                }
                if (va == DL_PROVED) (*npos)++;
            }
    return 0;
}

int main(void)
{
    char *src = source();
    built eager, plain, gen;
    if (build(&eager, src, 0, 1)) return 1;
    if (build(&plain, src, 1, 0)) return 1;
    if (build(&gen,   src, 1, 1)) return 1;

    int n1 = 0, n2 = 0;
    if (agree("matched-without-generator vs eager", &eager, &plain, &n1)) return 1;
    if (agree("matched-with-generator vs eager",    &eager, &gen,   &n2)) return 1;
    CHECK(n1 == n2 && n1 > 0);
    printf("  %d proved instances, identical across all three plans\n", n1);

    /* the plan must actually ENGAGE — an optimisation nothing selects is not
     * one, and this is the assertion that would have caught the uninitialised
     * gen_off that made the planner's choice depend on heap contents */
    long p_plain = story_matcher_last_probes(plain.m);
    long p_gen   = story_matcher_last_probes(gen.m);
    CHECK(p_gen < p_plain / 2);
    printf("  probes %ld -> %ld (%.1fx) with the generator engaged\n",
           p_plain, p_gen, (double)p_plain / (double)p_gen);

    /* AND IT MUST FOLLOW STATE. The provider re-reads positions when the tick
     * moves, the matcher re-grounds on a fact edit; a plan cached across either
     * would answer the previous world. */
    for (int i = 0; i < N; i += 3) {
        char b[32]; snprintf(b, sizeof b, "grid_x(u%d)", i);
        built *all[3] = { &eager, &plain, &gen };
        for (int k = 0; k < 3; k++) {
            world_set_num(all[k]->w, intern_id(all[k]->sy, b), 40 + i);
            stock_grid_refresh(all[k]->g);
        }
    }
    story_matcher_reground(plain.m);
    story_matcher_reground(gen.m);
    int m1 = 0, m2 = 0;
    if (agree("after a move: matched-without-generator", &eager, &plain, &m1)) return 1;
    if (agree("after a move: matched-with-generator",    &eager, &gen,   &m2)) return 1;
    CHECK(m1 == m2);
    CHECK(m1 != n1);                       /* the move must have CHANGED something */
    printf("  after scattering a third of them: %d proved, still identical\n", m1);

    teardown(&eager); teardown(&plain); teardown(&gen);
    free(src);
    printf("test_provjoin: all passed\n");
    return 0;
}
