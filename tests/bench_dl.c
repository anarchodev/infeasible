/* Micro-benchmark for the defeasible solver. Deterministic synthetic theory
 * (no rand/wallclock in construction); reports median solve time.
 *
 *   ./bench_dl [natoms] [fanin] [iters] [driver]   driver: sweep (default) | wl
 *
 * The theory is a wide layered graph: each atom in layer L is concluded by
 * `fanin` defeasible rules whose bodies draw from layer L-1, plus a competing
 * rule for its negation and a superiority edge — exercising supported /
 * countered / team-defeat paths, and the by-head + superiority indices. */

#include "logic/dl.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>

/* deterministic LCG so runs are reproducible and construction has no rand() */
static uint32_t rng_state = 0x1234567u;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int natoms = argc > 1 ? atoi(argv[1]) : 4000;
    int fanin  = argc > 2 ? atoi(argv[2]) : 4;
    int iters  = argc > 3 ? atoi(argv[3]) : 200;
    bool use_wl = argc > 4 && strcmp(argv[4], "wl") == 0;
    dl_result *(*solve)(dl_theory *) = use_wl ? dl_solve_wl : dl_solve;
    int layer  = natoms / 20 > 1 ? natoms / 20 : 1;  /* atoms per layer */

    intern *sy = intern_new();
    char buf[32];
    uint32_t *atom = malloc((size_t)natoms * sizeof *atom);
    for (int i = 0; i < natoms; i++) {
        snprintf(buf, sizeof buf, "a%d", i);
        atom[i] = intern_id(sy, buf);
    }

    /* Build the theory once; solve it `iters` times (dl_solve compiles the
     * theory into its indexed form on every call — that is what we measure). */
    dl_theory *t = dl_theory_new(sy);
    for (int i = 0; i < layer; i++)          /* layer 0 are base facts */
        dl_add_fact(t, dl_pos(atom[i]));

    int name_ctr = 0;
    for (int i = layer; i < natoms; i++) {
        dl_lit body[8];
        int nb = fanin < 8 ? fanin : 8;
        for (int k = 0; k < nb; k++) {
            int src = (int)(xrand() % (uint32_t)i);   /* some earlier atom */
            body[k] = dl_pos(atom[src]);
        }
        snprintf(buf, sizeof buf, "r%d", name_ctr++);
        int r_for = dl_add_rule(t, buf, DL_DEFEASIBLE, dl_pos(atom[i]), body, nb);
        /* a competing rule for the negation, beaten by the supporting rule */
        int src = (int)(xrand() % (uint32_t)i);
        dl_lit ab = dl_pos(atom[src]);
        snprintf(buf, sizeof buf, "n%d", name_ctr++);
        int r_against = dl_add_rule(t, buf, DL_DEFEASIBLE, dl_neg(atom[i]), &ab, 1);
        dl_add_sup(t, r_for, r_against);
    }

    /* warm up */
    for (int w = 0; w < 3; w++) { dl_result *r = solve(t); dl_result_free(r); }

    double *ms = malloc((size_t)iters * sizeof *ms);
    for (int it = 0; it < iters; it++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        dl_result *r = solve(t);
        clock_gettime(CLOCK_MONOTONIC, &b);
        ms[it] = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        dl_result_free(r);
    }
    qsort(ms, (size_t)iters, sizeof *ms, cmp_double);
    double total = 0; for (int i = 0; i < iters; i++) total += ms[i];

    printf("bench_dl[%s]: atoms=%d fanin=%d rules=%d sups=%d iters=%d\n", use_wl?"wl":"sweep",
           natoms, fanin, 2 * (natoms - layer), natoms - layer, iters);
    printf("  median %.3f ms   min %.3f ms   mean %.3f ms\n",
           ms[iters / 2], ms[0], total / iters);

    free(ms); free(atom);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}
