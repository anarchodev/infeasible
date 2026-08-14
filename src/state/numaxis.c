#include "state/numaxis.h"

#include <stdlib.h>
#include <string.h>

/* One bucket key per lane: the lane's coordinate on each axis divided by that
 * axis's cell width. Cell width = max(radius, 1), so two values within the
 * radius land in the same bucket or in an adjacent one — which is what bounds
 * the neighbourhood scan to 3 buckets per axis, and to ONE on a radius-0 axis
 * (an equality join, where an adjacent bucket is already out of band). */
typedef struct {
    long     key[NUMAXIS_MAXAXES];
    uint32_t lane;
} entry;

struct numaxis {
    int    naxis, nent;
    long   radius[NUMAXIS_MAXAXES];
    long   width[NUMAXIS_MAXAXES];
    long  *val;        /* [naxis*nent], read once at build */
    entry *sorted;     /* [nent], bucket-major then lane — the CSR-ish row */
    long   probes;     /* the last enumeration's examined-pair count */
};

/* Floor division: C truncates toward zero, which would fold -1 and 0 into one
 * bucket and split the band asymmetrically around the origin. */
static long fdiv(long a, long b)
{
    return a >= 0 ? a / b : -(((-a) + b - 1) / b);
}

static int entry_cmp(const void *pa, const void *pb)
{
    const entry *a = pa, *b = pb;
    for (int k = 0; k < NUMAXIS_MAXAXES; k++) {
        if (a->key[k] != b->key[k]) return a->key[k] < b->key[k] ? -1 : 1;
    }
    return a->lane < b->lane ? -1 : (a->lane > b->lane);
}

numaxis *numaxis_build(const world *w, int naxis, int nent,
                       const uint32_t *atom_cell, const long *radius)
{
    if (naxis < 1 || naxis > NUMAXIS_MAXAXES || nent < 0) return NULL;
    numaxis *ix = calloc(1, sizeof *ix);
    ix->naxis = naxis;
    ix->nent  = nent;
    ix->val    = malloc((size_t)(naxis * nent > 0 ? naxis * nent : 1) * sizeof *ix->val);
    ix->sorted = malloc((size_t)(nent ? nent : 1) * sizeof *ix->sorted);
    for (int a = 0; a < naxis; a++) {
        ix->radius[a] = radius[a] < 0 ? 0 : radius[a];
        ix->width[a]  = ix->radius[a] > 0 ? ix->radius[a] : 1;
    }
    for (int a = 0; a < naxis; a++)
        for (int e = 0; e < nent; e++) {
            uint32_t at = atom_cell[(size_t)a * nent + e];
            ix->val[(size_t)a * nent + e] = at ? world_get_num(w, at) : 0;
        }
    for (int e = 0; e < nent; e++) {
        ix->sorted[e].lane = (uint32_t)e;
        for (int k = 0; k < NUMAXIS_MAXAXES; k++)
            ix->sorted[e].key[k] = k < naxis
                ? fdiv(ix->val[(size_t)k * nent + e], ix->width[k]) : 0;
    }
    qsort(ix->sorted, (size_t)nent, sizeof *ix->sorted, entry_cmp);
    return ix;
}

void numaxis_free(numaxis *ix)
{
    if (!ix) return;
    free(ix->val); free(ix->sorted); free(ix);
}

long numaxis_probes(const numaxis *ix) { return ix ? ix->probes : 0; }

/* First index in `sorted` whose key is >= `key`. */
static int lower_bound(const numaxis *ix, const long *key)
{
    int lo = 0, hi = ix->nent;
    entry probe;
    memset(&probe, 0, sizeof probe);
    memcpy(probe.key, key, sizeof probe.key);
    probe.lane = 0;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (entry_cmp(&ix->sorted[mid], &probe) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static bool within(const numaxis *ix, int a, int b)
{
    for (int k = 0; k < ix->naxis; k++) {
        long d = ix->val[(size_t)k * ix->nent + a] - ix->val[(size_t)k * ix->nent + b];
        if (d < 0) d = -d;
        if (d > ix->radius[k]) return false;
    }
    return true;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y);
}

long numaxis_pairs(const numaxis *ix, uint32_t *out, long cap)
{
    if (!ix) return 0;
    long n = 0;
    ((numaxis *)ix)->probes = 0;
    if (ix->nent == 0) return 0;

    /* Per lane, visit the 3^naxis neighbouring buckets and keep the partners
     * inside the band. Emission is ascending by (low, high) — a function of the
     * lane numbering, not of bucket layout — so the order is canonical (I4)
     * whatever the sort did with equal keys. */
    int span[NUMAXIS_MAXAXES], nb = 1;
    for (int k = 0; k < ix->naxis; k++) {
        /* a radius-0 axis is an equality join: cell width is 1, so an adjacent
         * bucket differs by 1 and can never hold a partner. Skipping it turns
         * 3^k neighbours into 3^(k - zero axes) and makes the enumeration exact
         * rather than merely conservative on that axis. */
        span[k] = ix->radius[k] > 0 ? 3 : 1;
        nb *= span[k];
    }
    uint32_t *cand = malloc((size_t)ix->nent * sizeof *cand);

    for (int e = 0; e < ix->nent; e++) {
        long self[NUMAXIS_MAXAXES] = { 0 };
        for (int k = 0; k < ix->naxis; k++)
            self[k] = fdiv(ix->val[(size_t)k * ix->nent + e], ix->width[k]);
        int ncand = 0;
        for (int b = 0; b < nb; b++) {
            long key[NUMAXIS_MAXAXES] = { 0 };
            int rest = b;
            for (int k = 0; k < ix->naxis; k++) {
                key[k] = self[k] + (span[k] == 3 ? (rest % 3) - 1 : 0);
                rest /= span[k];
            }
            for (int i = lower_bound(ix, key); i < ix->nent; i++) {
                const entry *s = &ix->sorted[i];
                bool same = true;
                for (int k = 0; k < ix->naxis && same; k++) same = s->key[k] == key[k];
                if (!same) break;                       /* past this bucket's run */
                if ((int)s->lane <= e) continue;        /* each unordered pair once */
                ((numaxis *)ix)->probes++;
                if (within(ix, e, (int)s->lane)) cand[ncand++] = s->lane;
            }
        }
        qsort(cand, (size_t)ncand, sizeof *cand, u32_cmp);
        for (int i = 0; i < ncand; i++) {
            if (out && 2 * n + 1 < cap) { out[2 * n] = (uint32_t)e; out[2 * n + 1] = cand[i]; }
            n++;
        }
    }
    free(cand);
    return n;
}
