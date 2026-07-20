/* RTS-crowd benchmark: scalar grounded solve vs columnar family solve
 * (DESIGN.md 5.8 third backing). Deterministic; no rand()/wallclock in
 * construction.
 *
 *   ./bench_col [nents]      default sweep: 1000 10000 100000
 *
 * The family is a plausible per-unit AI judgment schema — 6 base fluents,
 * 4 derived, 10 rules covering chains, team-style competition, superiority,
 * a defeater, a strict rule, negative body literals:
 *
 *   enemy_near => alerted            has_orders & leader_alive => alerted
 *   alerted & in_range => engage     low_hp => ~engage   (beats the former)
 *   has_orders & in_range => engage
 *   alerted & ~in_range => advance   suppressed ~> ~advance
 *   low_hp & ~leader_alive => fallback
 *   has_orders => ~fallback          (beaten by the fallback rule)
 *   fallback -> ~advance
 *
 * Scalar baseline grounds N copies into one dl_theory and calls dl_solve
 * (compile included — that is the engine's per-recompute path today, as in
 * bench_dl). Columnar keeps one schema + fact columns and calls dlcol_solve.
 * Results are verified equal before timing. */

#include "logic/dl.h"
#include "logic/dl_col.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { EN, IR, LH, HO, SU, LA, AL, EG, AD, FB, NATOMS };
enum { NBASE = 6 };
static const char *aname[NATOMS] =
    { "en", "ir", "lh", "ho", "su", "la", "al", "eg", "ad", "fb" };

static uint32_t rng_state = 0xBEEFCAFEu;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }
static uint64_t xrand64(void) { return ((uint64_t)xrand() << 32) | xrand(); }

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

typedef struct {
    dl_rule_kind kind;
    int head_atom; int head_neg;
    int b_atom[2]; int b_neg[2]; int nb;
    int sup_over;                 /* rule index this rule beats, or -1 */
} srule;

static const srule SCHEMA[] = {
    { DL_DEFEASIBLE, AL, 0, { EN, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, AL, 0, { HO, LA }, { 0, 0 }, 2, -1 },
    { DL_DEFEASIBLE, EG, 0, { AL, IR }, { 0, 0 }, 2, -1 },
    { DL_DEFEASIBLE, EG, 1, { LH, -1 }, { 0, 0 }, 1,  2 },  /* low_hp beats engage */
    { DL_DEFEASIBLE, EG, 0, { HO, IR }, { 0, 0 }, 2, -1 },
    { DL_DEFEASIBLE, AD, 0, { AL, IR }, { 0, 1 }, 2, -1 },  /* alerted & ~in_range */
    { DL_DEFEATER,   AD, 1, { SU, -1 }, { 0, 0 }, 1, -1 },
    { DL_DEFEASIBLE, FB, 0, { LH, LA }, { 0, 1 }, 2, -1 },
    { DL_DEFEASIBLE, FB, 1, { HO, -1 }, { 0, 0 }, 1, -1 },
    { DL_STRICT,     AD, 1, { FB, -1 }, { 0, 0 }, 1, -1 },
};
enum { NRULES = (int)(sizeof SCHEMA / sizeof SCHEMA[0]) };
static const int SUP_EXTRA_W = 7, SUP_EXTRA_L = 8;   /* fallback beats orders */

static dl_lit mk(uint32_t atom, int neg) { dl_lit l = { atom, neg != 0 }; return l; }

/* per-entity base-fact bits, biased toward plausible battlefield rates */
static void gen_inputs(uint64_t *bits[NBASE], int W)
{
    for (int w = 0; w < W; w++) {
        uint64_t r1 = xrand64(), r2 = xrand64(), r3 = xrand64();
        bits[EN][w] = r1;                       /* ~50% enemy near      */
        bits[IR][w] = r1 & r2;                  /* ~25%, subset of near */
        bits[LH][w] = r2 & r3;                  /* ~25% low hp          */
        bits[HO][w] = r1 | r3;                  /* ~75% has orders      */
        bits[SU][w] = r1 & r2 & r3;             /* ~12% suppressed      */
        bits[LA][w] = r2 | r3;                  /* ~87% leader alive    */
    }
}

static int bench_one(int nents)
{
    int W = (nents + 63) / 64;
    uint64_t *bits[NBASE];
    for (int a = 0; a < NBASE; a++)
        bits[a] = malloc((size_t)W * sizeof(uint64_t));
    gen_inputs(bits, W);

    /* ---- columnar family ---- */
    dlcol *fam = dlcol_new(NATOMS, nents);
    int cid[NRULES];
    for (int r = 0; r < NRULES; r++) {
        const srule *s = &SCHEMA[r];
        dl_lit body[2];
        for (int i = 0; i < s->nb; i++)
            body[i] = mk((uint32_t)s->b_atom[i], s->b_neg[i]);
        cid[r] = dlcol_add_rule(fam, s->kind, mk((uint32_t)s->head_atom, s->head_neg),
                                body, s->nb);
    }
    for (int r = 0; r < NRULES; r++)
        if (SCHEMA[r].sup_over >= 0)
            dlcol_add_sup(fam, cid[r], cid[SCHEMA[r].sup_over]);
    dlcol_add_sup(fam, cid[SUP_EXTRA_W], cid[SUP_EXTRA_L]);
    for (int a = 0; a < NBASE; a++) {
        uint64_t *pos = dlcol_fact_row(fam, mk((uint32_t)a, 0));
        uint64_t *neg = dlcol_fact_row(fam, mk((uint32_t)a, 1));
        memcpy(pos, bits[a], (size_t)W * sizeof(uint64_t));
        for (int w = 0; w < W; w++)
            neg[w] = ~bits[a][w];               /* closed world */
    }

    /* ---- scalar grounding of the same N instances ---- */
    intern *sy = intern_new();
    dl_theory *t = dl_theory_new(sy);
    uint32_t *atom = malloc((size_t)nents * NATOMS * sizeof *atom);
    char buf[48];
    double t0 = now_ms();
    for (int e = 0; e < nents; e++)
        for (int a = 0; a < NATOMS; a++) {
            snprintf(buf, sizeof buf, "%s_%d", aname[a], e);
            atom[(size_t)e * NATOMS + a] = intern_id(sy, buf);
        }
    for (int e = 0; e < nents; e++) {
        const uint32_t *ea = atom + (size_t)e * NATOMS;
        int gid[NRULES];
        for (int r = 0; r < NRULES; r++) {
            const srule *s = &SCHEMA[r];
            dl_lit body[2];
            for (int i = 0; i < s->nb; i++)
                body[i] = mk(ea[s->b_atom[i]], s->b_neg[i]);
            snprintf(buf, sizeof buf, "r%d_%d", r, e);
            gid[r] = dl_add_rule(t, buf, s->kind, mk(ea[s->head_atom], s->head_neg),
                                 body, s->nb);
        }
        for (int r = 0; r < NRULES; r++)
            if (SCHEMA[r].sup_over >= 0)
                dl_add_sup(t, gid[r], gid[SCHEMA[r].sup_over]);
        dl_add_sup(t, gid[SUP_EXTRA_W], gid[SUP_EXTRA_L]);
        for (int a = 0; a < NBASE; a++) {
            int on = (bits[a][e / 64] >> (e % 64)) & 1;
            dl_add_fact(t, mk(ea[a], !on));
        }
    }
    double build_ms = now_ms() - t0;

    /* ---- verify agreement before timing ---- */
    dlcol_solve(fam);
    dl_result *res = dl_solve(t);
    int stride = nents <= 10000 ? 1 : 101, bad = 0;
    for (int e = 0; e < nents && bad < 5; e += stride) {
        const uint32_t *ea = atom + (size_t)e * NATOMS;
        for (int a = 0; a < NATOMS; a++)
            for (int neg = 0; neg < 2; neg++) {
                dl_lit sq = mk(ea[a], neg), cq = mk((uint32_t)a, neg);
                if (dl_defeasible(res, sq) != dlcol_defeasible(fam, cq, e) ||
                    dl_definite(res, sq) != dlcol_definite(fam, cq, e)) {
                    fprintf(stderr, "  MISMATCH entity %d atom %s neg %d\n",
                            e, aname[a], neg);
                    bad++;
                }
            }
    }
    dl_result_free(res);
    if (bad) {
        fprintf(stderr, "bench_col: verification FAILED at N=%d\n", nents);
        return 1;
    }

    /* ---- time both ---- */
    int s_iters = nents <= 1000 ? 50 : nents <= 10000 ? 15 : 5;
    int c_iters = 200;
    double *ms = malloc((size_t)(s_iters > c_iters ? s_iters : c_iters) * sizeof *ms);

    for (int i = 0; i < s_iters; i++) {
        double a = now_ms();
        dl_result *r = dl_solve(t);
        ms[i] = now_ms() - a;
        dl_result_free(r);
    }
    qsort(ms, (size_t)s_iters, sizeof *ms, cmp_double);
    double scalar_med = ms[s_iters / 2];

    for (int i = 0; i < c_iters; i++) {
        double a = now_ms();
        dlcol_solve(fam);
        ms[i] = now_ms() - a;
    }
    qsort(ms, (size_t)c_iters, sizeof *ms, cmp_double);
    double col_med = ms[c_iters / 2];

    printf("N=%-7d scalar %9.3f ms (build+intern %.0f ms, %d rules)   "
           "columnar %8.4f ms   speedup %.0fx\n",
           nents, scalar_med, build_ms, NRULES * nents, col_med,
           scalar_med / col_med);
    printf("           budget: columnar = %5.2f%% of a 60 Hz frame, "
           "%5.3f%% of a 10 Hz sim tick;  scalar = %.0f%% / %.1f%%\n",
           100.0 * col_med / 16.67, 100.0 * col_med / 100.0,
           100.0 * scalar_med / 16.67, 100.0 * scalar_med / 100.0);

    free(ms);
    free(atom);
    dl_theory_free(t);
    intern_free(sy);
    dlcol_free(fam);
    for (int a = 0; a < NBASE; a++)
        free(bits[a]);
    return 0;
}

int main(int argc, char **argv)
{
    printf("bench_col: per-unit AI family — %d atoms, %d rules "
           "(chains, superiority, defeater, strict), closed-world inputs\n",
           NATOMS, NRULES);
    if (argc > 1)
        return bench_one(atoi(argv[1]));
    static const int sweep[] = { 1000, 10000, 100000 };
    for (size_t i = 0; i < sizeof sweep / sizeof sweep[0]; i++)
        if (bench_one(sweep[i]))
            return 1;
    return 0;
}
