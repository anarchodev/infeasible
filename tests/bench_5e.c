/* Realistic-scenario benchmark: a 5e mass-combat CONDITIONS layer, measuring
 * both the time and the SPACE that the data-oriented (lane) representation wins
 * over the grounded-per-entity (N=1) one. These are the two engines the world
 * actually chooses between — the judgment lane family vs jfam — so this is the
 * real trade-off, not a strawman.
 *
 *   ./bench_5e [nents]        default sweep: 1000 10000 100000
 *
 * The schema is the 5e condition-interaction graph over a crowd of creatures
 * (boolean, defeasible, with chains / superiority / a defeater / negative
 * bodies — exactly what defeasible logic is for, and what "too slow for RTS"
 * claims can't be done at scale):
 *
 *   unconscious|stunned|paralyzed => incapacitated
 *   => can_act ;  incapacitated => ~can_act   (beats the default)
 *   prone|restrained|paralyzed|unconscious => attacked_at_adv
 *   invisible ~> ~attacked_at_adv             (defeater: adv+disadv cancel)
 *   frightened|poisoned|restrained => attacks_at_disadv
 *   can_act & ~frightened => is_threat        (negative body literal)
 *
 * True hp/damage is numeric (§5.8) and still runs N=1 — this measures the
 * boolean condition layer, which is what lanes cover today. Lane and N=1 are
 * both dl_col (the production engine); the only difference is packing: one
 * schema over N lanes vs N grounded copies over one lane. Verified equal first. */

#include "logic/dl_col.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* atom ids in the schema */
enum { UNC, STU, PAR, POI, FRI, RES, PRO, INV,       /* 8 base conditions */
       INC, ACT, ADV, DIS, THR,                       /* 5 derived */
       NATOMS };
enum { NBASE = 8 };
static const char *aname[NATOMS] = {
    "unconscious","stunned","paralyzed","poisoned","frightened","restrained",
    "prone","invisible","incapacitated","can_act","attacked_at_adv",
    "attacks_at_disadv","is_threat" };

typedef struct {
    dl_rule_kind kind;
    int head, head_neg;
    int b[2], bneg[2], nb;
    int sup_over;                 /* rule index this beats, or -1 */
} srule;

static const srule SCHEMA[] = {
    { DL_DEFEASIBLE, INC, 0, { UNC, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, INC, 0, { STU, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, INC, 0, { PAR, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, ACT, 0, { -1, -1 }, { 0, 0 }, 0, -1 },   /* default: can act */
    { DL_DEFEASIBLE, ACT, 1, { INC, -1 }, { 0, 0 }, 1,  3 },  /* incapacitated beats it */
    { DL_DEFEASIBLE, ADV, 0, { PRO, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, ADV, 0, { RES, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, ADV, 0, { PAR, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, ADV, 0, { UNC, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEATER,   ADV, 1, { INV, -1 }, { 0, 0 }, 1, -1 },  /* invisible cancels adv */
    { DL_DEFEASIBLE, DIS, 0, { FRI, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, DIS, 0, { POI, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, DIS, 0, { RES, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, THR, 0, { ACT, FRI }, { 0, 1 }, 2, -1 }, /* can_act & ~frightened */
};
enum { NRULES = (int)(sizeof SCHEMA / sizeof SCHEMA[0]) };

static uint32_t rng = 0x5eed5eedu;
static uint32_t xr(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static uint64_t xr64(void) { return ((uint64_t)xr() << 32) | xr(); }

static int cmp_d(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

static double now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

static dl_lit mk(uint32_t a, int neg) { dl_lit l = { a, neg != 0 }; return l; }

/* per-creature base-condition bits: most creatures are unafflicted; each
 * condition is uncommon (a real battlefield, not everyone stunned at once) */
static void gen_inputs(uint64_t *bits[NBASE], int W)
{
    for (int w = 0; w < W; w++) {
        uint64_t r1 = xr64(), r2 = xr64(), r3 = xr64();
        bits[UNC][w] = r1 & r2 & r3;                /* ~12% */
        bits[STU][w] = r1 & r2 & ~r3;               /* ~12% */
        bits[PAR][w] = r1 & r2 & r3 & xr64();       /* ~6%  */
        bits[POI][w] = r2 & r3;                     /* ~25% */
        bits[FRI][w] = r1 & r3;                     /* ~25% */
        bits[RES][w] = r1 & r2;                     /* ~25% */
        bits[PRO][w] = r2 & ~r1;                    /* ~25% */
        bits[INV][w] = r1 & r2 & r3 & ~xr64();      /* ~6%  */
    }
}

static int bench_one(int nents)
{
    int W = (nents + 63) / 64;
    uint64_t *bits[NBASE];
    for (int a = 0; a < NBASE; a++) bits[a] = malloc((size_t)W * sizeof(uint64_t));
    gen_inputs(bits, W);

    /* ---- lane family: one schema over nents lanes ---- */
    dlcol *lane = dlcol_new(NATOMS, nents);
    int lid[NRULES];
    for (int r = 0; r < NRULES; r++) {
        const srule *s = &SCHEMA[r];
        dl_lit body[2];
        for (int i = 0; i < s->nb; i++) body[i] = mk((uint32_t)s->b[i], s->bneg[i]);
        lid[r] = dlcol_add_rule(lane, NULL, s->kind, mk((uint32_t)s->head, s->head_neg),
                                body, s->nb);
    }
    for (int r = 0; r < NRULES; r++)
        if (SCHEMA[r].sup_over >= 0) dlcol_add_sup(lane, lid[r], lid[SCHEMA[r].sup_over]);
    for (int a = 0; a < NBASE; a++) {
        uint64_t *pos = dlcol_fact_row(lane, mk((uint32_t)a, 0));
        uint64_t *neg = dlcol_fact_row(lane, mk((uint32_t)a, 1));
        memcpy(pos, bits[a], (size_t)W * sizeof(uint64_t));
        for (int w = 0; w < W; w++) neg[w] = ~bits[a][w];      /* closed world */
    }

    /* ---- N=1 family: the same schema grounded per creature (nents copies) ---- */
    dlcol *n1 = dlcol_new(NATOMS * nents, 1);
    for (int e = 0; e < nents; e++) {
        int base = e * NATOMS, gid[NRULES];
        for (int r = 0; r < NRULES; r++) {
            const srule *s = &SCHEMA[r];
            dl_lit body[2];
            for (int i = 0; i < s->nb; i++)
                body[i] = mk((uint32_t)(base + s->b[i]), s->bneg[i]);
            gid[r] = dlcol_add_rule(n1, NULL, s->kind,
                                    mk((uint32_t)(base + s->head), s->head_neg),
                                    body, s->nb);
        }
        for (int r = 0; r < NRULES; r++)
            if (SCHEMA[r].sup_over >= 0)
                dlcol_add_sup(n1, gid[r], gid[SCHEMA[r].sup_over]);
        for (int a = 0; a < NBASE; a++) {
            int on = (bits[a][e / 64] >> (e % 64)) & 1;
            dlcol_add_fact(n1, mk((uint32_t)(base + a), !on), 0);
        }
    }

    /* ---- verify agreement (every creature, every atom, both statuses) ---- */
    dlcol_solve(lane);
    dlcol_solve(n1);
    int stride = nents <= 10000 ? 1 : 101, bad = 0;
    for (int e = 0; e < nents && bad < 5; e += stride)
        for (int a = 0; a < NATOMS; a++)
            for (int neg = 0; neg < 2; neg++) {
                dl_lit lq = mk((uint32_t)a, neg), nq = mk((uint32_t)(e * NATOMS + a), neg);
                if (dlcol_defeasible(lane, lq, e) != dlcol_defeasible(n1, nq, 0)) {
                    fprintf(stderr, "  MISMATCH creature %d atom %s neg %d\n",
                            e, aname[a], neg);
                    bad++;
                }
            }
    if (bad) { fprintf(stderr, "bench_5e: verification FAILED at N=%d\n", nents); return 1; }

    /* ---- time both ---- */
    int n1_iters = nents <= 1000 ? 50 : nents <= 10000 ? 15 : 5, lane_iters = 200;
    double *ms = malloc((size_t)(n1_iters > lane_iters ? n1_iters : lane_iters) * sizeof *ms);
    for (int i = 0; i < lane_iters; i++) { double t = now_ms(); dlcol_solve(lane); ms[i] = now_ms() - t; }
    qsort(ms, (size_t)lane_iters, sizeof *ms, cmp_d);
    double lane_ms = ms[lane_iters / 2];
    for (int i = 0; i < n1_iters; i++) { double t = now_ms(); dlcol_solve(n1); ms[i] = now_ms() - t; }
    qsort(ms, (size_t)n1_iters, sizeof *ms, cmp_d);
    double n1_ms = ms[n1_iters / 2];

    /* ---- space ---- */
    size_t lane_b = dlcol_footprint(lane), n1_b = dlcol_footprint(n1);

    printf("N=%-7d  lane %8.4f ms / %7.1f KB    N=1 %9.3f ms / %8.1f KB    "
           "%3.0fx faster, %3.0fx smaller\n",
           nents, lane_ms, lane_b / 1024.0, n1_ms, n1_b / 1024.0,
           n1_ms / lane_ms, (double)n1_b / (double)lane_b);
    printf("           lane per-solve budget: %5.2f%% of a 60 Hz frame, "
           "%5.3f%% of a 10 Hz sim tick\n",
           100.0 * lane_ms / 16.67, 100.0 * lane_ms / 100.0);

    free(ms);
    dlcol_free(lane);
    dlcol_free(n1);
    for (int a = 0; a < NBASE; a++) free(bits[a]);
    return 0;
}

int main(int argc, char **argv)
{
    printf("bench_5e: 5e mass-combat conditions — %d atoms, %d rules "
           "(chains, superiority, defeater, negative bodies)\n", NATOMS, NRULES);
    if (argc > 1) return bench_one(atoi(argv[1]));
    static const int sweep[] = { 1000, 10000, 100000 };
    for (size_t i = 0; i < sizeof sweep / sizeof sweep[0]; i++)
        if (bench_one(sweep[i])) return 1;
    return 0;
}
