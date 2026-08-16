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
 * and intersects. Same asymptotics, fewer passes, no per-axis machinery.
 *
 * TOPOLOGY is one enum and two small functions (`cell_dist`, `cell_corners`),
 * which is §5.6's claim that hex vs. square is the neighbour function inside
 * the provider, taken literally. The index, the ray walk, the generator and
 * the callbacks are shared: a square is two integer axes with eight
 * neighbours, a hex is two axial axes with six, and nothing else differs. */

enum { GRID_R = 1 };                 /* `adjacent` is one cell, either topology */

/* Sub-cell resolution for the ray walk. 6 divides by 2 (a square's corners sit
 * at half a cell) and by 3 (a hex corner is the mean of three hex centres), so
 * every sample point below is an exact integer and no ray needs a float. */
enum { SUB = 6 };

/* The six axial neighbour directions, in cyclic order — consecutive pairs are
 * the three hexes meeting at each corner. */
static const long HEX_DQ[6] = {  1,  1,  0, -1, -1,  0 };
static const long HEX_DR[6] = {  0, -1, -1,  0,  1,  1 };

typedef enum { TOPO_SQUARE, TOPO_HEX } topology;

struct stock_grid {
    world    *w;
    intern   *syms;
    topology  topo;
    int       nent;
    uint32_t *ent;                   /* [nent] entity atoms, install order */
    uint32_t *xa, *ya, *ba;          /* [nent] the position/blocker atoms */
    long     *x, *y;                 /* [nent] positions as of the last refresh */
    bool     *blocks;                /* [nent] is this entity a sight blocker */
    uint32_t  p_adj, p_los;
    uint32_t  f_cheb, f_manh, f_dist, f_occl;
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

/* ---- the topology ---------------------------------------------------------- */

static long labs_l(long v) { return v < 0 ? -v : v; }

/* The distance the topology calls its own, in whole cells: Chebyshev on a
 * square (a diagonal step costs one), the cube-coordinate distance on a hex.
 * `adjacent` is this thresholded at 1 in both. */
static long cell_dist(const stock_grid *g, long ax, long ay, long bx, long by)
{
    long dx = ax - bx, dy = ay - by;
    if (g->topo == TOPO_HEX)
        return (labs_l(dx) + labs_l(dy) + labs_l(dx + dy)) / 2;
    dx = labs_l(dx); dy = labs_l(dy);
    return dx > dy ? dx : dy;
}

/* The corners of a cell, in SUB units — the sample points an occlusion ray is
 * cast at. Four on a square, six on a hex: the target's outline, which is what
 * "how much of it is hidden" is a fraction of. */
static int cell_corners(const stock_grid *g, long u, long v, long *cx, long *cy)
{
    if (g->topo == TOPO_HEX) {
        for (int d = 0; d < 6; d++) {
            int e = (d + 1) % 6;
            /* a hex corner is the mean of the three hex centres meeting at it —
             * this one and two consecutive neighbours — which lands a third of
             * the way out along each of them, so scaling by SUB=6 leaves an
             * exact integer */
            cx[d] = u * SUB + (HEX_DQ[d] + HEX_DQ[e]) * (SUB / 3);
            cy[d] = v * SUB + (HEX_DR[d] + HEX_DR[e]) * (SUB / 3);
        }
        return 6;
    }
    static const long OX[4] = { -1, 1, 1, -1 }, OY[4] = { -1, -1, 1, 1 };
    for (int d = 0; d < 4; d++) {
        cx[d] = u * SUB + OX[d] * (SUB / 2);
        cy[d] = v * SUB + OY[d] * (SUB / 2);
    }
    return 4;
}

/* floor division and round-half-up over a positive denominator — integer, so
 * the sampling below has no float in it anywhere and replay is exact (I4). */
static long fdiv(long a, long b)
{
    long q = a / b, r = a % b;
    return (r != 0 && (r < 0) != (b < 0)) ? q - 1 : q;
}
static long rdiv(long a, long b) { return fdiv(2 * a + b, 2 * b); }

/* Which cell holds the point (px/den, py/den), where both are in SUB units.
 * The square rounds each axis; the hex rounds in cube coordinates and repairs
 * the coordinate that moved furthest, which is the standard reconstruction and
 * the only place the two topologies' arithmetic differs. Ties fall to
 * round-half-up in a fixed coordinate order — a tie-break, not a coin toss. */
static void point_cell(const stock_grid *g, long px, long py, long den,
                       long *ou, long *ov)
{
    if (g->topo != TOPO_HEX) { *ou = rdiv(px, den); *ov = rdiv(py, den); return; }
    long xn = px, zn = py, yn = -(px + py);
    long rx = rdiv(xn, den), ry = rdiv(yn, den), rz = rdiv(zn, den);
    if (rx + ry + rz != 0) {
        long dx = labs_l(rx * den - xn);
        long dy = labs_l(ry * den - yn);
        long dz = labs_l(rz * den - zn);
        if (dx > dy && dx > dz)  rx = -ry - rz;
        else if (dy > dz)        ry = -rx - rz;
        else                     rz = -rx - ry;
    }
    *ou = rx; *ov = rz;
}

/* ---- what stands between two entities -------------------------------------- */

/* Which sight blocker stands on this cell, if any (-1 for none)? The two
 * endpoints' own cells never block: an entity does not hide itself, and a
 * shooter does not hide behind their own square. Returning WHICH one rather
 * than merely whether is what lets the trace name it — §5.6's rule that a
 * provider must account for itself, or `why?` has a hole where the answer
 * came from. */
static int blocker_at(const stock_grid *g, long cx, long cy, int i, int j)
{
    if ((cx == g->x[i] && cy == g->y[i]) || (cx == g->x[j] && cy == g->y[j]))
        return -1;
    uint32_t b = cell_hash(cx, cy) & (uint32_t)(g->nbucket - 1);
    for (int s = g->start[b]; s < g->start[b + 1]; s++) {
        int k = g->item[s];
        /* the bucket is HASHED, so a collision puts unrelated cells in it —
         * the exact test is what makes the answer right */
        if (g->blocks[k] && g->x[k] == cx && g->y[k] == cy) return k;
    }
    return -1;
}

/* Walk the segment from `i`'s centre to the point (tx, ty) in SUB units, cell
 * by cell, and report the first blocker standing on one of them. The step is a
 * quarter-cell, so no cell the ray spends a whole crossing in can be skipped;
 * a ray that only clips a corner is not cover, which is the reading a player
 * expects and the one a sampled walk gives for free. */
static int ray_blocker(stock_grid *g, int i, int j, long tx, long ty)
{
    long sx = g->x[i] * SUB, sy = g->y[i] * SUB;
    long span = cell_dist(g, g->x[i], g->y[i], g->x[j], g->y[j]);
    long steps = 4 * span + 4;
    if (steps > 4096) steps = 4096;      /* a sight line across a continent */
    long den = SUB * steps;
    for (long s = 1; s < steps; s++) {
        long px = sx * steps + (tx - sx) * s;
        long py = sy * steps + (ty - sy) * s;
        long cx, cy;
        point_cell(g, px, py, den, &cx, &cy);
        int k = blocker_at(g, cx, cy, i, j);
        if (k >= 0) return k;
    }
    return -1;
}

static bool ray_blocked(stock_grid *g, int i, int j, long tx, long ty)
{
    return ray_blocker(g, i, j, tx, ty) >= 0;
}

/* Line of sight on the square: Bresenham from centre to centre, blocked by any
 * entity flagged `grid_blocks` on a cell it crosses. This is the MEASURED
 * premise (§5.6) — obscurement, darkvision, invisibility and blindsight are
 * exceptions the story layers on top, which is why there is no `can_see` here.
 * The hex answers the same question by walking the same centre-to-centre line
 * with `ray_blocked`, since axial coordinates have no Bresenham. */
static int los_blocker(stock_grid *g, int i, int j)
{
    if (g->topo == TOPO_HEX)
        return ray_blocker(g, i, j, g->x[j] * SUB, g->y[j] * SUB);
    long x0 = g->x[i], y0 = g->y[i], x1 = g->x[j], y1 = g->y[j];
    long dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    long dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1;
    long err = dx - dy, cx = x0, cy = y0;
    for (;;) {
        if (!(cx == x0 && cy == y0) && !(cx == x1 && cy == y1))
            for (int k = 0; k < g->nent; k++)
                if (g->blocks[k] && g->x[k] == cx && g->y[k] == cy) return k;
        if (cx == x1 && cy == y1) return -1;
        long e2 = 2 * err;
        if (e2 > -dy) { err -= dy; cx += sx; }
        if (e2 <  dx) { err += dx; cy += sy; }
    }
}

static bool los_clear(stock_grid *g, int i, int j) { return los_blocker(g, i, j) < 0; }

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

/* ---- the measurements ------------------------------------------------------ */

int stock_grid_chebyshev(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return -1;
    long dx = labs_l(g->x[i] - g->x[j]), dy = labs_l(g->y[i] - g->y[j]);
    return (int)(dx > dy ? dx : dy);
}

int stock_grid_manhattan(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return -1;
    return (int)(labs_l(g->x[i] - g->x[j]) + labs_l(g->y[i] - g->y[j]));
}

int stock_grid_distance(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return -1;
    return (int)cell_dist(g, g->x[i], g->y[i], g->x[j], g->y[j]);
}

bool stock_grid_los(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0 || i == j) return false;
    return los_clear(g, i, j);
}

/* Percent of the target's outline that blockers hide: one ray at each corner
 * of its cell, counted. Four corners on a square give 0/25/50/75/100 and six
 * on a hex give thirds and sixths — enough resolution for "half cover at 50,
 * three-quarters at 75" to be a line of story rather than a C ruling (§5.6).
 *
 * An entity the grid has never heard of is fully occluded rather than 0%: the
 * guard above it reads `>= n`, so a 0 would make an unplaced entity the one
 * everybody can see. Nothing occludes itself. */
int stock_grid_occlusion(stock_grid *g, uint32_t a, uint32_t b)
{
    ensure_fresh(g);
    int i = idx_of(g, a), j = idx_of(g, b);
    if (i < 0 || j < 0) return 100;
    if (i == j) return 0;
    long cx[6], cy[6];
    int n = cell_corners(g, g->x[j], g->y[j], cx, cy), hit = 0;
    for (int c = 0; c < n; c++) if (ray_blocked(g, i, j, cx[c], cy[c])) hit++;
    return hit * 100 / n;
}

/* ---- the provider callbacks ------------------------------------------------ */

static bool grid_point(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    stock_grid *g = ctx;
    if (nargs != 2) return false;
    ensure_fresh(g);
    int i = idx_of(g, args[0]), j = idx_of(g, args[1]);
    if (i < 0 || j < 0 || i == j) return false;
    if (pred == g->p_adj)
        return cell_dist(g, g->x[i], g->y[i], g->x[j], g->y[j]) <= GRID_R;
    if (pred == g->p_los) return los_clear(g, i, j);
    return false;
}

/* The account (§5.6): a provider that answers yes or no and explains nothing
 * puts a hole in the trace, and the author goes reading C to find out what the
 * engine believed. Strictly trace-time — it renders a verdict already reached,
 * so it cannot perturb one, and a run with it registered is the same run. */
static int grid_render(void *ctx, uint32_t pred, const uint32_t *args, int nargs,
                       bool holds, char *buf, size_t cap)
{
    stock_grid *g = ctx;
    if (nargs != 2) return 0;
    int i = idx_of(g, args[0]), j = idx_of(g, args[1]);
    if (i < 0 || j < 0)
        return snprintf(buf, cap, "%s is not on the grid",
                        intern_name(g->syms, args[i < 0 ? 0 : 1]));
    if (pred == g->p_adj)
        return snprintf(buf, cap, "%s %ld %s %d",
                        g->topo == TOPO_HEX ? "hex distance" : "chebyshev",
                        cell_dist(g, g->x[i], g->y[i], g->x[j], g->y[j]),
                        holds ? "<=" : ">", GRID_R);
    if (pred == g->p_los) {
        int k = los_blocker(g, i, j);
        if (k < 0)
            return snprintf(buf, cap, "nothing on the line from (%ld,%ld) to "
                            "(%ld,%ld)", g->x[i], g->y[i], g->x[j], g->y[j]);
        return snprintf(buf, cap, "%s blocks it at (%ld,%ld)",
                        intern_name(g->syms, g->ent[k]), g->x[k], g->y[k]);
    }
    return 0;
}

/* The generator (#254): walk the nine buckets around `a` rather than the sort.
 * Nine covers both topologies — a hex's six neighbours are a subset of the 3x3
 * axial box, and the exact test below drops the two corners that are two hexes
 * away. Order is the bucket layout's, which the engine canonicalises: a
 * rebuild that reorders a bucket must not move a die roll (I4). */
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
                if (cell_dist(g, g->x[i], g->y[i], g->x[k], g->y[k]) > GRID_R)
                    continue;
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
    uint32_t a = (uint32_t)args[0], b = (uint32_t)args[1];
    int d;
    if (pred == g->f_occl) return stock_grid_occlusion(g, a, b);
    if (pred == g->f_cheb)      d = stock_grid_chebyshev(g, a, b);
    else if (pred == g->f_manh) d = stock_grid_manhattan(g, a, b);
    else if (pred == g->f_dist) d = stock_grid_distance(g, a, b);
    else return GRID_FAR;
    return d < 0 ? GRID_FAR : d;
}

long stock_grid_probes(const stock_grid *g) { return g ? g->probes : 0; }

/* ---- installation ---------------------------------------------------------- */

static uint32_t atom_of(intern *syms, const char *fmt, const char *ent)
{
    char b[128];
    snprintf(b, sizeof b, fmt, ent);
    return intern_id(syms, b);
}

static stock_grid *install(world *w, intern *syms, const uint32_t *ents, int nent,
                           topology topo)
{
    if (nent <= 0) return NULL;
    const char *xf = topo == TOPO_HEX ? "hex_q(%s)"      : "grid_x(%s)";
    const char *yf = topo == TOPO_HEX ? "hex_r(%s)"      : "grid_y(%s)";
    const char *bf = topo == TOPO_HEX ? "hex_blocks(%s)" : "grid_blocks(%s)";

    stock_grid *g = calloc(1, sizeof *g);
    g->w = w; g->syms = syms; g->nent = nent; g->topo = topo;
    g->ent = malloc((size_t)nent * sizeof *g->ent);
    g->xa  = malloc((size_t)nent * sizeof *g->xa);
    g->ya  = malloc((size_t)nent * sizeof *g->ya);
    g->ba  = malloc((size_t)nent * sizeof *g->ba);
    g->x   = malloc((size_t)nent * sizeof *g->x);
    g->y   = malloc((size_t)nent * sizeof *g->y);
    g->blocks = malloc((size_t)nent * sizeof *g->blocks);
    int have = 0;
    for (int i = 0; i < nent; i++) {
        const char *nm = intern_name(syms, ents[i]);
        g->ent[i] = ents[i];
        g->xa[i] = atom_of(syms, xf, nm);
        g->ya[i] = atom_of(syms, yf, nm);
        g->ba[i] = atom_of(syms, bf, nm);
        g->blocks[i] = false;
        /* A position nobody declared reads 0, which would stand this entity on
         * the origin — and if nobody at all has one, the whole cast shares a
         * cell and every pair is adjacent. Refusing to install is the only
         * honest answer; the compiler catches the story-side case (#263). */
        if (world_has_num(w, g->xa[i]) || world_has_num(w, g->ya[i])) have++;
    }
    if (!have) { stock_grid_free(g); return NULL; }
    g->nbucket = 1;
    while (g->nbucket < 2 * nent) g->nbucket *= 2;
    g->start = malloc((size_t)(g->nbucket + 1) * sizeof *g->start);
    g->item  = malloc((size_t)nent * sizeof *g->item);
    if (topo == TOPO_HEX) {
        g->p_adj  = intern_id(syms, "hex_adjacent");
        g->p_los  = intern_id(syms, "hex_los");
        g->f_dist = intern_id(syms, "hex_distance");
        g->f_occl = intern_id(syms, "hex_occlusion");
    } else {
        g->p_adj  = intern_id(syms, "grid_adjacent");
        g->p_los  = intern_id(syms, "grid_los");
        g->f_cheb = intern_id(syms, "grid_chebyshev");
        g->f_manh = intern_id(syms, "grid_manhattan");
        g->f_occl = intern_id(syms, "grid_occlusion");
    }
    world_set_provider_fn(w, grid_point, g);
    world_set_provider_render_fn(w, grid_render, g);
    world_set_fn_provider_fn(w, grid_fn, g);
    world_set_provider_gen_fn(w, g->p_adj, grid_gen, g);
    stock_grid_refresh(g);
    return g;
}

stock_grid *stock_grid_install(world *w, intern *syms,
                               const uint32_t *ents, int nent)
{
    return install(w, syms, ents, nent, TOPO_SQUARE);
}

stock_grid *stock_hex_install(world *w, intern *syms,
                              const uint32_t *ents, int nent)
{
    return install(w, syms, ents, nent, TOPO_HEX);
}

void stock_grid_free(stock_grid *g)
{
    if (!g) return;
    free(g->ent); free(g->xa); free(g->ya); free(g->ba); free(g->x); free(g->y);
    free(g->blocks); free(g->start); free(g->item);
    free(g);
}
