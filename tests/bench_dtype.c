/* bench_dtype [N] [K] [T] — the #84 adoption gate (DESIGN §5.8 "One tick, in
 * order"): the CONFIGURED per-type commit stage (`as fire`, engine C) versus
 * the MODELED pipeline (per-tick transient accumulators + a stratum-1
 * response ramification over primed reads — pure .story). Semantics are
 * pinned equal by test_modeled; this measures the price of modeling.
 *
 * N units, K take fire damage per tick (event-shaped load), T ticks. Both
 * worlds step N=1 (typed and stratified worlds bail lanes alike), so the
 * comparison isolates the modeled pipeline's extra cost: one extra solve per
 * tick (2 strata) plus chain arithmetic, against the hardcoded C stage.
 * Build Release for meaningful numbers; not registered with ctest. */

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

static char *gen_source(int n, bool modeled)
{
    size_t cap = (size_t)n * 96 + 8192;
    char *s = malloc(cap);
    size_t off = 0;
#define APP(...) off += (size_t)snprintf(s + off, cap - off, __VA_ARGS__)
    APP("sort unit\nenum damage_type { fire, cold }\nentity (");
    for (int i = 0; i < n; i++) APP(" u%d%s", i, i + 1 < n ? "," : "");
    APP(" : unit )\n");
    APP("state (\n  on  raging(unit)  soaked(unit)  stoned(unit)\n"
        "  hp(unit) : int in 0 .. 1000000\n");
    if (modeled) APP("  inc_fire(unit) : int\n");
    APP(")\ninit ( on\n");
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
    if (!modeled) {
        APP("action burn(T: unit): causes hp(T) -= 7 as fire\n");
    } else {
        APP("rule zf(X: unit): on causes inc_fire(X) := 0\n"
            "action burn(T: unit): causes inc_fire(T) += 7\n"
            "rule apply_f(X: unit): inc_fire(X)' >= 1 causes\n"
            "    hp(X) -= (1 - test(immune(X, fire)))\n"
            "             * (inc_fire(X)'\n"
            "                + test(resistant(X, fire)) * (1 - test(vulnerable(X, fire)))\n"
            "                  * (inc_fire(X)' / 2 - inc_fire(X)')\n"
            "                + test(vulnerable(X, fire)) * (1 - test(resistant(X, fire)))\n"
            "                  * inc_fire(X)')\n");
    }
#undef APP
    return s;
}

static int run(int n, int k, int t, bool modeled, double *compile_ms,
               double *tick_ms, long *hp0_out)
{
    char *src = gen_source(n, modeled);
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    double c0 = now_ms();
    world *w = story_compile(src, "b.story", sy, &d);
    *compile_ms = now_ms() - c0;
    free(src);
    if (!w || d.nerrors) {
        fprintf(stderr, "compile failed: %s\n", d.count ? di[0].msg : "?");
        return 1;
    }
    uint32_t *acts = malloc((size_t)k * sizeof *acts);
    char err[128], b[48];
    double s0 = now_ms();
    for (int tick = 0; tick < t; tick++) {
        for (int i = 0; i < k; i++) {
            snprintf(b, sizeof b, "burn(u%d)", (tick * k + i) % n);
            acts[i] = intern_id(sy, b);
        }
        if (world_step(w, acts, k, err, sizeof err)) {
            fprintf(stderr, "step failed: %s\n", err);
            return 1;
        }
    }
    *tick_ms = (now_ms() - s0) / t;
    snprintf(b, sizeof b, "hp(u%d)", 1);           /* u1: no flags -> raw hits */
    *hp0_out = world_get_num(w, intern_id(sy, b));
    free(acts);
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int k = argc > 2 ? atoi(argv[2]) : n / 20;
    int t = argc > 3 ? atoi(argv[3]) : 10;
    if (k < 1) k = 1;

    double cc, ct, mc, mt;
    long hc, hm;
    if (run(n, k, t, false, &cc, &ct, &hc)) return 1;
    if (run(n, k, t, true, &mc, &mt, &hm)) return 1;
    printf("bench_dtype N=%d K=%d T=%d\n", n, k, t);
    printf("  configured: compile %8.1f ms   tick %10.3f ms\n", cc, ct);
    printf("  modeled:    compile %8.1f ms   tick %10.3f ms\n", mc, mt);
    printf("  modeled/configured tick ratio: %.2fx\n", ct > 0 ? mt / ct : 0.0);
    printf("  equivalence spot-check hp(u1): configured=%ld modeled=%ld %s\n",
           hc, hm, hc == hm ? "OK" : "MISMATCH");
    return hc == hm ? 0 : 1;
}
