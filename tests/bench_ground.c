/* Synthetic grounding-cost benchmark (§5.2 "grounding is where cost hides,
 * not inference"; §8 M1 measurement debt; EPIC #26 child 1).
 *
 * The eager grounder (`ground_rule` in src/lang/story.c) materializes the
 * FULL nᵏ cross product of a rule's typed variables into world_add_rule via
 * the `decode_binding` odometer — §5.2 discipline 4 ("match at tick time
 * rather than pre-grounding the nᵏ cross product") is NOT implemented. This
 * measures what that eager path actually costs, with NO dependency on 5e
 * content or expressibility work: a synthetic worst case that maximizes the
 * cross product and sweeps it.
 *
 * It answers the decision gate the epic hinges on: how do compile time, ground
 * atoms, and peak memory scale with entity count and rule arity — and WHERE
 * does the MAX_INSTANCES (2²⁰) cliff land in entity-count terms? Past the cliff
 * the grounder now rejects the rule with a hard compile error (story_compile
 * returns NULL) rather than dropping it silently (#27); those rows show
 * rules=0 and, since the fix, a flat/cheap compile instead of a multi-GB spike.
 *
 *   ./bench_ground              full arity×N sweep
 *   ./bench_ground <arity> <N>  one point
 *
 * NOT a correctness test — it is not registered with ctest. Build Release (or
 * accept the -O2 this target forces) for meaningful numbers. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#define MAX_INSTANCES (1 << 20)      /* mirror the grounder's hard cap (story.c) */
#define CARD_WARN     100000         /* mirror the grounder's warn threshold     */

static double now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

/* peak resident set in KB (Linux ru_maxrss is KB) — a monotone high-water
 * mark, so the sweep runs smallest→largest and each row's figure is the peak
 * reached *through* that config. */
static long maxrss_kb(void)
{ struct rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_maxrss; }

/* Emit a synthetic .story that grounds one rule to exactly N^arity instances.
 * Anchoring bodies are `adj(...)` chains over a single `actor` sort of N
 * entities; the grounder walks the whole cross product regardless of how
 * sparse `adj` actually is (that sparsity is exactly what discipline 4 would
 * exploit and today does not). Returns a malloc'd string. */
static char *gen_story(int arity, int N)
{
    /* generous buffer: N identifiers "aNNNNNN," ~ 9 bytes each + fixed header */
    size_t cap = (size_t)N * 12 + 4096;
    char *s = malloc(cap);
    int n = 0;
    #define EMIT(...) do { n += snprintf(s + n, cap - (size_t)n, __VA_ARGS__); } while (0)

    EMIT("scene bench\nsort actor\nentity (\n");
    for (int i = 0; i < N; i++) EMIT("  a%d%s", i, i + 1 < N ? "," : " : actor\n");
    EMIT(")\n");

    /* Declare ONLY the body predicate each arity uses, so the config's cost is
     * dominated by the target N^arity and not contaminated by an unrelated
     * binary state grounding to N². The head `d(...)` is left UNDECLARED — a
     * judgment head interned lazily by ground_rule, exactly like real rule
     * heads (cellar.story's dead/weakened are not states either). */
    if (arity == 1) {
        EMIT("state ( cond(actor) )\n");
        EMIT("init ( cond(a0) )\n");
    } else {
        EMIT("state ( adj(actor, actor) )\n");
        if (N > 1) EMIT("init ( adj(a0, a1) )\n");
    }

    /* the rule: vars X0..X(arity-1), body chains adj to keep every var safe
     * (range-restricted over a finite relation, §5.2 discipline 1), head d(...).
     *   arity 1:  cond(X0)                       => d(X0)          N^1
     *   arity 2:  adj(X0,X1)                      => d(X0,X1)       N^2
     *   arity 3:  adj(X0,X1) & adj(X1,X2)         => d(X0,X1,X2)    N^3 */
    EMIT("rule blow(");
    for (int v = 0; v < arity; v++) EMIT("X%d: actor%s", v, v + 1 < arity ? ", " : "");
    EMIT("): ");
    if (arity == 1) EMIT("cond(X0)");
    else for (int v = 0; v + 1 < arity; v++) EMIT("%sadj(X%d, X%d)", v ? " & " : "", v, v + 1);
    EMIT(" => d(");
    for (int v = 0; v < arity; v++) EMIT("X%d%s", v, v + 1 < arity ? ", " : "");
    EMIT(")\n");

    #undef EMIT
    return s;
}

/* expected ground instances = N^arity, saturating past MAX_INSTANCES+1 so we
 * can print "would be" without overflow */
static double expected_instances(int arity, int N)
{ double p = 1; for (int v = 0; v < arity; v++) p *= (double)N; return p; }

static void bench_one(int arity, int N)
{
    char *src = gen_story(arity, N);
    double want = expected_instances(arity, N);

    /* min compile (grounding) time over fresh compiles; fewer reps as the
     * ground size grows (a 1M-instance grounding needn't run seven times) */
    int iters = want <= 1e4 ? 7 : want <= 1e5 ? 3 : 1;
    double best = 1e18;
    int njr = 0; uint32_t atoms = 0;
    for (int it = 0; it < iters; it++) {
        intern *syms = intern_new();
        double t = now_ms();
        world *w = story_compile(src, "bench", syms, NULL);
        double dt = now_ms() - t;
        if (dt < best) best = dt;            /* min = least-noisy grounding cost */
        if (w) {
            njr = world_judgment_rule_count(w);
            atoms = intern_count(syms);
            world_free(w);
        }
        intern_free(syms);
    }
    free(src);

    int dropped = (want > (double)MAX_INSTANCES);   /* grounder drops past the cap */
    long rss = maxrss_kb();

    printf("  arity %d  N=%-7d  want %11.0f inst  ", arity, N, want);
    if (dropped)
        printf("DROPPED (>%d cap): rules=%-8d  ", MAX_INSTANCES, njr);
    else
        printf("rules=%-8d atoms=%-8u  ", njr, atoms);
    printf("compile %8.2f ms  peakRSS %7.1f MB%s\n",
           best, rss / 1024.0,
           (!dropped && want > CARD_WARN) ? "  [card-warn]" : "");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* stream rows even when piped */
    printf("bench_ground: eager grounder cost — synthetic nᵏ cross product "
           "(§5.2/§8, EPIC #26)\n");
    printf("  MAX_INSTANCES cliff = %d (2^20); CARD_WARN = %d\n\n",
           MAX_INSTANCES, CARD_WARN);

    if (argc > 2) { bench_one(atoi(argv[1]), atoi(argv[2])); return 0; }

    /* Sweeps chosen to straddle the 2²⁰ cliff for each arity. Smallest→largest
     * so peakRSS is monotone and readable. */
    printf("arity 1 (N^1 — linear, never near the cliff):\n");
    int a1[] = { 1000, 10000, 100000, 500000 };
    for (size_t i = 0; i < sizeof a1 / sizeof *a1; i++) bench_one(1, a1[i]);

    printf("\narity 2 (N^2 — cliff at N=1025):\n");
    int a2[] = { 100, 300, 1000, 1024, 2000 };
    for (size_t i = 0; i < sizeof a2 / sizeof *a2; i++) bench_one(2, a2[i]);

    printf("\narity 3 (N^3 — cliff at N=102):\n");
    int a3[] = { 30, 64, 100, 128, 256 };
    for (size_t i = 0; i < sizeof a3 / sizeof *a3; i++) bench_one(3, a3[i]);

    printf("\nReading: 'DROPPED' rows are rules past the 2²⁰ cap — the grounder "
           "now rejects them\nwith a hard compile error (#27) instead of "
           "silently dropping them, at a flat cost\ninstead of the old N^arity "
           "spike; the M3 tick-time matcher (#26) is what will let\nthese "
           "un-anchored cross products ground affordably.\n");
    return 0;
}
