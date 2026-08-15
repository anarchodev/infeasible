#include "lang/dbound.h"

/* `c cmp d` is `d cmp' c` with the comparator mirrored: <= becomes >=, <
 * becomes >, and = is its own mirror. */
static world_cmp mirror(world_cmp c)
{
    switch (c) {
    case WORLD_CMP_LE: return WORLD_CMP_GE;
    case WORLD_CMP_LT: return WORLD_CMP_GT;
    case WORLD_CMP_GE: return WORLD_CMP_LE;
    case WORLD_CMP_GT: return WORLD_CMP_LT;
    case WORLD_CMP_EQ: return WORLD_CMP_EQ;
    }
    return c;
}

int dbound_normalise(uint32_t pred, uint32_t u, uint32_t v, world_cmp cmp,
                     long c, bool flipped, dbound *out)
{
    if (u == v) return 0;                       /* d is identically 0: not a join */
    if (flipped) cmp = mirror(cmp);
    switch (cmp) {
    case WORLD_CMP_LT:
        c -= 1;                                 /* d < c  is  d <= c-1 */
        /* fall through */
    case WORLD_CMP_LE:
        out[0] = (dbound){ pred, u, v, c };
        return 1;
    case WORLD_CMP_GT:
        c += 1;                                 /* d > c  is  d >= c+1 */
        /* fall through */
    case WORLD_CMP_GE:
        /* d >= c  is  -d <= -c, and -d is val(v) - val(u): hi and lo trade. */
        out[0] = (dbound){ pred, v, u, -c };
        return 1;
    case WORLD_CMP_EQ:
        out[0] = (dbound){ pred, u, v,  c };
        out[1] = (dbound){ pred, v, u, -c };
        return 2;
    }
    return 0;
}

/* The tightest bound recorded for one direction, or `absent`. */
static bool tightest(const dbound *in, int n, uint32_t pred, uint32_t hi,
                     uint32_t lo, long *k)
{
    bool found = false;
    for (int i = 0; i < n; i++) {
        if (in[i].pred != pred || in[i].hi != hi || in[i].lo != lo) continue;
        if (!found || in[i].k < *k) { *k = in[i].k; found = true; }
    }
    return found;
}

/* Conservative radius for -k2 <= d <= k1: any satisfying pair has |d| bounded
 * by the larger endpoint magnitude. An offset band (5 <= d <= 10) therefore
 * gets radius 10 — wider than the band itself, which is a superset and so
 * correct, since the conjuncts remain in the body. */
static long radius_of(long k1, long k2)
{
    long a = k1 < 0 ? -k1 : k1, b = k2 < 0 ? -k2 : k2;
    return a > b ? a : b;
}

int dbound_bands(const dbound *in, int n, dbound_band *out, int cap)
{
    int nb = 0;
    for (int i = 0; i < n; i++) {
        uint32_t p = in[i].pred, u = in[i].hi, v = in[i].lo;
        if (u > v) continue;              /* visit each variable pair once, from
                                           * its lower-id side, so the emitted
                                           * band does not depend on body order */
        long k1, k2;
        if (!tightest(in, n, p, u, v, &k1)) continue;
        if (!tightest(in, n, p, v, u, &k2)) continue;   /* half-space: no band */
        if (k1 + k2 < 0) continue;                      /* unsatisfiable */
        bool seen = false;
        for (int j = 0; j < nb && !seen; j++)
            seen = out[j].pred == p && out[j].u == u && out[j].v == v;
        if (seen || nb >= cap) continue;
        out[nb++] = (dbound_band){ p, u, v, radius_of(k1, k2) };
    }
    return nb;
}

bool dbound_unsat(const dbound *in, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t p = in[i].pred, u = in[i].hi, v = in[i].lo;
        if (u > v) continue;
        long k1, k2;
        if (!tightest(in, n, p, u, v, &k1)) continue;
        if (!tightest(in, n, p, v, u, &k2)) continue;
        if (k1 + k2 < 0) return true;
    }
    return false;
}
