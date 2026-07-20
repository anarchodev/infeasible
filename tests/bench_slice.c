/* Vertical-slice RTS tick loop (DESIGN.md 5.8 / 5.6 / I4): the full
 * sense -> judge -> act -> step cycle at crowd scale, deterministic,
 * with per-phase timing against real frame budgets.
 *
 *   ./bench_slice [nunits] [nticks]     default 10000 units, 600 ticks
 *
 * Two armies march at each other and fight until the tick budget runs out.
 * Each tick:
 *
 *   provider  movement + uniform-grid spatial index (provider territory,
 *             DESIGN 5.6): nearest visible enemy, engagement ranges. Writes
 *             the base-fact columns (enemy_near, in_range, ...) closed-world.
 *   judge     one columnar defeasible solve of the per-unit AI family
 *             (bench_col's 10-rule schema: chains, superiority, defeater,
 *             strict). Behavior comes out as verdict columns.
 *   act+step  hosts read verdict columns (dlcol_proved_row) the way they
 *             wrote fact columns: engage-proved units deal damage (multiple
 *             attackers on one target SUM, 5.8's delta pipeline), advance/
 *             fallback drive movement. Then the step tier commits column-
 *             wise: hp -= damage (clamped), the low_hp landmark guard is
 *             re-derived from the threshold, suppression sets/decays, and
 *             two fluents update as word-wide inertia:
 *                 blooded' = blooded | damaged        (monotone inertia)
 *                 alive'   = alive  & ~died           (causal beats inertia)
 *
 * No allocation, wall-clock, or unseeded randomness inside the loop (I4):
 * the whole sim runs twice and the final state hashes must match — a save
 * here is literally (seed, nticks).
 *
 * This measures the world-model layer only. Pathfinding/steering are the
 * toy provider; a real RTS spends its budget there — the point is what the
 * *logic* layer costs beside it. */

#include "logic/dl_col.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { EN, IR, LH, HO, SU, LA, AL, EG, AD, FB, NATOMS };
enum { NBASE = 6 };

/* fixed-point world: CELL = one grid cell. The map scales with army size
 * (constant ~1.2 units/cell at spawn) so the toy provider's neighborhood
 * scans measure the same crowd density at every N; spawn bands sit at a
 * fixed frontage gap from the map's centerline so time-to-contact stays
 * roughly constant too. */
enum {
    CELL_SHIFT = 12, CELL = 1 << CELL_SHIFT,
    R_RANGE = CELL,                 /* weapons reach: 1 cell  (3x3 scan) */
    R_NEAR = 2 * CELL,              /* awareness: 2 cells     (5x5 scan) */
    SPEED = 1024,                   /* quarter cell per tick */
    HP_MAX = 100, HP_LOW = 30, DMG_HIT = 3, SUPP_TICKS = 8
};
static int grid_w, grid_h;          /* set from N in main */

static uint32_t rng_state;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

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

typedef struct {
    int n, W;
    /* provider state (plain arrays — the ECS components) */
    int32_t *x, *y;                 /* fixed-point position   */
    int32_t *hp;
    uint8_t *team, *supp;
    int32_t *target;                /* nearest enemy in range, -1 = none */
    int32_t *dmg;                   /* per-tick damage accumulator       */
    uint64_t *alive, *blooded, *damaged, *scratch;   /* host bit columns */
    /* spatial grid: intrusive lists, rebuilt per tick, no allocation */
    int32_t *cell_head, *nxt;
    /* per-team coarse occupancy (4x4 fine cells per coarse cell): lets a
     * unit skip the 5x5 fine scan entirely when no enemy is anywhere near —
     * i.e., every march tick, and every unit far from the front */
    int32_t *coarse[2];
    dlcol *fam;
} sim;

static const char *ANAMES[NATOMS] =
    { "enemy_near", "in_range", "low_hp", "has_orders",
      "suppressed", "leader_alive", "alerted", "engage", "advance", "fallback" };

static void build_family(sim *s)
{
    dlcol *f = dlcol_new(NATOMS, s->n);
    for (int a = 0; a < NATOMS; a++)
        dlcol_set_atom_name(f, (uint32_t)a, ANAMES[a]);
    dl_lit b[2];
    b[0] = mk(EN, 0);
    dlcol_add_rule(f, "near_alerts", DL_DEFEASIBLE, mk(AL, 0), b, 1);
    b[0] = mk(HO, 0); b[1] = mk(LA, 0);
    dlcol_add_rule(f, "orders_alert", DL_DEFEASIBLE, mk(AL, 0), b, 2);
    b[0] = mk(AL, 0); b[1] = mk(IR, 0);
    int r_eg = dlcol_add_rule(f, "alerted_engages", DL_DEFEASIBLE, mk(EG, 0), b, 2);
    b[0] = mk(LH, 0);
    int r_fl = dlcol_add_rule(f, "wounded_holds_back", DL_DEFEASIBLE, mk(EG, 1), b, 1);
    dlcol_add_sup(f, r_fl, r_eg);
    b[0] = mk(HO, 0); b[1] = mk(IR, 0);
    dlcol_add_rule(f, "ordered_engages", DL_DEFEASIBLE, mk(EG, 0), b, 2);
    b[0] = mk(AL, 0); b[1] = mk(IR, 1);
    dlcol_add_rule(f, "alerted_advances", DL_DEFEASIBLE, mk(AD, 0), b, 2);
    b[0] = mk(SU, 0);
    dlcol_add_rule(f, "suppression_pins", DL_DEFEATER, mk(AD, 1), b, 1);
    b[0] = mk(LH, 0); b[1] = mk(LA, 1);
    int r_fb = dlcol_add_rule(f, "wounded_leaderless_falls_back",
                              DL_DEFEASIBLE, mk(FB, 0), b, 2);
    b[0] = mk(HO, 0);
    int r_ho = dlcol_add_rule(f, "orders_hold_the_line", DL_DEFEASIBLE, mk(FB, 1), b, 1);
    dlcol_add_sup(f, r_fb, r_ho);
    b[0] = mk(FB, 0);
    dlcol_add_rule(f, "falling_back_stops_advance", DL_STRICT, mk(AD, 1), b, 1);
    s->fam = f;
}

static void sim_init(sim *s, int n, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    s->W = (n + 63) / 64;
    s->x = malloc((size_t)n * 4); s->y = malloc((size_t)n * 4);
    s->hp = malloc((size_t)n * 4);
    s->team = malloc((size_t)n); s->supp = calloc((size_t)n, 1);
    s->target = malloc((size_t)n * 4);
    s->dmg = malloc((size_t)n * 4);
    s->alive = calloc((size_t)s->W, 8);
    s->blooded = calloc((size_t)s->W, 8);
    s->damaged = calloc((size_t)s->W, 8);
    s->scratch = calloc((size_t)s->W, 8);
    s->cell_head = malloc((size_t)grid_w * grid_h * 4);
    s->nxt = malloc((size_t)n * 4);
    s->coarse[0] = malloc((size_t)(grid_w / 4) * (grid_h / 4) * 4);
    s->coarse[1] = malloc((size_t)(grid_w / 4) * (grid_h / 4) * 4);
    build_family(s);

    rng_state = seed;
    int half = n / 2;
    for (int i = 0; i < n; i++) {
        s->team[i] = (uint8_t)(i >= half);
        /* spawn bands 16 cells wide at a fixed frontage gap around the
         * centerline, so time-to-contact is N-independent */
        int32_t bx = (s->team[i] ? grid_w / 2 + 32 : grid_w / 2 - 48) * CELL;
        s->x[i] = bx + (int32_t)(xrand() % (16 * CELL));
        s->y[i] = (int32_t)(xrand() % (grid_h * CELL));
        s->hp[i] = HP_MAX;
        s->alive[i / 64] |= 1ull << (i % 64);
    }
}

static void sim_free(sim *s)
{
    dlcol_free(s->fam);
    free(s->x); free(s->y); free(s->hp); free(s->team); free(s->supp);
    free(s->target); free(s->dmg);
    free(s->alive); free(s->blooded); free(s->damaged); free(s->scratch);
    free(s->cell_head); free(s->nxt);
    free(s->coarse[0]); free(s->coarse[1]);
}

static int alive_bit(const sim *s, int i) { return (s->alive[i / 64] >> (i % 64)) & 1; }

/* ---- phase 1: provider — spatial index + base-fact columns ---- */

static void provider(sim *s)
{
    const int n = s->n, W = s->W;

    const int cw = grid_w / 4, ch = grid_h / 4;
    memset(s->cell_head, 0xff, (size_t)grid_w * grid_h * 4);
    memset(s->coarse[0], 0, (size_t)cw * ch * 4);
    memset(s->coarse[1], 0, (size_t)cw * ch * 4);
    for (int i = 0; i < n; i++) {
        if (!alive_bit(s, i))
            continue;
        int cx = s->x[i] >> CELL_SHIFT, cy = s->y[i] >> CELL_SHIFT;
        int c = cy * grid_w + cx;
        s->nxt[i] = s->cell_head[c];
        s->cell_head[c] = i;
        s->coarse[s->team[i]][(cy / 4) * cw + cx / 4]++;
    }

    /* leaders: unit 0 (team 0) and unit n/2 (team 1) */
    int lead_alive[2] = { alive_bit(s, 0), alive_bit(s, s->n / 2) };

    uint64_t *pos[NBASE];
    for (int a = 0; a < NBASE; a++) {
        pos[a] = dlcol_fact_row(s->fam, mk((uint32_t)a, 0));
        memset(pos[a], 0, (size_t)W * 8);
    }

    for (int i = 0; i < n; i++) {
        s->target[i] = -1;
        if (!alive_bit(s, i))
            continue;
        uint64_t bit = 1ull << (i % 64);
        int w = i / 64;
        /* non-spatial facts first — they hold with or without enemies */
        if (s->hp[i] < HP_LOW)              pos[LH][w] |= bit;
        if (lead_alive[s->team[i]])         pos[LA][w] |= bit;
        /* orders: team 0 always; team 1 only while its leader lives */
        if (s->team[i] == 0 || lead_alive[1]) pos[HO][w] |= bit;
        if (s->supp[i])                     pos[SU][w] |= bit;

        int cx = s->x[i] >> CELL_SHIFT, cy = s->y[i] >> CELL_SHIFT;
        /* coarse early-out: 3x3 coarse cells cover the whole R_NEAR disc */
        {
            const int32_t *enemy = s->coarse[1 - s->team[i]];
            int ccx = cx / 4, ccy = cy / 4, found = 0;
            for (int dy = -1; dy <= 1 && !found; dy++) {
                int yy = ccy + dy;
                if (yy < 0 || yy >= ch)
                    continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = ccx + dx;
                    if (xx >= 0 && xx < cw && enemy[yy * cw + xx]) {
                        found = 1;
                        break;
                    }
                }
            }
            if (!found)
                continue;           /* target stays -1, spatial bits stay 0 */
        }
        int64_t best = (int64_t)R_RANGE * R_RANGE;
        int64_t near2 = (int64_t)R_NEAR * R_NEAR;
        int tgt = -1, near = 0;
        /* capped neighbor query (what real engines do): melee fronts stack
         * hundreds of units per cell, and an uncapped nearest-enemy scan
         * goes quadratic there. Deterministic — grid traversal order is. */
        int budget = 48;
        for (int dy = -2; dy <= 2 && budget > 0; dy++) {
            int yy = cy + dy;
            if (yy < 0 || yy >= grid_h)
                continue;
            for (int dx = -2; dx <= 2 && budget > 0; dx++) {
                int xx = cx + dx;
                if (xx < 0 || xx >= grid_w)
                    continue;
                for (int j = s->cell_head[yy * grid_w + xx];
                     j >= 0 && budget > 0; j = s->nxt[j]) {
                    if (s->team[j] == s->team[i])
                        continue;
                    budget--;
                    int64_t ddx = s->x[j] - s->x[i], ddy = s->y[j] - s->y[i];
                    int64_t d2 = ddx * ddx + ddy * ddy;
                    if (d2 <= near2)
                        near = 1;
                    if (d2 <= best && (tgt < 0 || d2 < best || j < tgt)) {
                        best = d2;
                        tgt = j;
                    }
                }
            }
        }
        if (near)          pos[EN][w] |= bit;
        if (tgt >= 0)      pos[IR][w] |= bit;
        s->target[i] = tgt;
    }

    /* closed world, word-wide: ~f for every declared base fluent — but only
     * for the living; dead units get both rows false (out of the family). */
    for (int a = 0; a < NBASE; a++) {
        uint64_t *neg = dlcol_fact_row(s->fam, mk((uint32_t)a, 1));
        for (int w = 0; w < W; w++) {
            pos[a][w] &= s->alive[w];
            neg[w] = ~pos[a][w] & s->alive[w];
        }
    }
}

/* ---- phase 3: act on verdict columns, then commit the step tier ---- */

static void act_and_step(sim *s)
{
    const int n = s->n, W = s->W;
    const uint64_t *eg = dlcol_proved_row(s->fam, mk(EG, 0));
    const uint64_t *ad = dlcol_proved_row(s->fam, mk(AD, 0));
    const uint64_t *fb = dlcol_proved_row(s->fam, mk(FB, 0));

    memset(s->dmg, 0, (size_t)n * 4);
    for (int i = 0; i < n; i++) {
        if (!alive_bit(s, i))
            continue;
        uint64_t bit = 1ull << (i % 64);
        int w = i / 64;
        if ((eg[w] & bit) && s->target[i] >= 0) {
            s->dmg[s->target[i]] += DMG_HIT;      /* concurrent hits sum */
        } else if (fb[w] & bit) {
            s->x[i] += s->team[i] ? SPEED : -SPEED;
        } else if (ad[w] & bit) {
            if (s->target[i] >= 0) {              /* close on the target */
                int j = s->target[i];
                s->x[i] += s->x[j] > s->x[i] ? SPEED : -SPEED;
                s->y[i] += s->y[j] > s->y[i] ? SPEED : -SPEED;
            } else {
                s->x[i] += s->team[i] ? -SPEED : SPEED;
            }
        }
        if (s->x[i] < 0) s->x[i] = 0;
        if (s->x[i] >= grid_w * CELL) s->x[i] = grid_w * CELL - 1;
        if (s->y[i] < 0) s->y[i] = 0;
        if (s->y[i] >= grid_h * CELL) s->y[i] = grid_h * CELL - 1;
    }

    /* step-tier commit: damage pipeline, landmark re-derive, deaths */
    memset(s->damaged, 0, (size_t)W * 8);
    memset(s->scratch, 0, (size_t)W * 8);        /* died this tick */
    for (int i = 0; i < n; i++) {
        if (!alive_bit(s, i))
            continue;
        uint64_t bit = 1ull << (i % 64);
        int w = i / 64;
        if (s->dmg[i]) {
            s->hp[i] -= s->dmg[i];
            s->supp[i] = SUPP_TICKS;
            s->damaged[w] |= bit;
            if (s->hp[i] <= 0) {
                s->hp[i] = 0;
                s->scratch[w] |= bit;
            }
        } else {
            if (s->hp[i] < HP_MAX)
                s->hp[i]++;                       /* slow regen when unhurt */
            if (s->supp[i])
                s->supp[i]--;
        }
    }
    for (int w = 0; w < W; w++) {
        s->blooded[w] |= s->damaged[w];           /* f' = f | caused        */
        s->alive[w] &= ~s->scratch[w];            /* causal beats inertia   */
    }
}

static uint64_t state_hash(const sim *s)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < s->n; i++) {
        h = (h ^ (uint64_t)(uint32_t)s->x[i]) * 1099511628211ull;
        h = (h ^ (uint64_t)(uint32_t)s->y[i]) * 1099511628211ull;
        h = (h ^ (uint64_t)(uint32_t)s->hp[i]) * 1099511628211ull;
    }
    for (int w = 0; w < s->W; w++)
        h = (h ^ s->alive[w] ^ s->blooded[w]) * 1099511628211ull;
    return h;
}

static int popcnt_and(const uint64_t *a, const uint64_t *b, int W)
{
    int c = 0;
    for (int w = 0; w < W; w++)
        c += __builtin_popcountll(a[w] & b[w]);
    return c;
}

typedef struct { double prov, judge, act, total; } tick_ms;

static uint64_t run(int n, int nticks, uint32_t seed, tick_ms *times, int report)
{
    sim s;
    sim_init(&s, n, seed);
    uint64_t team1_off = 0;   /* silence unused in non-report runs */
    (void)team1_off;

    for (int t = 0; t < nticks; t++) {
        double t0 = now_ms();
        provider(&s);
        double t1 = now_ms();
        dlcol_solve(s.fam);
        double t2 = now_ms();
        act_and_step(&s);
        double t3 = now_ms();
        if (times) {
            times[t].prov = t1 - t0;
            times[t].judge = t2 - t1;
            times[t].act = t3 - t2;
            times[t].total = t3 - t0;
        }
        if (report && (t % 100 == 0 || t == nticks - 1)) {
            int a0 = 0, a1 = 0;
            for (int i = 0; i < s.n; i++)
                if (alive_bit(&s, i)) {
                    if (s.team[i]) a1++; else a0++;
                }
            int engaged = popcnt_and(dlcol_proved_row(s.fam, mk(EG, 0)),
                                     s.alive, s.W);
            printf("  tick %4d  alive %d/%d  engaged %d\n", t, a0, a1, engaged);
        }
    }

    if (report) {
        /* one live receipt: why is the first engaged unit engaging? */
        const uint64_t *eg = dlcol_proved_row(s.fam, mk(EG, 0));
        for (int i = 0; i < s.n; i++)
            if (alive_bit(&s, i) && ((eg[i / 64] >> (i % 64)) & 1)) {
                printf("\n");
                dlcol_why(s.fam, mk(EG, 0), i, stdout);
                break;
            }
    }

    uint64_t h = state_hash(&s);
    sim_free(&s);
    return h;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int nticks = argc > 2 ? atoi(argv[2]) : 600;
    int sc = 1;
    while (sc * sc * 10000 < n)
        sc++;                       /* map area scales with N: ~1.2 units/cell */
    grid_w = 128 * sc;
    grid_h = 64 * sc;
    printf("bench_slice: %d units, %d ticks, %dx%d-cell map — "
           "provider + columnar judge + columnar step\n",
           n, nticks, grid_w, grid_h);

    tick_ms *times = malloc((size_t)nticks * sizeof *times);
    uint64_t h1 = run(n, nticks, 0xC0FFEEu, times, 1);
    uint64_t h2 = run(n, nticks, 0xC0FFEEu, NULL, 0);   /* replay */
    printf("\nreplay hash: %s (%016llx)\n",
           h1 == h2 ? "MATCH" : "MISMATCH", (unsigned long long)h1);
    if (h1 != h2) {
        free(times);
        return 1;
    }

    double *tot = malloc((size_t)nticks * sizeof *tot);
    double sp = 0, sj = 0, sa = 0;
    for (int t = 0; t < nticks; t++) {
        tot[t] = times[t].total;
        sp += times[t].prov; sj += times[t].judge; sa += times[t].act;
    }
    qsort(tot, (size_t)nticks, sizeof *tot, cmp_double);
    double med = tot[nticks / 2], p99 = tot[(int)(nticks * 0.99)];
    printf("per tick: median %.3f ms  p99 %.3f ms   "
           "(provider %.3f + judge %.3f + act/step %.3f mean)\n",
           med, p99, sp / nticks, sj / nticks, sa / nticks);
    printf("budget:  %5.1f%% of a 30 Hz sim tick,  %5.1f%% of a 60 Hz frame\n",
           100.0 * med / 33.33, 100.0 * med / 16.67);
    free(tot);
    free(times);
    return 0;
}
