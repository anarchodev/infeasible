/* bench_dtype [N] [K] [T] [S] — the modeled typed-damage pipeline under
 * event-shaped load (DESIGN §5.8 "One tick, in order"): per-tick transient
 * accumulators + a stratum-1 response ramification over primed reads, pure
 * .story. This is the only typed-damage path.
 *
 * N units, K burn actions per tick, T ticks, S sources per target. The
 * accumulator only earns its name when a target takes more than one
 * contribution of a type in a tick, so the load is shaped as K/S targets each
 * hit by S distinct burn actions — a fold of width S, which is what separates
 * per-type summation from per-attack response (two 3s resisted are
 * floor(6/2)=3, not 1+1). K is the action volume, held constant as S varies.
 *
 * Stratified worlds bail lanes, so this measures the N=1 step. Build Release
 * for meaningful numbers; not registered with ctest. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static char *gen_source(int n, int nsrc)
{
    size_t cap = (size_t)n * 96 + (size_t)nsrc * 64 + 8192;
    char *s = malloc(cap);
    size_t off = 0;
#define APP(...) off += (size_t)snprintf(s + off, cap - off, __VA_ARGS__)
    APP("sort unit\nenum damage_type { fire, cold }\nentity (");
    for (int i = 0; i < n; i++) APP(" u%d%s", i, i + 1 < n ? "," : "");
    APP(" : unit )\n");
    APP("state (\n  on  raging(unit)  soaked(unit)  stoned(unit)\n"
        "  hp(unit) : int in 0 .. 1000000\n"
        "  inc_fire(unit) : int\n"
        ")\ninit ( on\n");
    for (int i = 0; i < n; i++) {
        APP("  hp(u%d)=1000000", i);
        if (i % 3 == 0) APP(" raging(u%d)", i);
        if (i % 5 == 0) APP(" soaked(u%d)", i);
        if (i % 7 == 0) APP(" stoned(u%d)", i);
        APP("\n");
    }
    APP(")\n");
    APP("rule r_f(X: unit): raging(X) => resistant(X, fire)\n"
        "rule v_f(X: unit): soaked(X) => vulnerable(X, fire)\n"
        "rule i_f(X: unit): stoned(X) => immune(X, fire)\n");
    APP("rule zf(X: unit): on causes inc_fire(X) := 0\n");
    for (int j = 0; j < nsrc; j++)
        APP("action burn%d(T: unit): causes inc_fire(T) += 7\n", j + 1);
    APP("rule apply_f(X: unit): inc_fire(X)' >= 1 causes\n"
        "    hp(X) -= (1 - test(immune(X, fire)))\n"
        "             * (inc_fire(X)'\n"
        "                + test(resistant(X, fire)) * (1 - test(vulnerable(X, fire)))\n"
        "                  * (inc_fire(X)' / 2 - inc_fire(X)')\n"
        "                + test(vulnerable(X, fire)) * (1 - test(resistant(X, fire)))\n"
        "                  * inc_fire(X)')\n");
#undef APP
    return s;
}

/* how many ticks the rotation lands on `unit` */
static int hits_for(int unit, int t, int ntgt, int n)
{
    int h = 0;
    for (int tick = 0; tick < t; tick++)
        for (int i = 0; i < ntgt; i++)
            if (((long)tick * ntgt + i) % n == unit) h++;
    return h;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int k = argc > 2 ? atoi(argv[2]) : n / 20;   /* burn actions per tick */
    int t = argc > 3 ? atoi(argv[3]) : 10;
    int s = argc > 4 ? atoi(argv[4]) : 2;        /* sources per target */
    if (s < 1) s = 1;
    if (k < s) k = s;
    int ntgt = k / s;                            /* targets per tick */
    if (ntgt > n) ntgt = n;                      /* no target twice in a tick */
    int nact = ntgt * s;

    char *src = gen_source(n, s);
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    double c0 = now_ms();
    world *w = story_compile(src, "b.story", sy, &d);
    double compile_ms = now_ms() - c0;
    free(src);
    if (!w || d.nerrors) {
        fprintf(stderr, "compile failed: %s\n", d.count ? di[0].msg : "?");
        return 1;
    }
    /* action ids resolved up front: the tick loop times the step, not snprintf */
    char err[128], b[48];
    uint32_t *acts = malloc((size_t)nact * sizeof *acts);
    uint32_t *ids = malloc((size_t)n * (size_t)s * sizeof *ids);
    for (int u = 0; u < n; u++)
        for (int j = 0; j < s; j++) {
            snprintf(b, sizeof b, "burn%d(u%d)", j + 1, u);
            ids[(size_t)u * s + j] = intern_id(sy, b);
        }
    double s0 = now_ms();
    for (int tick = 0; tick < t; tick++) {
        for (int i = 0; i < ntgt; i++) {
            long u = ((long)tick * ntgt + i) % n;
            for (int j = 0; j < s; j++)
                acts[(size_t)i * s + j] = ids[(size_t)u * s + j];
        }
        if (world_step(w, acts, nact, err, sizeof err)) {
            fprintf(stderr, "step failed: %s\n", err);
            return 1;
        }
    }
    double tick_ms = (now_ms() - s0) / t;

    printf("bench_dtype N=%d K=%d T=%d S=%d\n", n, nact, t, s);
    printf("  fold: %d targets x %d sources = %d actions/tick\n", ntgt, s, nact);
    printf("  modeled: compile %8.1f ms   tick %10.3f ms\n", compile_ms, tick_ms);

    /* u1 has no flags: the raw sum. u3 is raging -> resistant, so its drop is
     * floor(7S/2) if the response reads the tick's per-type SUM, and S*floor(7/2)
     * if it were applied per attack — the two differ from S=2 on. */
    if (n > 1) {
        long hp1 = world_get_num(w, intern_id(sy, "hp(u1)"));
        long e1 = 1000000L - (long)hits_for(1, t, ntgt, n) * 7 * s;
        printf("  spot-check raw       hp(u1) = %7ld (expect %7ld)%s\n",
               hp1, e1, hp1 == e1 ? "" : "  MISMATCH");
    }
    if (n > 3) {
        long hp3 = world_get_num(w, intern_id(sy, "hp(u3)"));
        int h3 = hits_for(3, t, ntgt, n);
        long e3 = 1000000L - (long)h3 * (7 * s / 2);
        long per_attack = 1000000L - (long)h3 * s * (7 / 2);
        printf("  spot-check resistant hp(u3) = %7ld (expect %7ld;"
               " per-attack would be %7ld)%s\n",
               hp3, e3, per_attack, hp3 == e3 ? "" : "  MISMATCH");
    }
    free(ids);
    free(acts);
    world_free(w);
    intern_free(sy);
    return 0;
}
