/* #120 (EPIC #117): how much of a lane-scale step is statically dead under
 * the current phase? This number GATES the `split` leaf (#121): per-phase
 * schema specialization gets built only if the hand-narrowed simulation
 * beats the honest baseline by >= 1.3x on the full-round total at 100k, on
 * a realistic workload (churn in every phase, no empty ticks — the
 * bench-the-honest-path rule).
 *
 *   ./bench_phase [nents]      default sweep: 1000 10000 100000
 *
 * The workload: one lane sort, 17 per-entity boolean fluent families with
 * realistically SKEWED per-phase write-sets —
 *
 *   phase     action-driven fams   ram target   gate (cross-phase read)
 *   declare   f0                   f1           f16  (cleanup's output)
 *   react     f2  f3               f4           f1   (declare's output)
 *   resolve   f5 .. f12            f13          f4   (react's output)
 *   cleanup   f14 f15              f16          f13  (resolve's output)
 *
 * Each action-driven family has a set and a clear action; each phase has a
 * complementary ramification pair (`ph & t(X)' & gate(X) causes r(X)` /
 * `ph & ~t(X)' & gate(X) causes ~r(X)`), so every phase both reads across
 * the round and writes both polarities — no monotone dead-ends, conflicts
 * impossible by construction.
 *
 * A — HONEST BASELINE: one world, the full schema, every rule guarded on a
 *     phase flag, stepped once per phase (4 steps = 1 round). This is what
 *     shipping code does today: all 17 families get fact-loaded, solved,
 *     and inertia-committed every step, whatever the phase. The phase is
 *     spelled as its boolean erasure (4 arity-0 globals) because the lane
 *     builder admits globals as broadcast reads but bails on MV — the MV
 *     spelling would knock A off the lane path entirely, so the erasure is
 *     the STRONGEST current baseline. The clock is driven by world_set
 *     between steps (global effects don't lane yet); that is bench
 *     instrumentation standing in for the advance ramifications, counted
 *     inside A's timed region.
 *
 * B — HAND-NARROWED SIMULATION of what the `split` compiler would build:
 *     four per-phase worlds, each holding ONLY that phase's live rules and
 *     touched fluents (no phase guards left — they are statically true).
 *     A fluent outside a phase's read/write set simply isn't in its world:
 *     inertia narrowing by construction. The one cross-phase gate family
 *     is copy-synced from its owner world before the step — counted inside
 *     B's timed region, which is CONSERVATIVE (the real split shares one
 *     fact store and copies nothing).
 *
 * Equivalence: A and the B-owner worlds must agree on every family after
 * every round of a seeded run — the narrowing is only interesting if it is
 * provably semantics-free.
 *
 * Reported per N: ms/step per phase and full-round total (A vs B, median
 * over rounds), % of A statically dead (1 - B/A), build time (A's one
 * compile vs B's K compiles — the split design pays K schema compiles),
 * arena bytes (A vs sum of B), and the GATE verdict against 1.3x. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NFAM   17
#define NPHASE 4
#define ROUNDS 9              /* odd: clean median; round 0 warms both paths */

typedef struct {
    const char *name;         /* the A-world phase flag (boolean erasure) */
    int nafam; int afam[8];   /* action-driven families (set+clear actions) */
    int trig, ram, gate;      /* ram pair: ph & [~]trig' & gate => [~]ram   */
} phase_cfg;

static const phase_cfg PH[NPHASE] = {
    { "ph_declare", 1, { 0 },                          0,  1,  16 },
    { "ph_react",   2, { 2, 3 },                       2,  4,  1  },
    { "ph_resolve", 8, { 5, 6, 7, 8, 9, 10, 11, 12 },  5,  13, 4  },
    { "ph_cleanup", 2, { 14, 15 },                     14, 16, 13 },
};

static uint32_t rng_state;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return v[n / 2];
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Emit one phase's actions and ramification pair. `guard` is the phase flag
 * conjunct for the A world, or NULL for a B world (statically specialized —
 * the guard is gone, not false). */
static int emit_phase(char *s, size_t cap, int off, const phase_cfg *p,
                      const char *guard)
{
    for (int k = 0; k < p->nafam; k++) {
        int f = p->afam[k];
        if (guard) {
            off += snprintf(s + off, cap - off,
                "action s%d(X: actor): requires %s causes f%d(X)\n"
                "action c%d(X: actor): requires %s causes ~f%d(X)\n",
                f, guard, f, f, guard, f);
        } else {
            off += snprintf(s + off, cap - off,
                "action s%d(X: actor): causes f%d(X)\n"
                "action c%d(X: actor): causes ~f%d(X)\n", f, f, f, f);
        }
    }
    off += snprintf(s + off, cap - off,
        "rule rp%d(X: actor): %s%sf%d(X)' & f%d(X) causes f%d(X)\n"
        "rule rn%d(X: actor): %s%s~f%d(X)' & f%d(X) causes ~f%d(X)\n",
        p->ram, guard ? guard : "", guard ? " & " : "", p->trig, p->gate, p->ram,
        p->ram, guard ? guard : "", guard ? " & " : "", p->trig, p->gate, p->ram);
    return off;
}

/* Build a world source. `phase` = -1 for the full A world (all phases,
 * guarded); 0..3 for a B world (that phase only, unguarded, its own
 * families + the gate family). */
static char *gen_source(int n, int phase)
{
    size_t cap = (size_t)n * 12 + 8192;
    char *s = malloc(cap);
    int off = snprintf(s, cap, "sort actor\nentity (");
    for (int e = 0; e < n; e++)
        off += snprintf(s + off, cap - off, "%su%d", e ? ", " : "", e);
    off += snprintf(s + off, cap - off, " : actor)\nstate (");
    if (phase < 0) {
        for (int p = 0; p < NPHASE; p++)
            off += snprintf(s + off, cap - off, " %s", PH[p].name);
        for (int f = 0; f < NFAM; f++)
            off += snprintf(s + off, cap - off, " f%d(actor)", f);
    } else {
        const phase_cfg *p = &PH[phase];
        for (int k = 0; k < p->nafam; k++)
            off += snprintf(s + off, cap - off, " f%d(actor)", p->afam[k]);
        off += snprintf(s + off, cap - off, " f%d(actor) f%d(actor)",
                        p->ram, p->gate);
    }
    off += snprintf(s + off, cap - off, " )\n");
    if (phase < 0)
        for (int p = 0; p < NPHASE; p++)
            off = emit_phase(s, cap, off, &PH[p], PH[p].name);
    else
        off = emit_phase(s, cap, off, &PH[phase], NULL);
    return s;
}

/* Ground-atom id tables, shared across all worlds via the one intern. */
typedef struct {
    uint32_t *fl[NFAM];              /* fl[f][e] = "f<f>(u<e>)"          */
    uint32_t *sa[NFAM], *ca[NFAM];   /* set/clear actions, a-driven fams */
    uint32_t ph[NPHASE];             /* the A-world phase flags          */
} atoms;

static void build_atoms(atoms *at, intern *sy, int n)
{
    char b[48];
    memset(at, 0, sizeof *at);
    for (int f = 0; f < NFAM; f++) {
        at->fl[f] = malloc((size_t)n * sizeof(uint32_t));
        for (int e = 0; e < n; e++) {
            snprintf(b, sizeof b, "f%d(u%d)", f, e);
            at->fl[f][e] = intern_id(sy, b);
        }
    }
    for (int p = 0; p < NPHASE; p++) {
        at->ph[p] = intern_id(sy, PH[p].name);
        for (int k = 0; k < PH[p].nafam; k++) {
            int f = PH[p].afam[k];
            at->sa[f] = malloc((size_t)n * sizeof(uint32_t));
            at->ca[f] = malloc((size_t)n * sizeof(uint32_t));
            for (int e = 0; e < n; e++) {
                snprintf(b, sizeof b, "s%d(u%d)", f, e);
                at->sa[f][e] = intern_id(sy, b);
                snprintf(b, sizeof b, "c%d(u%d)", f, e);
                at->ca[f][e] = intern_id(sy, b);
            }
        }
    }
}

static void free_atoms(atoms *at)
{
    for (int f = 0; f < NFAM; f++) { free(at->fl[f]); free(at->sa[f]); free(at->ca[f]); }
}

static world *compile_or_die(const char *src, intern *sy, const char *what)
{
    story_diag di[4];
    story_diags d = { di, 4, 0, 0 };
    world *w = story_compile(src, "bench_phase.story", sy, &d);
    if (!w) {
        fprintf(stderr, "bench_phase: %s failed to compile: %s\n", what,
                d.count ? di[0].msg : "?");
        exit(1);
    }
    return w;
}

static int bench_one(int n)
{
    intern *sy = intern_new();
    rng_state = 0xBEEFCAFEu;

    /* ---- build A (full schema) and the four B worlds ---- */
    double t0 = now_ms();
    char *src = gen_source(n, -1);
    world *wa = compile_or_die(src, sy, "A");
    free(src);
    double build_a = now_ms() - t0;

    world *wb[NPHASE];
    t0 = now_ms();
    for (int p = 0; p < NPHASE; p++) {
        src = gen_source(n, p);
        wb[p] = compile_or_die(src, sy, PH[p].name);
        free(src);
    }
    double build_b = now_ms() - t0;

    if (world_step_lane_family_count(wa) != 1) {
        fprintf(stderr, "bench_phase: A did not lane its transition at N=%d\n", n);
        return 1;
    }
    for (int p = 0; p < NPHASE; p++)
        if (world_step_lane_family_count(wb[p]) != 1) {
            fprintf(stderr, "bench_phase: B[%s] did not lane at N=%d\n",
                    PH[p].name, n);
            return 1;
        }

    atoms at;
    build_atoms(&at, sy, n);

    /* seed every family ~half-true, identically in A and each holder of the
     * family (owner + the one reader of a gate) */
    for (int f = 0; f < NFAM; f++)
        for (int e = 0; e < n; e++)
            if (xrand() & 1) {
                world_set(wa, at.fl[f][e], true);
                for (int p = 0; p < NPHASE; p++) {
                    const phase_cfg *pc = &PH[p];
                    bool holds = (f == pc->ram || f == pc->gate);
                    for (int k = 0; k < pc->nafam && !holds; k++)
                        holds = (pc->afam[k] == f);
                    if (holds) world_set(wb[p], at.fl[f][e], true);
                }
            }

    /* who owns (writes) each family — the authoritative B world */
    int owner[NFAM];
    for (int p = 0; p < NPHASE; p++) {
        for (int k = 0; k < PH[p].nafam; k++) owner[PH[p].afam[k]] = p;
        owner[PH[p].ram] = p;
    }

    /* ---- the seeded rounds: identical action stream through A and B ---- */
    uint32_t *acts = malloc((size_t)n * 8 * sizeof *acts + 1);
    double ta[NPHASE][ROUNDS], tb[NPHASE][ROUNDS];
    char err[128];
    int first_solve_a = 1, first_solve_b = 1;
    double schema_a = 0, schema_b = 0;

    for (int r = 0; r < ROUNDS; r++) {
        for (int p = 0; p < NPHASE; p++) {
            const phase_cfg *pc = &PH[p];

            /* churn: each entity sets/clears each of the phase's families
             * with prob 1/4 each (half the entities move per family) */
            int nacts = 0;
            for (int k = 0; k < pc->nafam; k++) {
                int f = pc->afam[k];
                for (int e = 0; e < n; e++) {
                    uint32_t v = xrand() & 3;
                    if (v == 0)      acts[nacts++] = at.sa[f][e];
                    else if (v == 1) acts[nacts++] = at.ca[f][e];
                }
            }

            /* A: flip the clock (stand-in for advance ramifications),
             * then one full-schema step */
            double a0 = now_ms();
            for (int q = 0; q < NPHASE; q++)
                world_set(wa, at.ph[q], q == p);
            if (world_step(wa, acts, nacts, err, sizeof err) != 0) {
                fprintf(stderr, "bench_phase: A step contested (%s)\n", err);
                return 1;
            }
            double a1 = now_ms();
            ta[p][r] = a1 - a0;
            if (first_solve_a) { schema_a = ta[p][r]; first_solve_a = 0; }

            /* B: sync the gate family from its owner world, then one
             * narrowed step — both inside the timed region (conservative:
             * the real split shares one store and copies nothing) */
            double b0 = now_ms();
            world *own = wb[owner[pc->gate]];
            for (int e = 0; e < n; e++)
                world_set(wb[p], at.fl[pc->gate][e],
                          world_get(own, at.fl[pc->gate][e]));
            if (world_step(wb[p], acts, nacts, err, sizeof err) != 0) {
                fprintf(stderr, "bench_phase: B[%s] step contested (%s)\n",
                        pc->name, err);
                return 1;
            }
            double b1 = now_ms();
            tb[p][r] = b1 - b0;
            if (first_solve_b) { schema_b = tb[p][r]; first_solve_b = 0; }
        }

        /* ---- equivalence: A vs the owner world, every family ---- */
        int stride = n <= 10000 ? 1 : 101, bad = 0;
        for (int f = 0; f < NFAM && bad < 5; f++)
            for (int e = 0; e < n && bad < 5; e += stride)
                if (world_get(wa, at.fl[f][e]) !=
                    world_get(wb[owner[f]], at.fl[f][e])) {
                    fprintf(stderr, "  MISMATCH round %d f%d(u%d): A=%d B=%d\n",
                            r, f, e, world_get(wa, at.fl[f][e]),
                            world_get(wb[owner[f]], at.fl[f][e]));
                    bad++;
                }
        if (bad) {
            fprintf(stderr, "bench_phase: NOT semantics-free at N=%d — "
                    "narrowing is disqualified\n", n);
            return 1;
        }
    }

    /* ---- report (medians skip the round-0 schema build by construction) -- */
    double atot = 0, btot = 0;
    printf("N=%d  (%d rounds; first A/B step includes schema build: "
           "%.0f / %.0f ms)\n", n, ROUNDS, schema_a, schema_b);
    for (int p = 0; p < NPHASE; p++) {
        double ma = median(ta[p], ROUNDS), mb = median(tb[p], ROUNDS);
        atot += ma; btot += mb;
        printf("  %-10s  A %9.3f ms   B %9.3f ms   %5.2fx\n",
               PH[p].name + 3, ma, mb, ma / mb);
    }
    printf("  %-10s  A %9.3f ms   B %9.3f ms   %5.2fx   "
           "statically dead: %.0f%% of A\n",
           "ROUND", atot, btot, atot / btot, 100.0 * (1.0 - btot / atot));
    printf("  build: A %.0f ms (1 compile)   B %.0f ms (%d compiles)   "
           "arena: A %.1f MB   B %.1f MB\n",
           build_a, build_b, NPHASE,
           (double)world_arena_bytes(wa) / 1e6,
           (double)(world_arena_bytes(wb[0]) + world_arena_bytes(wb[1]) +
                    world_arena_bytes(wb[2]) + world_arena_bytes(wb[3])) / 1e6);
    printf("  GATE (#121 builds only if >= 1.30x at 100k): %.2fx — %s\n",
           atot / btot, atot / btot >= 1.30 ? "PASS" : "FAIL");

    free(acts);
    free_atoms(&at);
    world_free(wa);
    for (int p = 0; p < NPHASE; p++) world_free(wb[p]);
    intern_free(sy);
    return 0;
}

int main(int argc, char **argv)
{
    printf("bench_phase (#120): full phase-guarded schema (A) vs hand-narrowed "
           "per-phase worlds (B)\n"
           "%d families, write-sets declare:2 react:3 resolve:9 cleanup:3; "
           "churn every phase\n", NFAM);
    if (argc > 1)
        return bench_one(atoi(argv[1]));
    static const int sweep[] = { 1000, 10000, 100000 };
    for (size_t i = 0; i < sizeof sweep / sizeof sweep[0]; i++)
        if (bench_one(sweep[i]))
            return 1;
    return 0;
}
