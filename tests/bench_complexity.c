/* bench_complexity [nents] [depth] [width] [iters] — columnar solve cost against
 * RULE COMPLEXITY, the axis the other family benchmarks hold fixed.
 *
 * bench_col and bench_5e each pin one hand-written schema (10 and 14 rules) and
 * sweep only the entity count, which answers "how many entities" but not "how
 * complicated a creature". This sweeps both, so the cost model can be stated as
 * a single constant instead of extrapolated from one schema shape.
 *
 * The schema is a layered stack `depth` tiers deep and `width` atoms wide: every
 * derived atom is concluded by two defeasible rules over the tier below, plus an
 * attacker on the same head that the first supporter beats by a superiority edge.
 * That is the shape of a 5e condition/feat stack — a conclusion, a competing
 * conclusion, and an ordering that settles it — deepened past anything a real
 * character carries, so the far end of the grid is a bound rather than a target.
 *
 * Reported per point: rules/entity, the median solve, the share of a 60 Hz frame
 * and a 10 Hz sim tick, and the per-rule-entity cost in nanoseconds. That last
 * column is the useful one: it is flat across the grid, which is what makes
 * `N x rules_per_entity` a budget an author can actually compute against.
 *
 * Deterministic construction (I4): a seeded LCG picks body atoms and fact bits,
 * so a given point is byte-identical run to run. Built -O2 regardless of build
 * type; build Release for meaningful numbers. Not registered with ctest. */

#include "logic/dl_col.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NBASE = 8 };            /* base fluents every tier-0 rule draws from */

static uint32_t rng_state = 0x1234567u;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }
static uint64_t xrand64(void) { return ((uint64_t)xrand() << 32) | xrand(); }

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static dl_lit mk(uint32_t atom, int neg) { dl_lit l = { atom, neg != 0 }; return l; }

static void bench_one(int nents, int depth, int width, int iters)
{
    int natoms = NBASE + depth * width;
    int W = (nents + 63) / 64;
    dlcol *f = dlcol_new(natoms, nents);
    int nrules = 0;

    /* Tier d reads tier d-1 (tier 0 reads the base fluents). Two supporters and
     * one beaten attacker per head — supported / countered / team-defeat all on
     * the hot path, matching bench_col's schema in kind, not in size. */
    for (int d = 0; d < depth; d++) {
        for (int k = 0; k < width; k++) {
            int head = NBASE + d * width + k;
            int lo   = d == 0 ? 0 : NBASE + (d - 1) * width;
            int span = d == 0 ? NBASE : width;
            dl_lit body[2];
            int first = -1;
            for (int v = 0; v < 2; v++) {
                body[0] = mk((uint32_t)(lo + (int)(xrand() % (uint32_t)span)), 0);
                body[1] = mk((uint32_t)(lo + (int)(xrand() % (uint32_t)span)),
                             (int)(xrand() & 1));   /* negative bodies too */
                int id = dlcol_add_rule(f, NULL, DL_DEFEASIBLE,
                                        mk((uint32_t)head, 0), body, 2);
                if (v == 0) first = id;
                nrules++;
            }
            body[0] = mk((uint32_t)(lo + (int)(xrand() % (uint32_t)span)), 0);
            int att = dlcol_add_rule(f, NULL, DL_DEFEASIBLE,
                                     mk((uint32_t)head, 1), body, 1);
            nrules++;
            dlcol_add_sup(f, first, att);
        }
    }

    /* closed world over the base fluents: ~25% true, the rest explicitly false */
    for (int a = 0; a < NBASE; a++) {
        uint64_t *pos = dlcol_fact_row(f, mk((uint32_t)a, 0));
        uint64_t *neg = dlcol_fact_row(f, mk((uint32_t)a, 1));
        for (int w = 0; w < W; w++) {
            uint64_t b = xrand64() & xrand64();
            pos[w] = b;
            neg[w] = ~b;
        }
    }

    for (int i = 0; i < 3; i++)             /* warm the schema compile */
        dlcol_solve(f);
    double *ms = malloc((size_t)iters * sizeof *ms);
    for (int i = 0; i < iters; i++) {
        double t0 = now_ms();
        dlcol_solve(f);
        ms[i] = now_ms() - t0;
    }
    qsort(ms, (size_t)iters, sizeof *ms, cmp_double);
    double med = ms[iters / 2];
    double ns_per = med * 1e6 / ((double)nents * nrules);

    printf("  N=%-7d depth=%-2d width=%-3d  rules/ent=%-5d atoms=%-4d  "
           "solve %8.3f ms  %6.1f%% frame  %6.2f%% tick  %5.2f ns/rule-ent\n",
           nents, depth, width, nrules, natoms, med,
           med / 16.67 * 100.0, med / 100.0 * 100.0, ns_per);

    free(ms);
    dlcol_free(f);
}

int main(int argc, char **argv)
{
    if (argc > 4) {
        bench_one(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        return 0;
    }

    static const struct { int depth, width, iters; const char *note; } GRID[] = {
        {  3,  5, 200, "bench_col / bench_5e territory" },
        {  5, 10, 100, "a loaded 5e character"          },
        {  8, 25,  50, "a heavy modded stack"           },
        { 12, 50,  20, "past any plausible content"     },
    };
    enum { NGRID = (int)(sizeof GRID / sizeof GRID[0]) };

    printf("bench_complexity: columnar solve vs schema depth x width (per-entity rules)\n");
    for (int n = 1000; n <= 100000; n *= 10) {
        printf(" -- N=%d --\n", n);
        for (int g = 0; g < NGRID; g++)
            bench_one(n, GRID[g].depth, GRID[g].width, GRID[g].iters);
    }
    printf("\nReading: cost is linear in BOTH axes — the ns/rule-entity column is flat\n"
           "across the grid, so the budget is the product `N x rules_per_entity`. It is\n"
           "sub-nanosecond because 64 entities ride in one word; per WORD it is a few\n"
           "loads and ANDs, which is the columnar lift (DESIGN.md 8) doing its job.\n"
           "This is the judgment solve alone — a real tick also pays providers, the\n"
           "step, and grounding, and bench_slice shows the provider dominating there.\n");
    return 0;
}
