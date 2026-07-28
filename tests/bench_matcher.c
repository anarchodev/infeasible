/* Tick-time matcher vs. eager grounding — time AND space across workloads
 * (EPIC #43 / §8.1). The matcher grounds only body-satisfying instances, so its
 * SPACE tracks the DENSITY of the anchor relation, not the sort cross product.
 * Facts are loaded via world_set (as a host does), so we aren't bound by the
 * init-list size. Metrics, eager vs. matcher:
 *   - compile time      (eager materializes N^2 rules; matcher captures 1, defers)
 *   - rules+rows        (SPACE: eager N^2 rules; matcher ~= matches — as view
 *                        ROWS for island preds (#80), ground rules otherwise)
 *   - arena+view bytes   (SPACE: rule storage + the view store)
 *   - per-update time    (change one fact, requery: the judgment-layer refresh)
 *
 * Expected shape: the matcher's SPACE win is decisive when sparse, and with
 * the island/view path (#80) its per-update TIME is the join + view refill —
 * no rule churn, no schema rebuild, no solve for view-answered queries.
 * NOT a ctest; build -O2 (Release) for real numbers.
 *
 *   ./bench_matcher              density sweep + scale sweep
 *   ./bench_matcher <N> <permil> one point (density in per-mille of N^2)
 */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

/* rel(ai,aj) true iff a cheap deterministic hash of (i,j) falls under `permil`. */
static int rel_true(int i, int j, int permil)
{
    unsigned h = (unsigned)(i * 2654435761u + j * 40503u);
    return (int)(h % 1000u) < permil;
}

/* Workload: N actors; a base relation `rel` (loaded via world_set), a `link`
 * rule over it, and a scalar `awake` fluent we toggle to force a judgment
 * refresh without changing the match set. */
static char *gen_story(int N)
{
    size_t cap = (size_t)N * 16u + 4096u;
    char *s = malloc(cap);
    int n = 0;
    #define EMIT(...) do { n += snprintf(s + n, cap - (size_t)n, __VA_ARGS__); } while (0)
    EMIT("scene bench\nsort actor\nentity (\n");
    for (int i = 0; i < N; i++) EMIT("  a%d%s", i, i + 1 < N ? "," : " : actor\n");
    EMIT(")\n");
    EMIT("state ( rel(actor, actor) awake(actor) )\n");
    EMIT("rule link(X: actor, Y: actor): rel(X, Y) => linked(X, Y)\n");
    #undef EMIT
    return s;
}

static long count_rel(int N, int permil)
{
    long t = 0;
    for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) t += rel_true(i, j, permil);
    return t;
}

typedef struct { double compile_ms, upd_us; int rules; long bytes; } result;

/* Set rel facts, build the layer, then time UPDATES: toggle awake(a_k) and
 * requery `linked(fi,fj)` — the judgment-layer refresh (matcher: re-ground +
 * schema rebuild + solve; eager: re-solve the cached schema). */
static result run(const char *src, int matched, intern *sy, int N, int permil,
                  int fi, int fj, int updates)
{
    result r = { 0, 0, 0, 0 };
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };

    double best = 1e18;
    for (int rep = 0; rep < (N <= 120 ? 3 : 1); rep++) {
        intern *tmp = intern_new();
        world *ww = NULL; story_matcher *mm = NULL;
        double t0 = now_ms();
        if (matched) mm = story_compile_matcher(src, "b", tmp, &dg, &ww);
        else         ww = story_compile(src, "b", tmp, &dg);
        double dt = now_ms() - t0;
        if (dt < best) best = dt;
        if (ww) world_free(ww);
        if (mm) story_matcher_free(mm);
        intern_free(tmp);
    }
    r.compile_ms = best;

    world *w = NULL; story_matcher *m = NULL;
    if (matched) m = story_compile_matcher(src, "b", sy, &dg, &w);
    else         w = story_compile(src, "b", sy, &dg);
    if (!w) { fprintf(stderr, "compile FAILED: %s\n", dg.count ? di[0].msg : "?"); exit(1); }

    /* load the relation via world_set (host-style; no init-list cap) */
    char nm[64];
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            if (rel_true(i, j, permil)) {
                snprintf(nm, sizeof nm, "rel(a%d,a%d)", i, j);
                world_set(w, intern_id(sy, nm), true);
            }

    snprintf(nm, sizeof nm, "linked(a%d,a%d)", fi, fj);
    dl_lit q = dl_pos(intern_id(sy, nm));
    (void)world_query(w, q);                 /* build the initial (matched) layer */
    /* SPACE: island matches live as view ROWS, not ground rules (#80), so the
     * matched layer's footprint is rules + view rows and arena + view bytes —
     * counting rules alone would read as an (unreal) infinite space win. */
    r.rules = world_judgment_rule_count(w) + world_view_row_count(w);
    r.bytes = (long)(world_arena_bytes(w) + world_view_bytes(w));

    double t0 = now_ms();
    for (int k = 0; k < updates; k++) {
        snprintf(nm, sizeof nm, "awake(a%d)", k % N);
        world_set(w, intern_id(sy, nm), (k & 1) != 0);   /* toggle -> invalidate */
        (void)world_query(w, q);             /* forces re-ground (matcher) + solve */
    }
    r.upd_us = (now_ms() - t0) * 1000.0 / updates;

    world_free(w);
    if (m) story_matcher_free(m);
    return r;
}

static void bench_one(int N, int permil, int updates)
{
    char *src = gen_story(N);
    long ntrue = count_rel(N, permil);
    int fi = 0, fj = 0;                       /* first true rel pair, for the query */
    for (int i = 0; i < N && !fi && !fj; i++)
        for (int j = 0; j < N; j++)
            if (rel_true(i, j, permil)) { fi = i; fj = j; goto got; }
got:;
    intern *se = intern_new(), *sm = intern_new();
    result E = run(src, 0, se, N, permil, fi, fj, updates);
    result M = run(src, 1, sm, N, permil, fi, fj, updates);
    intern_free(se); intern_free(sm);
    free(src);

    printf("  N=%-4d %5.1f%%  rel=%-7ld | compile %7.1f/%-7.1f ms  "
           "rules %8d/%-8d (%6.1fx)  arena %7ldK/%-7ldK  update %8.1f/%-8.1f us (%.2fx)\n",
           N, permil / 10.0, ntrue,
           E.compile_ms, M.compile_ms,
           E.rules, M.rules, M.rules ? (double)E.rules / M.rules : 0,
           E.bytes / 1024, M.bytes / 1024,
           E.upd_us, M.upd_us, M.upd_us ? E.upd_us / M.upd_us : 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("bench_matcher: eager vs. tick-time matcher (EPIC #43 / §8.1)\n");
    printf("  eager/matcher.  rules ratio >1 => matcher smaller;  update ratio >1 => matcher faster.\n\n");
    if (argc > 2) { bench_one(atoi(argv[1]), atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 100); return 0; }

    printf("density sweep (N=120, N^2=14400 possible link instances):\n");
    int perm[] = { 5, 20, 100, 300, 1000 };   /* 0.5% .. 100% */
    for (size_t i = 0; i < sizeof perm / sizeof *perm; i++) bench_one(120, perm[i], 100);

    printf("\nscale sweep at 2%% density (sparse — the matcher's target):\n");
    int Ns[] = { 40, 80, 160, 240 };
    for (size_t i = 0; i < sizeof Ns / sizeof *Ns; i++) bench_one(Ns[i], 20, 100);

    printf("\nReading: SPACE — the matcher's matched layer (view rows here, since the link\n"
           "rule is an island, #80) tracks the anchor relation's density, converging to eager\n"
           "at 100%%. TIME — a matcher update is the join + view refill; a view-answered query\n"
           "runs no solve, so update time scales with matches, not the fluent universe.\n");
    return 0;
}
