#include "state/factindex.h"

#include <stdlib.h>
#include <string.h>

/* One predicate's extension: a flat run of `count` tuples, each `nargs` wide.
 * Insertion order is preserved (deterministic enumeration, I4). */
typedef struct {
    uint32_t  pred;
    int       nargs;
    uint32_t *tuples;      /* count * nargs, row-major */
    int       count, cap;
} bucket;

struct factindex {
    bucket *buckets;
    int     nb, cap;
    /* pred id -> bucket index, direct-indexed (intern ids are dense, like
     * world's fluent_of). Slot -1 = predicate absent. Grown geometrically. */
    int      *pred_of;
    uint32_t  pred_of_cap;
};

factindex *factindex_new(void)
{
    factindex *ix = calloc(1, sizeof *ix);
    return ix;
}

void factindex_free(factindex *ix)
{
    if (!ix) return;
    for (int i = 0; i < ix->nb; i++) free(ix->buckets[i].tuples);
    free(ix->buckets);
    free(ix->pred_of);
    free(ix);
}

void factindex_clear(factindex *ix)
{
    for (int i = 0; i < ix->nb; i++) ix->buckets[i].count = 0;
}

/* Ensure pred_of covers `pred`, filling new slots with -1. */
static void ensure_pred_of(factindex *ix, uint32_t pred)
{
    if (pred < ix->pred_of_cap) return;
    uint32_t ncap = ix->pred_of_cap ? ix->pred_of_cap : 16;
    while (ncap <= pred) ncap *= 2;
    ix->pred_of = realloc(ix->pred_of, ncap * sizeof *ix->pred_of);
    for (uint32_t i = ix->pred_of_cap; i < ncap; i++) ix->pred_of[i] = -1;
    ix->pred_of_cap = ncap;
}

/* Bucket for `pred`, creating it (with arity `nargs`) if absent. */
static bucket *get_bucket(factindex *ix, uint32_t pred, int nargs)
{
    ensure_pred_of(ix, pred);
    int bi = ix->pred_of[pred];
    if (bi >= 0) return &ix->buckets[bi];

    if (ix->nb == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 8;
        ix->buckets = realloc(ix->buckets, (size_t)ix->cap * sizeof *ix->buckets);
    }
    bi = ix->nb++;
    ix->pred_of[pred] = bi;
    bucket *b = &ix->buckets[bi];
    b->pred = pred;
    b->nargs = nargs;
    b->tuples = NULL;
    b->count = b->cap = 0;
    return b;
}

static const bucket *find_bucket(const factindex *ix, uint32_t pred)
{
    if (pred >= ix->pred_of_cap) return NULL;
    int bi = ix->pred_of[pred];
    return bi >= 0 ? &ix->buckets[bi] : NULL;
}

void factindex_add(factindex *ix, uint32_t pred, const uint32_t *args, int nargs)
{
    if (nargs < 0) nargs = 0;
    if (nargs > FACTINDEX_MAXARGS) nargs = FACTINDEX_MAXARGS;
    bucket *b = get_bucket(ix, pred, nargs);

    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->tuples = realloc(b->tuples, (size_t)b->cap * (size_t)b->nargs * sizeof(uint32_t));
    }
    uint32_t *row = &b->tuples[(size_t)b->count * (size_t)b->nargs];
    for (int i = 0; i < b->nargs; i++) row[i] = args ? args[i] : 0;
    b->count++;
}

int factindex_count(const factindex *ix, uint32_t pred)
{
    const bucket *b = find_bucket(ix, pred);
    return b ? b->count : 0;
}

void factindex_scan(const factindex *ix, uint32_t pred,
                    const bool *filter, const uint32_t *want,
                    factindex_cursor *cur)
{
    const bucket *b = find_bucket(ix, pred);
    cur->ix    = ix;
    cur->pos   = 0;
    cur->bucket = b ? (int)(b - ix->buckets) : -1;
    cur->nargs = b ? b->nargs : 0;
    for (int i = 0; i < cur->nargs; i++) {
        cur->filtered[i] = filter ? filter[i] : false;
        cur->want[i]     = (filter && filter[i] && want) ? want[i] : 0;
    }
}

bool factindex_next(factindex_cursor *cur, uint32_t *out)
{
    if (cur->bucket < 0) return false;
    const bucket *b = &cur->ix->buckets[cur->bucket];
    while (cur->pos < b->count) {
        const uint32_t *row = &b->tuples[(size_t)cur->pos * (size_t)b->nargs];
        cur->pos++;
        bool ok = true;
        for (int i = 0; i < cur->nargs; i++)
            if (cur->filtered[i] && row[i] != cur->want[i]) { ok = false; break; }
        if (!ok) continue;
        for (int i = 0; i < cur->nargs; i++) out[i] = row[i];
        return true;
    }
    cur->bucket = -1;
    return false;
}
