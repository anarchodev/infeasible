/* Golden test for the ordered-axis index (§8.3, #247).
 *
 * The index exists so a difference bound can GENERATE pairs instead of
 * filtering them, and the only property that makes that substitution safe is
 * that it drops nothing: a superset costs a comparison the rule was going to
 * make anyway, a missing pair is a rule instance that silently never exists.
 * So every case here asserts set EQUALITY against an exhaustive O(N^2) filter,
 * not containment.
 *
 * The degenerate rows are the point of the file. An index that is merely
 * useless on flat data is fine; one that is WRONG there is not, and "every
 * entity on the same coordinate" is exactly the shape a spatial intuition
 * forgets to test. */

#include "state/numaxis.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

enum { MAXAX = 3 };

static intern *SY;
static world  *W;

/* Declare `naxis` numeric fluents over `nent` lanes and set the given values;
 * fills `cell` with the ground atoms in the layout numaxis_build expects. */
static void build_world(int naxis, int nent, const long *vals, uint32_t *cell)
{
    for (int a = 0; a < naxis; a++)
        for (int e = 0; e < nent; e++) {
            char b[32];
            snprintf(b, sizeof b, "ax%d(u%d)", a, e);
            uint32_t at = intern_id(SY, b);
            world_declare_num(W, at, -1000000, 1000000, true);
            world_set_num(W, at, vals[(size_t)a * nent + e]);
            cell[(size_t)a * nent + e] = at;
        }
}

/* The oracle: every unordered pair, filtered. */
static long brute(int naxis, int nent, const long *vals, const long *radius,
                  uint32_t *out)
{
    long n = 0;
    for (int i = 0; i < nent; i++)
        for (int j = i + 1; j < nent; j++) {
            bool ok = true;
            for (int a = 0; a < naxis && ok; a++) {
                long d = vals[(size_t)a * nent + i] - vals[(size_t)a * nent + j];
                if (d < 0) d = -d;
                ok = d <= radius[a];
            }
            if (ok) { out[2 * n] = (uint32_t)i; out[2 * n + 1] = (uint32_t)j; n++; }
        }
    return n;
}

/* One case end to end: index vs oracle, exact set equality AND identical order
 * (the oracle emits ascending (low, high), which is the canonical order the
 * index promises — so comparing the arrays flat checks both at once). */
static int case_run(const char *name, int naxis, int nent, const long *vals,
                    const long *radius)
{
    uint32_t *cell = malloc((size_t)(naxis * nent ? naxis * nent : 1) * sizeof *cell);
    build_world(naxis, nent, vals, cell);

    long capn = (long)nent * nent + 2;
    uint32_t *got = malloc((size_t)capn * 2 * sizeof *got);
    uint32_t *want = malloc((size_t)capn * 2 * sizeof *want);

    numaxis *ix = numaxis_build(W, naxis, nent, cell, radius);
    if (!ix) { fprintf(stderr, "FAIL %s: build returned NULL\n", name); return 1; }
    long n = numaxis_pairs(ix, got, capn * 2);
    long m = brute(naxis, nent, vals, radius, want);

    if (n != m) {
        fprintf(stderr, "FAIL %s: %ld pairs, oracle says %ld\n", name, n, m);
        return 1;
    }
    if (memcmp(got, want, (size_t)n * 2 * sizeof *got) != 0) {
        fprintf(stderr, "FAIL %s: pair set or order differs from the oracle\n", name);
        for (long k = 0; k < n; k++)
            if (got[2*k] != want[2*k] || got[2*k+1] != want[2*k+1]) {
                fprintf(stderr, "  first at %ld: got (%u,%u) want (%u,%u)\n", k,
                        got[2*k], got[2*k+1], want[2*k], want[2*k+1]);
                break;
            }
        return 1;
    }
    /* determinism: a second enumeration of the same index is byte-identical */
    uint32_t *again = malloc((size_t)capn * 2 * sizeof *again);
    long n2 = numaxis_pairs(ix, again, capn * 2);
    if (n2 != n || memcmp(got, again, (size_t)n * 2 * sizeof *got) != 0) {
        fprintf(stderr, "FAIL %s: re-enumeration differs (I4)\n", name);
        return 1;
    }
    /* separability: with every axis indexed, nothing is examined in vain */
    long probes = numaxis_probes(ix);
    if (probes < n) {
        fprintf(stderr, "FAIL %s: %ld probes for %ld pairs — probes cannot be fewer\n",
                name, probes, n);
        return 1;
    }
    free(again); free(got); free(want); free(cell);
    numaxis_free(ix);
    printf("  %-34s %4ld pairs, %5ld probes\n", name, n, probes);
    return 0;
}

static uint32_t rs = 7;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

int main(void)
{
    SY = intern_new();
    W  = world_new(SY);
    int bad = 0;

    /* a scattered population, one axis at a time */
    for (int naxis = 1; naxis <= MAXAX; naxis++) {
        enum { N = 200 };
        long vals[MAXAX * N], radius[MAXAX];
        for (int a = 0; a < naxis; a++) {
            radius[a] = 1;
            for (int e = 0; e < N; e++) vals[a * N + e] = (long)(rnd() % 20);
        }
        char nm[48];
        snprintf(nm, sizeof nm, "%dD scattered, radius 1", naxis);
        bad |= case_run(nm, naxis, N, vals, radius);
    }

    /* radius 0 — "equal on this axis", the boundary the cell width folds to 1 */
    {
        enum { N = 120 };
        long vals[2 * N], radius[2] = { 0, 0 };
        for (int e = 0; e < N; e++) { vals[e] = e % 7; vals[N + e] = e % 5; }
        bad |= case_run("2D radius 0 (equality join)", 2, N, vals, radius);
    }

    /* NEGATIVE coordinates straddling zero: C division truncates toward zero,
     * so a bucket function that forgets to floor folds -1 and 0 together and
     * splits the band asymmetrically around the origin. */
    {
        enum { N = 160 };
        long vals[2 * N], radius[2] = { 2, 2 };
        for (int e = 0; e < N; e++) {
            vals[e]     = (long)(rnd() % 21) - 10;
            vals[N + e] = (long)(rnd() % 21) - 10;
        }
        bad |= case_run("2D negative coords, radius 2", 2, N, vals, radius);
    }

    /* the flat axis: every lane on one coordinate. Useless, and it must still
     * be exactly right — this is bench_dbound's counter-case as a correctness
     * test rather than a cost one. */
    {
        enum { N = 90 };
        long vals[2 * N], radius[2] = { 1, 1 };
        for (int e = 0; e < N; e++) { vals[e] = 0; vals[N + e] = (long)(rnd() % 3); }
        bad |= case_run("2D flat first axis", 2, N, vals, radius);
    }

    /* a radius wider than the whole spread: every pair matches */
    {
        enum { N = 60 };
        long vals[N], radius[1] = { 10000 };
        for (int e = 0; e < N; e++) vals[e] = (long)(rnd() % 50);
        bad |= case_run("1D radius wider than the range", 1, N, vals, radius);
    }

    /* clustered: most lanes in one bucket, a few far away — the density skew a
     * uniform bucketing is worst at, and it must stay correct */
    {
        enum { N = 150 };
        long vals[2 * N], radius[2] = { 1, 1 };
        for (int e = 0; e < N; e++) {
            bool far = (e % 25) == 0;
            vals[e]     = far ? (long)(rnd() % 500) : (long)(rnd() % 2);
            vals[N + e] = far ? (long)(rnd() % 500) : (long)(rnd() % 2);
        }
        bad |= case_run("2D clustered + outliers", 2, N, vals, radius);
    }

    /* degenerate populations */
    {
        long vals[1] = { 5 }, radius[1] = { 1 };
        bad |= case_run("one lane", 1, 1, vals, radius);
        bad |= case_run("zero lanes", 1, 0, vals, radius);
    }

    /* out-of-range axis counts are refused rather than mis-indexed */
    {
        long r[1] = { 1 };
        uint32_t cell[1] = { 0 };
        CHECK(numaxis_build(W, 0, 1, cell, r) == NULL);
        CHECK(numaxis_build(W, NUMAXIS_MAXAXES + 1, 1, cell, r) == NULL);
    }

    world_free(W);
    intern_free(SY);
    if (bad) { fprintf(stderr, "test_numaxis: FAILED\n"); return 1; }
    printf("test_numaxis: all passed\n");
    return 0;
}
