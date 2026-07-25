/* Fact-store extension index (§5.2 item 4, EPIC #26 / the matcher #28). Pins the
 * one new data structure the semi-naïve matcher needs: predicate -> enumerable
 * set of true ground tuples, probeable by a bound-arg pattern, enumerated in
 * insertion order (deterministic, I4). */

#include "state/factindex.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } \
    } while (0)

/* collect a scan's tuples into `rows` (flat, per_nargs wide); return count */
static int collect(factindex *ix, uint32_t pred, const bool *filt,
                   const uint32_t *want, int nargs, uint32_t *rows, int maxrows)
{
    factindex_cursor c;
    factindex_scan(ix, pred, filt, want, &c);
    uint32_t out[FACTINDEX_MAXARGS];
    int n = 0;
    while (factindex_next(&c, out) && n < maxrows) {
        memcpy(&rows[(size_t)n * nargs], out, (size_t)nargs * sizeof(uint32_t));
        n++;
    }
    return n;
}

int main(void)
{
    factindex *ix = factindex_new();

    enum { ADJ = 3, COND = 7, ALONE = 9 };   /* pretend intern ids */

    /* adj: (1,2) (1,5) (4,2) (1,9)  — a sparse binary relation */
    factindex_add(ix, ADJ, (uint32_t[]){1, 2}, 2);
    factindex_add(ix, ADJ, (uint32_t[]){1, 5}, 2);
    factindex_add(ix, ADJ, (uint32_t[]){4, 2}, 2);
    factindex_add(ix, ADJ, (uint32_t[]){1, 9}, 2);
    /* cond: unary {2, 5} */
    factindex_add(ix, COND, (uint32_t[]){2}, 1);
    factindex_add(ix, COND, (uint32_t[]){5}, 1);
    /* a 0-arity proposition */
    factindex_add(ix, ALONE, NULL, 0);

    /* counts (selectivity) */
    CHECK(factindex_count(ix, ADJ) == 4);
    CHECK(factindex_count(ix, COND) == 2);
    CHECK(factindex_count(ix, ALONE) == 1);
    CHECK(factindex_count(ix, 42) == 0);          /* absent predicate */

    uint32_t rows[16];

    /* full scan of adj yields all 4 in INSERTION ORDER */
    {
        int n = collect(ix, ADJ, NULL, NULL, 2, rows, 8);
        CHECK(n == 4);
        CHECK(rows[0] == 1 && rows[1] == 2);
        CHECK(rows[2] == 1 && rows[3] == 5);
        CHECK(rows[4] == 4 && rows[5] == 2);
        CHECK(rows[6] == 1 && rows[7] == 9);
    }

    /* probe adj with arg0 bound to 1: yields (1,2)(1,5)(1,9), still in order */
    {
        bool filt[2] = { true, false };
        uint32_t want[2] = { 1, 0 };
        int n = collect(ix, ADJ, filt, want, 2, rows, 8);
        CHECK(n == 3);
        CHECK(rows[0] == 1 && rows[1] == 2);
        CHECK(rows[2] == 1 && rows[3] == 5);
        CHECK(rows[4] == 1 && rows[5] == 9);
    }

    /* probe adj with arg1 bound to 2: yields (1,2)(4,2) */
    {
        bool filt[2] = { false, true };
        uint32_t want[2] = { 0, 2 };
        int n = collect(ix, ADJ, filt, want, 2, rows, 8);
        CHECK(n == 2);
        CHECK(rows[0] == 1 && rows[1] == 2);
        CHECK(rows[2] == 4 && rows[3] == 2);
    }

    /* both positions bound to an existing tuple: exactly one match */
    {
        bool filt[2] = { true, true };
        uint32_t want[2] = { 4, 2 };
        CHECK(collect(ix, ADJ, filt, want, 2, rows, 8) == 1);
    }
    /* both positions bound to a NON-tuple: zero matches */
    {
        bool filt[2] = { true, true };
        uint32_t want[2] = { 4, 5 };
        CHECK(collect(ix, ADJ, filt, want, 2, rows, 8) == 0);
    }

    /* a 0-arity proposition scans as a single empty tuple */
    CHECK(collect(ix, ALONE, NULL, NULL, 0, rows, 8) == 1);

    /* scanning an absent predicate yields nothing (and doesn't crash) */
    CHECK(collect(ix, 42, NULL, NULL, 0, rows, 8) == 0);

    /* clear drops tuples but the index stays usable */
    factindex_clear(ix);
    CHECK(factindex_count(ix, ADJ) == 0);
    factindex_add(ix, ADJ, (uint32_t[]){8, 8}, 2);
    CHECK(factindex_count(ix, ADJ) == 1);

    factindex_free(ix);
    printf("test_factindex: all passed\n");
    return 0;
}
