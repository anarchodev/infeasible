/* Difference bounds as a GENERATOR — is the fact space sparse and separable?
 *
 * `x(A) - x(B) <= r & x(B) - x(A) <= r & …` already compiles (a #130 expression
 * guard between two fluent reads on two vars) and already grounds the sort
 * CROSS PRODUCT: the bound is carried as a filter the solver evaluates after
 * every pair has been materialised. §5.2's matcher grounds only body-satisfying
 * instances, but a var bound only by such a guard is not generator-bound, so the
 * rule falls back to eager — `story.c`'s "generator-provider case, deferred".
 *
 * This prices the alternative before building it. A difference bound over an
 * ORDERED axis is a band join: sort by the axis, sweep, and only pairs inside
 * the band are ever formed. The measurements:
 *
 *   eager     what the engine does today: ground rules + arena bytes + compile
 *   sweep     what a difference-bound generator would form: PROBES (intermediate
 *             pairs examined) and RESULTS (pairs that satisfy every bound)
 *   check     the sweep's pair set must equal the exhaustive N^2 filter's, or
 *             the generator is not the same rule (the test_matcher equivalence:
 *             an omitted instance must be one that concludes nothing)
 *
 * The dimensionality sweep is the point. If probes track the SORT AXIS alone
 * while results fall with each added dimension, then higher-dimensional state is
 * cheaper, not dearer — which is the claim that decides whether this belongs in
 * the language. The `flat` row is the counter-case: an axis with no spread makes
 * the sweep degenerate to the cross product, so separability is a property of
 * the DATA, not of the query.
 *
 * NOT a ctest; build Release for meaningful numbers.
 *
 *   ./bench_dbound            the full sweep
 *   ./bench_dbound <N>        one scale point
 */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { RADIUS = 1 };            /* the band half-width, in cells */

static double ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1e3 + t.tv_nsec/1e6; }

static uint32_t rng = 12345;
static uint32_t xr(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
                                     /* high bits: an LCG's low bits are
                                      * badly correlated, and `% side`
                                      * takes exactly those */

/* Positions at a fixed density (~1.2 entities per cell, bench_slice's spawn
 * density) so the neighbour count per entity is constant across N and only the
 * POPULATION grows — otherwise a scale sweep silently changes two variables. */
typedef struct { int x, y, z; } pos;

static void place(pos *p, int n, int ndim, bool flat)
{
    int side = 1;
    while ((double)side * side < n / 1.2) side++;
    for (int i = 0; i < n; i++) {
        p[i].x = flat ? 0 : (int)(xr() % (uint32_t)side);
        p[i].y = ndim >= 2 ? (int)(xr() % (uint32_t)side) : 0;
        p[i].z = ndim >= 3 ? (int)(xr() % (uint32_t)side) : 0;
    }
}

static bool within(const pos *a, const pos *b, int ndim)
{
    int dx = a->x - b->x; if (dx < -RADIUS || dx > RADIUS) return false;
    if (ndim >= 2) { int dy = a->y - b->y; if (dy < -RADIUS || dy > RADIUS) return false; }
    if (ndim >= 3) { int dz = a->z - b->z; if (dz < -RADIUS || dz > RADIUS) return false; }
    return true;
}

/* ---- the band sweep: sort on one axis, walk the window, filter the rest ---- */

typedef struct { int key, idx; } slot;
static int slot_cmp(const void *a, const void *b)
{
    const slot *p = a, *q = b;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->idx < q->idx ? -1 : (p->idx > q->idx);   /* ties by id: I4 */
}

/* Returns the number of satisfying ordered pairs; *probes gets the intermediate
 * pairs the sweep actually formed (what the join would walk). */
static long sweep(const pos *p, int n, int ndim, long *probes, uint8_t *hit)
{
    slot *s = malloc((size_t)n * sizeof *s);
    for (int i = 0; i < n; i++) { s[i].key = p[i].x; s[i].idx = i; }
    qsort(s, (size_t)n, sizeof *s, slot_cmp);
    long res = 0, pr = 0;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n && s[j].key - s[i].key <= RADIUS; j++) {
            pr++;                                     /* an intermediate binding */
            int a = s[i].idx, b = s[j].idx;
            if (!within(&p[a], &p[b], ndim)) continue;
            res += 2;                                 /* the rule is ordered: A,B and B,A */
            if (hit) { hit[(size_t)a * n + b] = 1; hit[(size_t)b * n + a] = 1; }
        }
    }
    free(s);
    *probes = pr;
    return res;
}


/* ---- indexing EVERY bound axis: bucket, then compare only neighbours -------
 * The sweep above files entities by x alone, so it compares everything in the
 * same column strip however far apart in y. Filing by every bound axis puts
 * non-matching pairs in different buckets, so they are never formed. This is
 * the same answer by the same predicate — only the search changes. */
static long bucketed(const pos *p, int n, int ndim, long *probes)
{
    int side = 1;
    while ((double)side * side < n / 1.2) side++;
    int W = side + 2, H = ndim >= 2 ? side + 2 : 1, D = ndim >= 3 ? side + 2 : 1;
    size_t nb = (size_t)W * H * D;
    int *head = malloc(nb * sizeof *head), *next = malloc((size_t)n * sizeof *next);
    for (size_t i = 0; i < nb; i++) head[i] = -1;
    for (int i = 0; i < n; i++) {
        size_t b = ((size_t)(p[i].x + 1) * H + (ndim >= 2 ? p[i].y + 1 : 0)) * D
                 + (ndim >= 3 ? p[i].z + 1 : 0);
        next[i] = head[b]; head[b] = i;
    }
    long res = 0, pr = 0;
    for (int i = 0; i < n; i++) {
        for (int dx = -RADIUS; dx <= RADIUS; dx++)
        for (int dy = ndim >= 2 ? -RADIUS : 0; dy <= (ndim >= 2 ? RADIUS : 0); dy++)
        for (int dz = ndim >= 3 ? -RADIUS : 0; dz <= (ndim >= 3 ? RADIUS : 0); dz++) {
            size_t b = ((size_t)(p[i].x + 1 + dx) * H + (ndim >= 2 ? p[i].y + 1 + dy : 0)) * D
                     + (ndim >= 3 ? p[i].z + 1 + dz : 0);
            if (b >= nb) continue;
            for (int j = head[b]; j >= 0; j = next[j]) {
                if (j <= i) continue;                 /* each unordered pair once */
                pr++;
                if (within(&p[i], &p[j], ndim)) res += 2;
            }
        }
    }
    free(head); free(next);
    *probes = pr;
    return res;
}

/* ---- eager: what the engine grounds today ---------------------------------- */

static void eager(int n, int ndim, const pos *p, int *rules, double *mb, double *comp)
{
    size_t cap = 1u << 26; char *s = malloc(cap); int o = 0;
    o += snprintf(s+o, cap-o, "sort actor\nentity (");
    for (int i = 0; i < n; i++) o += snprintf(s+o, cap-o, "%sg%d", i?", ":"", i);
    o += snprintf(s+o, cap-o, " : actor)\nstate (\n");
    o += snprintf(s+o, cap-o, "    x(actor) : int in 0..100000\n");
    if (ndim >= 2) o += snprintf(s+o, cap-o, "    y(actor) : int in 0..100000\n");
    if (ndim >= 3) o += snprintf(s+o, cap-o, "    z(actor) : int in 0..100000\n");
    o += snprintf(s+o, cap-o, "    alive(actor)\n)\ninit (\n");
    for (int i = 0; i < n; i++) {
        o += snprintf(s+o, cap-o, "    alive(g%d) x(g%d)=%d", i, i, p[i].x);
        if (ndim >= 2) o += snprintf(s+o, cap-o, " y(g%d)=%d", i, p[i].y);
        if (ndim >= 3) o += snprintf(s+o, cap-o, " z(g%d)=%d", i, p[i].z);
        o += snprintf(s+o, cap-o, "\n");
    }
    o += snprintf(s+o, cap-o, ")\nrule adj(A: actor, B: actor):\n"
                              "    x(A) - x(B) <= %d & x(B) - x(A) <= %d", RADIUS, RADIUS);
    if (ndim >= 2) o += snprintf(s+o, cap-o, "\n  & y(A) - y(B) <= %d & y(B) - y(A) <= %d", RADIUS, RADIUS);
    if (ndim >= 3) o += snprintf(s+o, cap-o, "\n  & z(A) - z(B) <= %d & z(B) - z(A) <= %d", RADIUS, RADIUS);
    o += snprintf(s+o, cap-o, "\n    => adjacent(A, B)\n");

    intern *sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    double t0 = ms();
    world *w = story_compile(s, "d", sy, &dg);
    *comp = ms() - t0;
    if (!w) { *rules = -1; *mb = 0; }
    else { *rules = world_judgment_rule_count(w); *mb = world_arena_bytes(w)/1048576.0; }
    if (w) world_free(w);
    intern_free(sy); free(s);
}

/* ---- equivalence: the sweep must find exactly what an N^2 filter finds ------ */

static int check(const pos *p, int n, int ndim)
{
    uint8_t *hit = calloc((size_t)n * n, 1);
    long probes = 0;
    sweep(p, n, ndim, &probes, hit);
    int bad = 0;
    for (int a = 0; a < n && bad == 0; a++)
        for (int b = 0; b < n; b++) {
            bool want = (a != b) && within(&p[a], &p[b], ndim);
            bool got  = hit[(size_t)a * n + b] != 0;
            if (want != got) {
                fprintf(stderr, "MISMATCH at (%d,%d): brute=%d sweep=%d\n", a, b, want, got);
                bad = 1; break;
            }
        }
    free(hit);
    return bad;
}

int main(int argc, char **argv)
{
    printf("difference bounds as a generator (radius %d, ~1.2 entities/cell)\n\n", RADIUS);

    /* equivalence first: a faster wrong answer is not interesting */
    printf("== equivalence vs an exhaustive N^2 filter\n");
    for (int ndim = 1; ndim <= 3; ndim++) {
        pos *p = malloc(400 * sizeof *p);
        place(p, 400, ndim, false);
        printf("   %dD  %s\n", ndim, check(p, 400, ndim) ? "MISMATCH" : "identical pair set");
        free(p);
    }

    printf("\n== eager grounding vs the sweep (2D)\n");
    printf("     N |  eager rules   arena   compile |    probes    results  sweep\n");
    printf("-------+--------------------------------+----------------------------\n");
    int NS[] = { 128, 256, 512, 1024, 2048, 4096, 16384, 65536 };
    for (unsigned k = 0; k < sizeof NS/sizeof *NS; k++) {
        int n = NS[k];
        pos *p = malloc((size_t)n * sizeof *p);
        place(p, n, 2, false);
        int rules = -2; double mb = 0, comp = 0;
        if (n <= (argc > 1 ? atoi(argv[1]) : 1024)) eager(n, 2, p, &rules, &mb, &comp);
        long probes = 0;
        double t0 = ms();
        long res = sweep(p, n, 2, &probes, NULL);
        double ts = ms() - t0;
        if (rules >= 0)
            printf("%6d | %11d %7.1f %8.1f ms | %9ld %9ld %6.2f ms\n",
                   n, rules, mb, comp, probes, res, ts);
        else
            printf("%6d | %11s %7s %11s | %9ld %9ld %6.2f ms\n",
                   n, "(skipped)", "-", "-", probes, res, ts);
        fflush(stdout);
        free(p);
    }

    printf("\n== dimensionality: does adding a dimension cost or save? (N=65536)\n");
    printf("  dims |    probes    results   pairs/entity\n");
    printf("-------+---------------------------------------\n");
    for (int ndim = 1; ndim <= 3; ndim++) {
        int n = 65536;
        pos *p = malloc((size_t)n * sizeof *p);
        place(p, n, ndim, false);
        long probes = 0;
        long res = sweep(p, n, ndim, &probes, NULL);
        printf("   %dD  | %9ld %9ld %10.1f\n", ndim, probes, res, (double)res/n);
        free(p);
    }

    printf("\n== one indexed axis vs every bound axis (N=65536)\n");
    printf("  dims |   1-axis probes   all-axis probes   results   ratio\n");
    printf("-------+------------------------------------------------------\n");
    for (int ndim = 1; ndim <= 3; ndim++) {
        int n = 65536;
        pos *p = malloc((size_t)n * sizeof *p);
        place(p, n, ndim, false);
        long p1 = 0, p2 = 0;
        long r1 = sweep(p, n, ndim, &p1, NULL);
        long r2 = bucketed(p, n, ndim, &p2);
        printf("   %dD  | %14ld %17ld %9ld %6.0fx%s\n", ndim, p1, p2, r2,
               (double)p1 / (double)(p2 ? p2 : 1), r1 == r2 ? "" : "  RESULTS DIFFER!");
        free(p);
    }

    printf("\n== the counter-case: an axis with no spread (N=16384, 2D)\n");
    {
        int n = 16384;
        pos *p = malloc((size_t)n * sizeof *p);
        place(p, n, 2, true);                 /* every entity shares x */
        long probes = 0;
        double t0 = ms();
        long res = sweep(p, n, 2, &probes, NULL);
        double ts = ms() - t0;
        printf("   flat x | probes %ld (N^2/2 = %ld)  results %ld  %.2f ms\n",
               probes, (long)n * (n-1) / 2, res, ts);
        free(p);
    }
    return 0;
}
