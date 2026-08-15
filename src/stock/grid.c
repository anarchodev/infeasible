#include "stock/grid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A HASHED cell bucket, counting-sorted — the shape tests/bench_slice.c's
 * hand-written reference already proves out, hashed rather than dense because
 * a `grid_x : int` is unbounded and a dense array over its declared range would
 * be arbitrarily large for a handful of entities.
 *
 * This answers the question #247 was left open on: a grid provider does NOT
 * want a general ordered-axis index. Knowing its own geometry — two integer
 * axes, a radius in whole cells — it hashes BOTH coordinates into one bucket
 * and scans nine of them, where a general index buckets each axis separately
 * and intersects. Same asymptotics, fewer passes, no per-axis machinery. */

enum { GRID_R = 1 };                 /* `adjacent` is one cell, Chebyshev */

struct stock_grid {
    world    *w;
    intern   *syms;
    int       nent;
    uint32_t *ent;                   /* [nent] entity atoms, install order */
    uint32_t *xa, *ya, *ba;          /* [nent] the grid_x/grid_y/grid_blocks atoms */
    long     *x, *y;                 /* [nent] positions as of the last refresh */
    bool     *blocks;                /* [nent] is this entity a sight blocker */
    uint32_t  p_adj, p_los, f_cheb, f_manh;
    /* bucket index: CSR over a power-of-two hash of (cell_x, cell_y) */
    int      *start, *item;          /* [nbucket+1], [nent] */
    int       nbucket;
    uint64_t  built_tick;
    bool      built;
    long      probes;
};

static uint32_t cell_hash(long cx, long cy)
{
    uint64_t h = (uint64_t)(cx * 0x9E3779B97F4A7C15ull) ^
                 (uint64_t)(cy * 0xC2B2AE3D27D4EB4Full);
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    return (uint32_t)h;
}

static int idx_of(const stock_grid *g, uint32_t e)
{
    for (int i = 0; i < g->nent; i++) if (g->ent[i] == e) return i;
    return -1;
}

void stock_grid_refresh(stock_grid *g)
{
    for (int i = 0; i < g->nent; i++) {
        g->x[i] = world_get_num(g->w, g->xa[i]);
        g->y[i] = world_get_num(g->w, g->ya[i]);
        g->blocks[i] = world_get(g->w, g->ba[i]);
    }
    /* counting sort into buckets: one pass to count, one to place */
    memset(g->start, 0, (size_t)(g->nbucket + 1) * sizeof *g->start);
    for (int i = 0; i < g->nent; i++) {
        uint32_t b = cell_hash(g->x[i], g->y[i]) & (uint32_t)(g->nbucket - 1);
        g->start[b + 1]++;
    }
    for (int b = 0; b < g->nbucket; b++) g->start[b + 1] += g->start[b];
    int *at = malloc((size_t)g->nbucket * sizeof *at);
    memcpy(at, g->start, (size_t)g->nbucket * sizeof *at);
    for (int i = 0; i < g->nent; i++) {
        uint32_t b = cell_hash(g->x[i], g->y[i]) & (uint32_t)(g->nbucket - 1);
        g->item[at[b]++] = i;
    }
    free(at);
    g->built = true;
    g->built_tick = world_tick(g->w);
}

static void ensure_fresh(stock_grid *g)
{
    if (!g->built || g->built_tick != world_tick(g->w)) stock_grid_refresh(g);
}

int stock_grid_chebyshev(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return -1;
    long dx = g->x[i] - g->x[j], dy = g->y[i] - g->y[j];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (int)(dx > dy ? dx : dy);
}

int stock_grid_manhattan(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return -1;
    long dx = g->x[i] - g->x[j], dy = g->y[i] - g->y[j];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (int)(dx + dy);
}

/* Line of sight: a supercover walk of the segment, blocked by any entity
 * flagged `grid_blocks` standing on a cell it crosses. The endpoints never
 * block their own sight. This is the MEASURED premise (§5.6) — obscurement,
 * darkvision, invisibility and blindsight are exceptions the story layers on
 * top, which is why there is no `can_see` here. */
static bool los_clear(stock_grid *g, int i, int j)
{
    long x0 = g->x[i], y0 = g->y[i], x1 = g->x[j], y1 = g->y[j];
    long dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    long dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
    long err = dx - dy, cx = x0, cy = y0;
    for (;;) {
        if (!(cx == x0 && cy == y0) && !(cx == x1 && cy == y1))
            for (int k = 0; k < g->nent; k++)
                if (g->blocks[k] && g->x[k] == cx && g->y[k] == cy) return false;
        if (cx == x1 && cy == y1) return true;
        long e2 = 2 * err;
        if (e2 > -dy) { err -= dy; cx += sx; }
        if (e2 <  dx) { err += dx; cy += sy; }
    }
}

/* ---- the provider callbacks ------------------------------------------------ */

static bool grid_point(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    stock_grid *g = ctx;
    if (nargs != 2) return false;
    ensure_fresh(g);
    int i = idx_of(g, args[0]), j = idx_of(g, args[1]);
    if (i < 0 || j < 0 || i == j) return false;
    if (pred == g->p_adj) {
        long dx = g->x[i] - g->x[j], dy = g->y[i] - g->y[j];
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx <= GRID_R && dy <= GRID_R;
    }
    if (pred == g->p_los) return los_clear(g, i, j);
    return false;
}

/* The generator (#254): walk the nine buckets around `a` rather than the sort.
 * Order is the bucket layout's, which the engine canonicalises — a rebuild that
 * reorders a bucket must not move a die roll (I4). */
static int grid_gen(void *ctx, uint32_t pred, uint32_t a, uint32_t *out, int cap)
{
    stock_grid *g = ctx;
    (void)pred;
    ensure_fresh(g);
    int i = idx_of(g, a);
    if (i < 0) return 0;
    int n = 0;
    for (long dx = -GRID_R; dx <= GRID_R; dx++)
        for (long dy = -GRID_R; dy <= GRID_R; dy++) {
            uint32_t b = cell_hash(g->x[i] + dx, g->y[i] + dy) &
                         (uint32_t)(g->nbucket - 1);
            for (int s = g->start[b]; s < g->start[b + 1]; s++) {
                int k = g->item[s];
                if (k == i) continue;
                g->probes++;
                /* the bucket is HASHED, so a collision puts unrelated cells in
                 * it — the exact test is what makes the run correct, and it is
                 * the same test the point form applies */
                long ex = g->x[i] - g->x[k], ey = g->y[i] - g->y[k];
                if (ex < 0) ex = -ex;
                if (ey < 0) ey = -ey;
                if (ex > GRID_R || ey > GRID_R) continue;
                if (!(g->x[k] == g->x[i] + dx && g->y[k] == g->y[i] + dy)) continue;
                if (n < cap) out[n] = g->ent[k];
                n++;
            }
        }
    return n;
}

/* The MEASUREMENT form (#258): a value provider called from a rule body, so
 * the story writes the threshold — `grid_chebyshev(A, B) <= 3` — and the
 * exceptions stay where `why?` can reach them. Entity arguments arrive as
 * their interned atoms, the same identity the relation callbacks receive.
 *
 * An entity the grid has never heard of is INFINITELY far rather than -1: a
 * guard reads `<= n`, so a negative "undefined" would make every unknown
 * entity adjacent to everything. */
enum { GRID_FAR = 1 << 24 };

static long grid_fn(void *ctx, uint32_t pred, const long *args, int nargs)
{
    stock_grid *g = ctx;
    if (nargs != 2) return GRID_FAR;
    int d;
    if (pred == g->f_cheb)
        d = stock_grid_chebyshev(g, (uint32_t)args[0], (uint32_t)args[1]);
    else if (pred == g->f_manh)
        d = stock_grid_manhattan(g, (uint32_t)args[0], (uint32_t)args[1]);
    else return GRID_FAR;
    return d < 0 ? GRID_FAR : d;
}

long stock_grid_probes(const stock_grid *g) { return g ? g->probes : 0; }

stock_grid *stock_grid_install(world *w, intern *syms,
                               const uint32_t *ents, int nent)
{
    if (nent <= 0) return NULL;
    stock_grid *g = calloc(1, sizeof *g);
    g->w = w; g->syms = syms; g->nent = nent;
    g->ent = malloc((size_t)nent * sizeof *g->ent);
    g->xa  = malloc((size_t)nent * sizeof *g->xa);
    g->ya  = malloc((size_t)nent * sizeof *g->ya);
    g->ba  = malloc((size_t)nent * sizeof *g->ba);
    g->x   = malloc((size_t)nent * sizeof *g->x);
    g->y   = malloc((size_t)nent * sizeof *g->y);
    g->blocks = malloc((size_t)nent * sizeof *g->blocks);
    int have = 0;
    for (int i = 0; i < nent; i++) {
        char b[128];
        g->ent[i] = ents[i];
        snprintf(b, sizeof b, "grid_x(%s)", intern_name(syms, ents[i]));
        g->xa[i] = intern_id(syms, b);
        snprintf(b, sizeof b, "grid_y(%s)", intern_name(syms, ents[i]));
        g->ya[i] = intern_id(syms, b);
        snprintf(b, sizeof b, "grid_blocks(%s)", intern_name(syms, ents[i]));
        g->ba[i] = intern_id(syms, b);
        g->blocks[i] = false;
        have++;
    }
    if (!have) { stock_grid_free(g); return NULL; }
    g->nbucket = 1;
    while (g->nbucket < 2 * nent) g->nbucket *= 2;
    g->start = malloc((size_t)(g->nbucket + 1) * sizeof *g->start);
    g->item  = malloc((size_t)nent * sizeof *g->item);
    g->p_adj = intern_id(syms, "grid_adjacent");
    g->p_los = intern_id(syms, "grid_los");
    g->f_cheb = intern_id(syms, "grid_chebyshev");
    g->f_manh = intern_id(syms, "grid_manhattan");
    world_set_provider_fn(w, grid_point, g);
    world_set_fn_provider_fn(w, grid_fn, g);
    world_set_provider_gen_fn(w, g->p_adj, grid_gen, g);
    stock_grid_refresh(g);
    return g;
}

void stock_grid_free(stock_grid *g)
{
    if (!g) return;
    free(g->ent); free(g->xa); free(g->ya); free(g->ba); free(g->x); free(g->y);
    free(g->blocks); free(g->start); free(g->item);
    free(g);
}
