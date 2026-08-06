/* bench_lanes [nents] [ticks] — which authoring shapes reach the step lanes,
 * and what falling off them costs (EPIC #75, #165, DESIGN.md §5.8/§8.1).
 *
 * `emit_step_lanes` (src/lang/story.c) bails to the N=1 step on a list of
 * conditions, and the bail is WORLD-WIDE: one disqualifying construct anywhere
 * drops every family, not just the rules that used it. The conditions are
 * documented only as comments beside the early returns, so this benchmark makes
 * the frontier observable — one world, several variants, each differing in
 * exactly one property, reporting `world_step_lane_family_count` beside the
 * measured per-tick cost.
 *
 * Read it two ways. As a COST MODEL it prices each gate separately, so the
 * widening work can be ordered by what it actually buys. As a FRONTIER MARKER
 * it is a regression guard in reverse: a variant that flips from lanes=0 to
 * lanes=1 means a bail retired, which is the good news this file exists to
 * report and should not be discovered by accident.
 *
 * The world is fixed across variants — one sort, arity-1 boolean and numeric
 * fluents, one action driven at a fixed rate — so the differences below are the
 * gate under test and nothing else.
 *
 * Deterministic construction (I4): entity flags are assigned by index, not
 * randomly, and the driven targets rotate deterministically. Built -O2
 * regardless of build type; build Release for meaningful numbers. Not
 * registered with ctest. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Each variant supplies only the rules/actions; the vocabulary and the initial
 * state below are shared, so a variant differs in exactly one property. Every
 * one defines an action `hurt(T: unit)` that damages hp. */
typedef struct {
    const char *name;
    const char *gate;      /* the property under test */
    const char *tail;      /* the .story fragment */
    bool        expect_lanes;
} variant;

static const variant VARIANTS[] = {
    { "const RHS, fluent guards", "none — the laneable shape",
      "action hurt(T: unit): requires ~resist_fire(T) & ~vuln_fire(T) & ~immune_fire(T)\n"
      "    causes hp(T) -= 7\n", true },

    { "non-const RHS",           "num_eff_ok: RHS must constant-fold",
      "action hurt(T: unit): requires ~immune_fire(T)\n"
      "    causes hp(T) -= inc_fire(T)\n", false },

    { "test() in RHS",           "num_eff_ok: RHS must constant-fold",
      "action hurt(T: unit): causes hp(T) -= 7 * (1 - test(immune_fire(T)))\n", false },

    { "judgment guard",          "step_atom_ok: guards must be fluents",
      "rule res(X: unit): resist_fire(X) => mitigated(X)\n"
      "action hurt(T: unit): requires ~mitigated(T)\n"
      "    causes hp(T) -= 7\n", false },

    { "accumulator + primed read", "has_pguards: strata step one solve each",
      "rule zf(X: unit): on causes inc_fire(X) := 0\n"
      "action hurt(T: unit): causes inc_fire(T) += 7\n"
      "rule ap(X: unit): inc_fire(X)' >= 1 causes hp(X) -= inc_fire(X)'\n", false },
};
enum { NVARIANTS = (int)(sizeof VARIANTS / sizeof VARIANTS[0]) };

/* vocabulary + initial state shared by every variant */
static char *gen_source(const char *tail, int n)
{
    size_t cap = (size_t)n * 72 + 16384, off = 0;
    char *s = malloc(cap);
#define APP(...) off += (size_t)snprintf(s + off, cap - off, __VA_ARGS__)
    APP("sort unit\nentity (");
    for (int i = 0; i < n; i++)
        APP(" u%d%s", i, i + 1 < n ? "," : "");
    APP(" : unit )\n");
    APP("state (\n  on resist_fire(unit) vuln_fire(unit) immune_fire(unit)\n"
        "  hp(unit) : int in 0 .. 1000000\n  inc_fire(unit) : int\n)\ninit ( on\n");
    for (int i = 0; i < n; i++) {
        APP("  hp(u%d)=1000000", i);
        if (i % 3 == 0) APP(" resist_fire(u%d)", i);
        if (i % 5 == 0) APP(" vuln_fire(u%d)", i);
        if (i % 7 == 0) APP(" immune_fire(u%d)", i);
        APP("\n");
    }
    APP(")\n%s", tail);
#undef APP
    return s;
}

static double run(const variant *v, int n, int nacts, int ticks, int *lanes_out)
{
    char *src = gen_source(v->tail, n);
    intern *sy = intern_new();
    story_diag items[16];
    story_diags diags = { items, 16, 0, 0 };
    world *w = story_compile(src, "bench_lanes.story", sy, &diags);
    free(src);
    if (!w || diags.nerrors) {
        printf("  %-28s COMPILE FAILED: %s\n", v->name,
               diags.count ? items[0].msg : "?");
        intern_free(sy);
        *lanes_out = -1;
        return -1.0;
    }
    *lanes_out = world_step_lane_family_count(w);

    uint32_t *acts = malloc((size_t)nacts * sizeof *acts);
    char err[160], buf[48];
    double t0 = now_ms();
    for (int t = 0; t < ticks; t++) {
        for (int i = 0; i < nacts; i++) {
            snprintf(buf, sizeof buf, "hurt(u%d)", (t * nacts + i) % n);
            acts[i] = intern_id(sy, buf);
        }
        if (world_step(w, acts, nacts, err, sizeof err)) {
            printf("  %-28s STEP FAILED: %s\n", v->name, err);
            free(acts); world_free(w); intern_free(sy);
            return -1.0;
        }
    }
    double ms = (now_ms() - t0) / ticks;
    free(acts);
    world_free(w);
    intern_free(sy);
    return ms;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000;
    int ticks = argc > 2 ? atoi(argv[2]) : 5;
    int nacts = n / 20 > 1 ? n / 20 : 1;

    printf("bench_lanes: step-lane eligibility — N=%d, %d actions/tick, %d ticks\n",
           n, nacts, ticks);
    printf("  %-28s %-6s %-10s %s\n", "variant", "lanes", "tick", "gate under test");

    double laned = -1.0;
    for (int i = 0; i < NVARIANTS; i++) {
        int lanes = 0;
        double ms = run(&VARIANTS[i], n, nacts, ticks, &lanes);
        if (ms < 0)
            continue;
        bool got = lanes > 0;
        if (got && laned < 0)
            laned = ms;
        const char *mark = got == VARIANTS[i].expect_lanes ? "" :
            (got ? "  <== NEWLY LANED (a bail retired)" : "  <== REGRESSED (lost lanes)");
        printf("  %-28s %-6d %7.3f ms  %s%s\n",
               VARIANTS[i].name, lanes, ms, VARIANTS[i].gate, mark);
        if (!got && laned > 0)
            printf("  %-28s %-6s %6.1fx the laned shape\n", "", "", ms / laned);
    }

    printf("\nReading: the bail is world-wide — one disqualifying construct drops every\n"
           "family, so these costs are the whole step, not the affected rules. Three\n"
           "independent gates, any one of which is sufficient: a primed guard anywhere\n"
           "(has_pguards), a numeric effect whose RHS does not constant-fold\n"
           "(num_eff_ok/expr_fold — this also excludes every rolled amount, so no dice\n"
           "damage lanes today), and a step rule reading a judgment (step_atom_ok).\n"
           "A `<==` marker means the frontier MOVED: re-price the widening work in #165\n"
           "and update this table's expectations.\n");
    return 0;
}
