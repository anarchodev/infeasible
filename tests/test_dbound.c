/* Golden test for difference-bound normalisation (§8.3, #249).
 *
 * The reduction to canonical form is small and entirely sign-errors: five
 * comparison operators, a constant that may sit on either side, `>=` flipping
 * which variable is the minuend, and off-by-ones on the strict forms. A
 * mirrored bound is still a well-formed bound, so nothing about its SHAPE
 * catches the mistake.
 *
 * So the test never inspects the normalised fields. It evaluates them. For
 * every conjunct, the set of (val(u), val(v)) pairs the canonical bounds admit
 * must equal the set the original conjunct admits, over a small integer grid
 * straddling zero. A sign flip changes that set and is caught; a
 * representation change is not, and should not be.
 *
 * The band tests then pin the property the index depends on: a band must be a
 * SUPERSET of what the bounds admit (#247's conservative-never-lossy contract),
 * because the conjuncts stay in the rule body and are re-checked, so a wide
 * radius costs a comparison while a narrow one loses an answer. */

#include "lang/dbound.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

enum { LO = -6, HI = 6 };                 /* the grid, straddling zero */
enum { PRED = 100, U = 1, V = 2, W = 3 };

static bool holds(long l, world_cmp c, long r)
{
    switch (c) {
    case WORLD_CMP_LE: return l <= r;
    case WORLD_CMP_LT: return l <  r;
    case WORLD_CMP_GE: return l >= r;
    case WORLD_CMP_GT: return l >  r;
    case WORLD_CMP_EQ: return l == r;
    }
    return false;
}

/* Do all `n` canonical bounds hold when val(U)=a, val(V)=b? */
static bool bounds_admit(const dbound *d, int n, long a, long b)
{
    for (int i = 0; i < n; i++) {
        long hi = d[i].hi == U ? a : b;
        long lo = d[i].lo == U ? a : b;
        if (hi - lo > d[i].k) return false;
    }
    return true;
}

static const char *cmp_name(world_cmp c)
{
    switch (c) {
    case WORLD_CMP_LE: return "<=";  case WORLD_CMP_LT: return "<";
    case WORLD_CMP_GE: return ">=";  case WORLD_CMP_GT: return ">";
    case WORLD_CMP_EQ: return "=";
    }
    return "?";
}

/* Every operator, both constant positions, a range of constants: the canonical
 * bounds must admit exactly the pairs the original conjunct admits. */
static int case_equivalence(void)
{
    static const world_cmp CMPS[] = { WORLD_CMP_LE, WORLD_CMP_LT, WORLD_CMP_GE,
                                      WORLD_CMP_GT, WORLD_CMP_EQ };
    int checked = 0;
    for (unsigned ci = 0; ci < sizeof CMPS / sizeof CMPS[0]; ci++)
        for (int flip = 0; flip <= 1; flip++)
            for (long c = -3; c <= 3; c++) {
                dbound d[2];
                int n = dbound_normalise(PRED, U, V, CMPS[ci], c, flip != 0, d);
                if (n == 0) {
                    fprintf(stderr, "FAIL: %s%s%ld normalised to nothing\n",
                            flip ? "c " : "d ", cmp_name(CMPS[ci]), c);
                    return 1;
                }
                for (long a = LO; a <= HI; a++)
                    for (long b = LO; b <= HI; b++) {
                        long dd = a - b;
                        bool want = flip ? holds(c, CMPS[ci], dd)
                                         : holds(dd, CMPS[ci], c);
                        bool got = bounds_admit(d, n, a, b);
                        if (want != got) {
                            fprintf(stderr,
                                "FAIL: %s %s %ld at (a=%ld b=%ld): want %d got %d\n",
                                flip ? "c" : "d", cmp_name(CMPS[ci]), c, a, b,
                                (int)want, (int)got);
                            return 1;
                        }
                        checked++;
                    }
            }
    printf("  equivalence      %d (operator, position, constant, a, b) points\n", checked);
    return 0;
}

/* A band must cover everything the bounds admit. This is the safety direction:
 * too wide is a wasted comparison, too narrow is a lost rule instance. */
static int band_covers(const dbound *d, int n, const dbound_band *b, int nb)
{
    for (long a = LO; a <= HI; a++)
        for (long x = LO; x <= HI; x++) {
            if (!bounds_admit(d, n, a, x)) continue;
            for (int i = 0; i < nb; i++) {
                long delta = a - x;
                if (delta < 0) delta = -delta;
                if (delta > b[i].r) {
                    fprintf(stderr, "FAIL: band r=%ld misses admitted pair "
                                    "(%ld, %ld)\n", b[i].r, a, x);
                    return 1;
                }
            }
        }
    return 0;
}

int main(void)
{
    if (case_equivalence()) return 1;

    /* a variable against itself is not a join */
    {
        dbound d[2];
        CHECK(dbound_normalise(PRED, U, U, WORLD_CMP_LE, 1, false, d) == 0);
    }

    /* the symmetric band both directions describe */
    {
        dbound d[4]; int n = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 1, false, d + n);
        n += dbound_normalise(PRED, V, U, WORLD_CMP_LE, 1, false, d + n);
        dbound_band b[4];
        int nb = dbound_bands(d, n, b, 4);
        CHECK(nb == 1);
        CHECK(b[0].r == 1);
        CHECK(band_covers(d, n, b, nb) == 0);
        printf("  symmetric        radius %ld\n", b[0].r);
    }

    /* one direction only is a HALF-SPACE — no finite radius covers it, so it
     * must yield no band rather than a plausible-looking one */
    {
        dbound d[2];
        int n = dbound_normalise(PRED, U, V, WORLD_CMP_LE, 5, false, d);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 0);
        printf("  half-space       no band\n");
    }

    /* an equality is both directions at once, so it IS a band, of radius 0 */
    {
        dbound d[2];
        int n = dbound_normalise(PRED, U, V, WORLD_CMP_EQ, 0, false, d);
        CHECK(n == 2);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 1);
        CHECK(b[0].r == 0);
        CHECK(band_covers(d, n, b, 1) == 0);
        printf("  equality         radius %ld\n", b[0].r);
    }

    /* an OFFSET band: 5 <= d <= 10 never straddles zero, and the conservative
     * radius is the larger endpoint — wider than the band, which is the safe
     * direction */
    {
        dbound d[4]; int n = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 10, false, d + n);
        n += dbound_normalise(PRED, U, V, WORLD_CMP_GE,  5, false, d + n);
        dbound_band b[4];
        int nb = dbound_bands(d, n, b, 4);
        CHECK(nb == 1);
        CHECK(b[0].r == 10);
        CHECK(band_covers(d, n, b, nb) == 0);
        printf("  offset 5..10     radius %ld\n", b[0].r);
    }

    /* unsatisfiable: d <= 1 and d >= 4 together admit nothing */
    {
        dbound d[4]; int n = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 1, false, d + n);
        n += dbound_normalise(PRED, U, V, WORLD_CMP_GE, 4, false, d + n);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 0);
        CHECK(dbound_unsat(d, n));
        printf("  unsatisfiable    no band, flagged\n");
    }

    /* duplicates on one direction keep the tightest */
    {
        dbound d[6]; int n = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 9, false, d + n);
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 2, false, d + n);
        n += dbound_normalise(PRED, V, U, WORLD_CMP_LE, 7, false, d + n);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 1);
        CHECK(b[0].r == 7);                    /* max(2, 7), not max(9, 7) */
        CHECK(band_covers(d, n, b, 1) == 0);
        printf("  duplicates       radius %ld (tightest kept)\n", b[0].r);
    }

    /* body order must not change the emitted bands (I4: grounding order is
     * semantics, so a band that depends on conjunct order is a defect) */
    {
        dbound a[4], z[4]; int n = 0, m = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 3, false, a + n);
        n += dbound_normalise(PRED, V, U, WORLD_CMP_LE, 1, false, a + n);
        m += dbound_normalise(PRED, V, U, WORLD_CMP_LE, 1, false, z + m);
        m += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 3, false, z + m);
        dbound_band ba[4], bz[4];
        int na = dbound_bands(a, n, ba, 4), nz = dbound_bands(z, m, bz, 4);
        CHECK(na == 1 && nz == 1);
        CHECK(memcmp(ba, bz, sizeof *ba) == 0);
        printf("  order-invariant  identical bands\n");
    }

    /* two independent variable pairs on one predicate are two bands */
    {
        dbound d[8]; int n = 0;
        n += dbound_normalise(PRED, U, V, WORLD_CMP_LE, 1, false, d + n);
        n += dbound_normalise(PRED, V, U, WORLD_CMP_LE, 1, false, d + n);
        n += dbound_normalise(PRED, U, W, WORLD_CMP_LE, 2, false, d + n);
        n += dbound_normalise(PRED, W, U, WORLD_CMP_LE, 2, false, d + n);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 2);
        printf("  two var pairs    2 bands\n");
    }

    /* a different predicate on the same pair does not pair up with it */
    {
        dbound d[4]; int n = 0;
        n += dbound_normalise(PRED,     U, V, WORLD_CMP_LE, 1, false, d + n);
        n += dbound_normalise(PRED + 1, V, U, WORLD_CMP_LE, 1, false, d + n);
        dbound_band b[4];
        CHECK(dbound_bands(d, n, b, 4) == 0);
        printf("  cross-predicate  no band\n");
    }

    printf("test_dbound: all passed\n");
    return 0;
}
