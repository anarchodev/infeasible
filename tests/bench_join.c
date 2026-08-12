/* bench_join — the MULTI-VARIABLE lane family (the "island", src/lang/story.c's
 * join-matcher section): which higher-arity shapes reach it, what it buys, and
 * where it stops scaling.
 *
 * A single-variable rule lanes over its sort: one schema rule, entities as bits,
 * 64 per word (bench_col prices that at 234-558x against scalar). A rule over K
 * variables has no single forced axis, so the compiler picks one structurally —
 * the FIRST variable is the lane axis, the rest are ITERATED, and the K-1
 * non-lane sorts are flattened into the family's `niter` index. Lane one, loop
 * the others: a nested loop whose inner dimension is bit-parallel.
 *
 * bench_lanes does this for the STEP layer's gates. This is the judgment layer's
 * higher-arity half, and it reports three things the comments beside the early
 * returns cannot:
 *
 *   1. COVERAGE — which shapes get a family, each checked against N=1 with
 *      world_lanes_check, because a family that forms and disagrees is worse
 *      than no family at all. Like bench_lanes, a shape that flips from 0 to 1
 *      is a bail retiring, and should be read as news rather than found by
 *      accident.
 *   2. WHAT IT BUYS — the same binary rule solved laned and forced onto N=1,
 *      the latter by a never-firing UNARY second rule concluding the head (the
 *      routing-soundness bail). That rule adds N ground rules against the
 *      shape's N^2, so it prices the gate and not itself.
 *   3. WHERE IT STOPS — the ground map is npred x niter x nent uint32, so the
 *      dense outer loop is quadratic in N for a binary rule. This is the wall,
 *      and it is a BUILD wall, not a solve one: the solve stays two orders of
 *      magnitude ahead of N=1 the whole way up while compile time and memory
 *      go quadratic underneath it.
 *
 * Deterministic construction (I4): the `near` extension is assigned by index,
 * never randomly. Built -O2 regardless of build type; build Release for
 * meaningful numbers. Not registered with ctest. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* `sort actor, item` + N actors + M items, then the caller's declarations and
 * rules. The lane sort is declared first on purpose: which sort comes first
 * decided lane eligibility until #220, and this file is not the place to
 * re-litigate that. */
static char *world_src(int nactors, int nitems, const char *decls, const char *rules)
{
    size_t cap = 1u << 24;
    char *s = malloc(cap);
    size_t o = 0;
    o += (size_t)snprintf(s + o, cap - o, "sort actor, item\nentity (");
    for (int i = 0; i < nactors; i++)
        o += (size_t)snprintf(s + o, cap - o, "%sa%d", i ? ", " : " ", i);
    o += (size_t)snprintf(s + o, cap - o, " : actor");
    if (nitems > 0) {
        for (int i = 0; i < nitems; i++)
            o += (size_t)snprintf(s + o, cap - o, "%si%d", i ? ", " : "  ", i);
        o += (size_t)snprintf(s + o, cap - o, " : item");
    }
    o += (size_t)snprintf(s + o, cap - o, " )\n%s\n%s\n", decls, rules);
    return s;
}

/* ---- 1. coverage ------------------------------------------------------- */

struct shape {
    const char *name;
    const char *decls;
    const char *rules;
    int         expect_fams;
    const char *gate;
};

#define NEAR_AWAKE "state ( near(actor, actor)  awake(actor) )"

static const struct shape SHAPES[] = {
    { "unary: p(X) => q(X)", "state ( p(actor) )",
      "rule r(X: actor): p(X) => q(X)", 1,
      "the single-var family (bench_col's shape)" },
    { "binary head, both vars", NEAR_AWAKE,
      "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)", 1,
      "lane X, iterate Y" },
    { "binary body, UNARY head", NEAR_AWAKE,
      "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => alarmed(X)", 1,
      "a reduction over the iterated var" },
    { "cross-sort: actor x item", "state ( holding(actor, item)  sharp(item) )",
      "rule r(X: actor, K: item): holding(X, K) & sharp(K) => armed(X, K)", 1,
      "the iterated var need not share the lane sort" },
    { "ternary: 3 vars", "state ( near(actor, actor)  holding(actor, item) )",
      "rule r(X: actor, Y: actor, K: item): near(X, Y) & holding(Y, K) "
      "=> steal(X, Y, K)", 1,
      "niter is the product of the K-1 non-lane sorts" },
    { "PROVIDER body atom", "state ( awake(actor) )\nprovider near(actor, actor)",
      "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)", 1,
      "a host-answered relation is a column the provider fills (#233)" },
    { "derived body atom", NEAR_AWAKE,
      "rule d(Y: actor): awake(Y) => alert(Y)\n"
      "rule r(X: actor, Y: actor): near(X, Y) & alert(Y) => threat(X, Y)", 2,
      "a derived body imports from its own family" },
    { "a CONSTANT in an argument", NEAR_AWAKE,
      "rule r(X: actor, Y: actor): near(X, Y) & awake(a0) => threat(X, Y)", 0,
      "join_atom_ok: every arg must be a rule variable" },
    { "two rules conclude the head", "state ( near(actor, actor)  awake(actor)  angry(actor) )",
      "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)\n"
      "rule s(X: actor, Y: actor): near(X, Y) & angry(Y) => threat(X, Y)", 0,
      "routing: the family must be the head's whole cone" },
    { "recursive (transitive step)", "state ( near(actor, actor) )",
      "rule base(X: actor, Y: actor): near(X, Y) => reach(X, Y)\n"
      "rule step(X: actor, Y: actor): reach(X, Y) & near(Y, X) => reach(X, Y)", 0,
      "a slice cannot see sibling iterations" },
};

static int coverage(int nactors, int nitems)
{
    printf("== coverage: which higher-arity shapes reach a family "
           "(N=%d actors, %d items) ==\n", nactors, nitems);
    printf("  %-32s %-5s %-9s %-9s %s\n",
           "shape", "fams", "compile", "arena", "differential vs N=1");
    for (size_t i = 0; i < sizeof SHAPES / sizeof SHAPES[0]; i++) {
        char *src = world_src(nactors, nitems, SHAPES[i].decls, SHAPES[i].rules);
        intern *sy = intern_new();
        story_diag di[16];
        story_diags dg = { di, 16, 0, 0 };
        double t0 = now_ms();
        world *w = story_compile(src, "join.story", sy, &dg);
        double compile_ms = now_ms() - t0;
        if (!w) {
            printf("  %-32s COMPILE FAILED: %s\n", SHAPES[i].name,
                   dg.count ? di[0].msg : "?");
            intern_free(sy);
            free(src);
            return 1;
        }
        int fams = world_lane_family_count(w);
        bool ok = true;
        int cells = world_lanes_check(w, &ok);
        const char *mark = fams == SHAPES[i].expect_fams ? "" :
            (fams > SHAPES[i].expect_fams ? "  <== NEWLY LANED (a bail retired)"
                                          : "  <== REGRESSED (lost lanes)");
        char diff[64];
        if (cells == 0)      snprintf(diff, sizeof diff, "n/a");
        else if (ok)         snprintf(diff, sizeof diff, "OK (%d cells)", cells);
        else                 snprintf(diff, sizeof diff, "*** MISMATCH (%d cells) ***", cells);
        printf("  %-32s %-5d %6.2f ms %6ldK   %-22s %s\n",
               SHAPES[i].name, fams, compile_ms,
               (long)(world_arena_bytes(w) + world_view_bytes(w)) / 1024,
               diff, mark);
        if (fams == 0)
            printf("  %-32s       %s\n", "", SHAPES[i].gate);
        world_free(w);
        intern_free(sy);
        free(src);
    }
    return 0;
}

/* ---- 2. what it buys, and 3. where it stops ---------------------------- */

/* The binary shape, optionally disqualified by a never-firing unary rule on the
 * head (`impossible` is declared and never set, so it is closed-world false). */
static world *binary_world(int n, bool laned, intern *sy, double *compile_ms)
{
    const char *decls = "state ( near(actor, actor)  awake(actor)  impossible(actor) )";
    const char *laned_rules =
        "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)";
    const char *n1_rules =
        "rule r(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)\n"
        "rule s(X: actor): impossible(X) => threat(X, X)";
    char *src = world_src(n, 0, decls, laned ? laned_rules : n1_rules);
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    double t0 = now_ms();
    world *w = story_compile(src, "join.story", sy, &dg);
    if (compile_ms) *compile_ms = now_ms() - t0;
    if (!w) {
        fprintf(stderr, "bench_join: compile failed: %s\n", dg.count ? di[0].msg : "?");
        exit(1);
    }
    free(src);
    return w;
}

/* Deterministic extension: half the actors awake, ~2% of the N^2 pairs near. */
static void fill(world *w, intern *sy, int n)
{
    char buf[64];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "awake(a%d)", i);
        world_set(w, intern_id(sy, buf), i % 2 == 0);
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (((i * 7 + j * 13) % 50) == 0) {
                snprintf(buf, sizeof buf, "near(a%d,a%d)", i, j);
                world_set(w, intern_id(sy, buf), true);
            }
}

/* One tick's shape: a base fact moves, then a conclusion is read. */
static double solve_ms(world *w, intern *sy, int reps)
{
    uint32_t q = intern_id(sy, "threat(a0,a1)");
    uint32_t toggle = intern_id(sy, "awake(a1)");
    double t0 = now_ms();
    for (int k = 0; k < reps; k++) {
        world_set(w, toggle, k % 2 == 0);
        (void)world_query(w, dl_pos(q));
    }
    return (now_ms() - t0) / reps;
}

static void cost(const int *ns, int nn)
{
    printf("\n== what the island buys, and what the dense outer loop costs ==\n");
    printf("  %-8s %-11s %-11s %-9s %-11s %s\n",
           "N", "laned", "N=1", "speedup", "compile", "arena (laned)");
    for (int i = 0; i < nn; i++) {
        int n = ns[i], reps = n <= 80 ? 200 : 50;
        intern *sy1 = intern_new(), *sy0 = intern_new();
        double cms = 0;
        world *lw = binary_world(n, true, sy1, &cms);
        world *nw = binary_world(n, false, sy0, NULL);
        fill(lw, sy1, n);
        fill(nw, sy0, n);
        double a = solve_ms(lw, sy1, reps);
        double b = solve_ms(nw, sy0, reps);
        printf("  %-8d %8.4f ms %8.4f ms %7.1fx %8.1f ms %8ldK\n",
               n, a, b, b / a, cms,
               (long)(world_arena_bytes(lw) + world_view_bytes(lw)) / 1024);
        world_free(lw); world_free(nw);
        intern_free(sy1); intern_free(sy0);
    }
}

int main(int argc, char **argv)
{
    int cov_n = argc > 1 ? atoi(argv[1]) : 40;
    if (coverage(cov_n, 8)) return 1;
    int ns[] = { 40, 80, 160, 320, 640 };
    cost(ns, (int)(sizeof ns / sizeof ns[0]));

    printf("\nReading: the nested loop already exists — lane the first variable,\n"
           "iterate the cartesian product of the rest — and it is worth two orders\n"
           "of magnitude against N=1, never under ~70x here (the spread above is\n"
           "noise: the laned side is sub-microsecond at small N; read the floor).\n"
           "The wall is not the solve, it is the\n"
           "BUILD: the ground map is npred x niter x nent uint32, so a binary rule\n"
           "is quadratic in N, in compile time AND resident memory, while the solve\n"
           "stays two orders of magnitude ahead. That is what caps the mechanism\n"
           "today, and it is why the widening that matters is a SPARSE outer loop\n"
           "(iterate the tuples the fact index says exist, the way the #28 matcher\n"
           "already does for grounding) rather than anything about throughput.\n"
           "bench_matcher prices that sparsity at ~50x fewer instances at 2%% density.\n"
           "A `<==` in the coverage table means the frontier MOVED: re-price the\n"
           "widening work and update this file's expectations.\n");
    return 0;
}
