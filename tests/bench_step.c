/* Transition-layer benchmark: world_step on the lanes (routed) vs the N=1 path,
 * end-to-end through the real world_* API — the per-tick cost that decides
 * whether defeasible transitions are "too slow for RTS" (the thesis). Where
 * bench_col times the dl_col solver directly, this times the whole step:
 * closed-world fact load, the fixpoint, contested check, and commit.
 *
 *   ./bench_step [nents]      default sweep: 1000 10000 100000
 *
 * A homogeneous per-unit transition (3 boolean fluents over one sort, 3 actions,
 * one primed-body ramification):
 *   action advance(X): causes moving(X)     action stop(X): causes ~moving(X)
 *   action spot(X):    causes alerted(X)
 *   rule  engages(X):  alerted(X)' & moving(X)' causes engaging(X)
 * plus generated inertia on every fluent. The SAME theory is built two ways
 * against one intern: story_compile (homogeneous -> world_step routes through the
 * step lane family) and the C API (grounded per entity -> the N=1 step family).
 * Results are verified identical before timing.
 *
 * Build time is reported separately from the per-tick solve (the RTS number).
 * Both the world's atom->index maps and the grounder's entity lookups are hashed
 * (direct-indexed on the dense interns), so construction is ~linear and 100000
 * entities build in well under a second. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng_state = 0xBEEFCAFEu;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Build the homogeneous step world's .story source with `n` entities. */
static char *gen_source(int n)
{
    size_t cap = (size_t)n * 12 + 512;
    char *s = malloc(cap);
    int off = snprintf(s, cap, "sort actor\nentity (");
    for (int e = 0; e < n; e++)
        off += snprintf(s + off, cap - off, "%su%d", e ? ", " : "", e);
    off += snprintf(s + off, cap - off,
        " : actor)\n"
        "state (moving(actor) alerted(actor) engaging(actor))\n"
        "action advance(X: actor): causes moving(X)\n"
        "action stop(X: actor): causes ~moving(X)\n"
        "action spot(X: actor): causes alerted(X)\n"
        "rule engages(X: actor): alerted(X)' & moving(X)' causes engaging(X)\n"
        "init (moving(u0))\n");
    return s;
}

/* The same theory, grounded per entity, via the C API (the N=1 world). */
static void build_n1(world *w, intern *sy, int n)
{
    char b[48];
    for (int e = 0; e < n; e++) {
        snprintf(b, sizeof b, "moving(u%d)", e);  uint32_t mv = intern_id(sy, b);
        snprintf(b, sizeof b, "alerted(u%d)", e); uint32_t al = intern_id(sy, b);
        snprintf(b, sizeof b, "engaging(u%d)", e);uint32_t eg = intern_id(sy, b);
        world_declare_fluent(w, mv);
        world_declare_fluent(w, al);
        world_declare_fluent(w, eg);
        dl_lit em = { mv, false }, emn = { mv, true }, ea = { al, false }, ee = { eg, false };
        snprintf(b, sizeof b, "advance(u%d)", e);
        world_add_step_rule(w, "advance", intern_id(sy, b), NULL, 0, &em, 1);
        snprintf(b, sizeof b, "stop(u%d)", e);
        world_add_step_rule(w, "stop", intern_id(sy, b), NULL, 0, &emn, 1);
        snprintf(b, sizeof b, "spot(u%d)", e);
        world_add_step_rule(w, "spot", intern_id(sy, b), NULL, 0, &ea, 1);
        step_cond body[2] = { { { al, false }, true }, { { mv, false }, true } };
        world_add_step_rule(w, "engages", INTERN_NONE, body, 2, &ee, 1);
    }
    world_set(w, intern_id(sy, "moving(u0)"), true);   /* match the story init */
}

static double median_step(world *w, const uint32_t *acts, int nacts, int iters)
{
    double *ms = malloc((size_t)iters * sizeof *ms);
    char err[128];
    for (int i = 0; i < iters; i++) {
        double a = now_ms();
        world_step(w, acts, nacts, err, sizeof err);
        ms[i] = now_ms() - a;
    }
    qsort(ms, (size_t)iters, sizeof *ms, cmp_double);
    double m = ms[iters / 2];
    free(ms);
    return m;
}

static int bench_one(int n)
{
    intern *sy = intern_new();

    /* ---- the routed (lane) world: compile the homogeneous source ---- */
    double t0 = now_ms();
    char *src = gen_source(n);
    story_diags *nd = NULL;
    world *wl = story_compile(src, "bench.story", sy, nd);
    free(src);
    double build_lane = now_ms() - t0;
    if (!wl) { fprintf(stderr, "bench_step: compile failed at N=%d\n", n); return 1; }
    if (world_step_lane_family_count(wl) != 1) {
        fprintf(stderr, "bench_step: N=%d did not lane its transition\n", n);
        return 1;
    }

    /* ---- the N=1 world: same theory, grounded per entity ---- */
    t0 = now_ms();
    world *wn = world_new(sy);
    build_n1(wn, sy, n);
    double build_n1_ms = now_ms() - t0;

    /* ---- one tick's actions: each unit advances/stops/idles, maybe spots ---- */
    uint32_t *acts = malloc((size_t)n * 2 * sizeof *acts);
    int nacts = 0;
    char b[48];
    for (int e = 0; e < n; e++) {
        int mv = xrand() % 3;                       /* 0 advance, 1 stop, 2 idle */
        if (mv == 0)      { snprintf(b, sizeof b, "advance(u%d)", e); acts[nacts++] = intern_id(sy, b); }
        else if (mv == 1) { snprintf(b, sizeof b, "stop(u%d)", e);    acts[nacts++] = intern_id(sy, b); }
        if (xrand() & 1)  { snprintf(b, sizeof b, "spot(u%d)", e);    acts[nacts++] = intern_id(sy, b); }
    }

    /* ---- verify the two paths agree on the transition ---- */
    char err[128];
    if (world_step(wl, acts, nacts, err, sizeof err) != 0 ||
        world_step(wn, acts, nacts, err, sizeof err) != 0) {
        fprintf(stderr, "bench_step: a step was contested at N=%d (%s)\n", n, err);
        return 1;
    }
    int stride = n <= 10000 ? 1 : 101, bad = 0;
    for (int e = 0; e < n && bad < 5; e += stride) {
        const char *fl[3] = { "moving", "alerted", "engaging" };
        for (int k = 0; k < 3; k++) {
            snprintf(b, sizeof b, "%s(u%d)", fl[k], e);
            uint32_t a = intern_id(sy, b);
            if (world_get(wl, a) != world_get(wn, a)) {
                fprintf(stderr, "  MISMATCH %s: lane=%d n1=%d\n",
                        b, world_get(wl, a), world_get(wn, a));
                bad++;
            }
        }
    }
    if (bad) { fprintf(stderr, "bench_step: verification FAILED at N=%d\n", n); return 1; }

    /* ---- time both per-tick paths ---- */
    int n1_iters = n <= 1000 ? 50 : n <= 10000 ? 15 : 5, lane_iters = 200;
    double lane_med = median_step(wl, acts, nacts, lane_iters);
    double n1_med   = median_step(wn, acts, nacts, n1_iters);

    printf("N=%-6d  lane %8.4f ms   N=1 %9.3f ms   speedup %5.0fx   "
           "(build: lane %.0f ms, n1 %.0f ms)\n",
           n, lane_med, n1_med, n1_med / lane_med, build_lane, build_n1_ms);
    printf("          per-tick budget: lane = %.3f%% of a 60 Hz frame, "
           "%.4f%% of a 10 Hz sim tick;  N=1 = %.0f%% / %.1f%%\n",
           100.0 * lane_med / 16.67, 100.0 * lane_med / 100.0,
           100.0 * n1_med / 16.67, 100.0 * n1_med / 100.0);

    free(acts);
    world_free(wl);
    world_free(wn);
    intern_free(sy);
    return 0;
}

int main(int argc, char **argv)
{
    printf("bench_step: per-unit transition — 3 fluents, 3 actions, 1 ramification, "
           "generated inertia; world_step lanes vs N=1\n");
    if (argc > 1)
        return bench_one(atoi(argv[1]));
    static const int sweep[] = { 1000, 10000, 100000 };
    for (size_t i = 0; i < sizeof sweep / sizeof sweep[0]; i++)
        if (bench_one(sweep[i]))
            return 1;
    return 0;
}
