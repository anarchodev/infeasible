/* Differential coverage for the STEP-LANE FRONTIER (#165, §5.8/§8.1).
 *
 * `bench_lanes` prices which authoring shapes reach the lanes. This is the
 * correctness half of the same table: every shape that DOES lane must step
 * identically to the N=1 path, checked with world_step_lanes_check — which
 * solves the N=1 step family as an oracle and compares every fluent's
 * next-state verdict, per lane.
 *
 * The point is the coupling, not the individual cases. Lane coverage was
 * previously written per-feature, by hand, with a twin world built to bail;
 * a widening that landed without someone remembering to add a twin got no
 * differential at all. That is not hypothetical — test() reads in a numeric
 * RHS shipped that way and committed the wrong constant on every lane while
 * the whole suite passed.
 *
 * Here a shape that flips from bailing to laned starts being checked the
 * moment it flips, because `expect_lanes` is asserted and the differential
 * runs on exactly the laned rows. Retiring a bail therefore cannot silently
 * skip its own correctness proof: either the row is marked laned and gets
 * checked, or the assertion below fails and says so.
 *
 * Kept deliberately parallel to bench_lanes' VARIANTS table — same shapes,
 * same gate names — so the cost model and the correctness pin cannot drift
 * apart in what they think the frontier is. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

enum { NENT = 8 };

typedef struct {
    const char *name;
    const char *tail;
    bool        expect_lanes;
    const char *also;      /* an extra ground action cast every step, or NULL —
                            * for shapes whose action takes no unit argument and
                            * so cannot ride TRAJ's `hurt(u%d)` (#240) */
} variant;

/* Same shapes and the same order as bench_lanes' table. Every one defines
 * `hurt(T: unit)`; flags are assigned by index in gen_source below. */
static const variant VARIANTS[] = {
    { "const RHS, fluent guards",
      "action hurt(T: unit): requires ~resist_fire(T) & ~vuln_fire(T) & ~immune_fire(T)\n"
      "    causes hp(T) -= 7\n", true },

    { "non-const RHS",
      "action hurt(T: unit): requires ~immune_fire(T)\n"
      "    causes hp(T) -= inc_fire(T)\n", false },

    { "test() in RHS",
      "action hurt(T: unit): causes hp(T) -= 7 * (1 - test(immune_fire(T)))\n", true },

    { "test() ladder (3 reads)",
      "action hurt(T: unit): causes hp(T) -= (1 - test(immune_fire(T)))\n"
      "    * (7 + test(resist_fire(T)) * (1 - test(vuln_fire(T))) * (7 / 2 - 7)\n"
      "         + test(vuln_fire(T)) * (1 - test(resist_fire(T))) * 7)\n", true },

    { "judgment guard",
      "rule res(X: unit): resist_fire(X) => mitigated(X)\n"
      "action hurt(T: unit): requires ~mitigated(T)\n"
      "    causes hp(T) -= 7\n", false },

    { "accumulator + primed read",
      "rule zf(X: unit): on causes inc_fire(X) := 0\n"
      "action hurt(T: unit): causes inc_fire(T) += 7\n"
      "rule ap(X: unit): inc_fire(X)' >= 1 causes hp(X) -= inc_fire(X)'\n", false },

    /* boolean effects and a ramification reading a primed BOOLEAN (not numeric,
     * so no strata) — the shape test_lanes covers, kept here so the table is the
     * whole frontier rather than only its numeric half */
    { "boolean effect + ramification",
      "action hurt(T: unit): causes on_fire(T)\n"
      "rule spread(X: unit): on_fire(X)' causes panicked(X)\n", true },

    /* CASTERLESS binder casts (#240): a `for each` whose action takes no caster
     * — the setup/broadcast shape. One arity-0 cast atom drives the bcast local
     * that a per-caster cast drives once per caster. */
    { "casterless binder, const RHS",
      "action hurt(T: unit): causes hp(T) -= 7\n"
      "action sweep: causes for each X: unit where ~immune_fire(X) : hp(X) -= 3\n",
      true, "sweep" },

    { "casterless binder, per-item when",
      "action hurt(T: unit): causes hp(T) -= 7\n"
      "action sweep: causes for each X: unit where ~immune_fire(X) :\n"
      "    { hp(X) -= 3 when resist_fire(X), hp(X) -= 9 when vuln_fire(X) }\n",
      true, "sweep" },

    /* NUMERIC LANDMARK GUARDS (#242): `hp(X) >= n` in a body is one read-only
     * bit per lane, filled from the value store at fact-load. The thresholds sit
     * a few hits from the starting hp so the columns actually FLIP mid-trajectory
     * — a guard that never changes value would pass the differential without
     * having been tested. */
    { "numeric guard in an action's requires",
      "action hurt(T: unit): requires hp(T) >= 99990 causes hp(T) -= 7\n", true },

    { "numeric guard in a ramification body",
      "action hurt(T: unit): causes hp(T) -= 7\n"
      "rule bloodied(X: unit): hp(X) <= 99990 causes on_fire(X)\n", true },

    /* two thresholds on one fluent are two columns, not one (the #235 lesson
     * about keying by predicate alone, on the guard side) */
    { "two thresholds on one numeric fluent",
      "action hurt(T: unit): requires hp(T) >= 99980 causes hp(T) -= 7\n"
      "rule hurt1(X: unit): hp(X) <= 99993 causes on_fire(X)\n"
      "rule hurt2(X: unit): hp(X) <= 99986 causes panicked(X)\n", true },

    /* #241: a boolean binder item still bails. When that retires, flip this to
     * true and the differential below starts covering it. */
    { "casterless binder, BOOLEAN item",
      "action hurt(T: unit): causes hp(T) -= 7\n"
      "action sweep: causes for each X: unit where ~immune_fire(X) : on_fire(X)\n",
      false, "sweep" },
};
enum { NVARIANTS = (int)(sizeof VARIANTS / sizeof VARIANTS[0]) };

/* u_i: i%3==0 resist, i%5==0 vuln, i%7==0 immune — the same index rule
 * bench_lanes uses, so both read the same world.
 *
 * `bail` appends a 2-var action, which is never cast but disqualifies the whole
 * family — so the SAME shape can be compiled twice, once laned and once on the
 * N=1 oracle, without hand-authoring a second world per feature. That is what
 * makes this table self-extending: a newly-laned shape gets its twin for free. */
static char *gen_source(const char *tail, bool bail)
{
    size_t cap = 8192, off = 0;
    char *s = malloc(cap);
#define APP(...) off += (size_t)snprintf(s + off, cap - off, __VA_ARGS__)
    APP("sort unit\nentity (");
    for (int i = 0; i < NENT; i++) APP(" u%d%s", i, i + 1 < NENT ? "," : "");
    APP(" : unit )\n");
    APP("state (\n  on resist_fire(unit) vuln_fire(unit) immune_fire(unit)\n"
        "  on_fire(unit) panicked(unit)\n"
        "  hp(unit) : int in 0 .. 100000\n  inc_fire(unit) : int\n)\ninit ( on\n");
    for (int i = 0; i < NENT; i++) {
        APP("  hp(u%d)=100000", i);
        if (i % 3 == 0) APP(" resist_fire(u%d)", i);
        if (i % 5 == 0) APP(" vuln_fire(u%d)", i);
        if (i % 7 == 0) APP(" immune_fire(u%d)", i);
        APP("\n");
    }
    APP(")\n%s", tail);
    if (bail)
        APP("action pin(A: unit, B: unit): causes resist_fire(A)\n");
#undef APP
    return s;
}

/* every fluent the shared vocabulary declares, numerics included */
static int cmp_state(const char *name, int t, world *L, intern *sl,
                     world *N, intern *sn)
{
    static const char *NUMS[] = { "hp", "inc_fire" };
    static const char *BOOLS[] = { "resist_fire", "vuln_fire", "immune_fire",
                                   "on_fire", "panicked" };
    for (int u = 0; u < NENT; u++) {
        char b[48];
        for (size_t k = 0; k < sizeof NUMS / sizeof NUMS[0]; k++) {
            snprintf(b, sizeof b, "%s(u%d)", NUMS[k], u);
            long a = world_get_num(L, intern_id(sl, b));
            long c = world_get_num(N, intern_id(sn, b));
            if (a != c) {
                fprintf(stderr, "FAIL %s: step %d: %s lane=%ld n1=%ld\n",
                        name, t, b, a, c);
                return 1;
            }
        }
        for (size_t k = 0; k < sizeof BOOLS / sizeof BOOLS[0]; k++) {
            snprintf(b, sizeof b, "%s(u%d)", BOOLS[k], u);
            int a = world_get(L, intern_id(sl, b));
            int c = world_get(N, intern_id(sn, b));
            if (a != c) {
                fprintf(stderr, "FAIL %s: step %d: %s lane=%d n1=%d\n",
                        name, t, b, a, c);
                return 1;
            }
        }
    }
    return 0;
}

/* Trajectories are fixed, not random (I4): no target, one target, several at
 * once, and repeats so inertia and the clamp both get walked. */
static const int TRAJ[][3] = {
    { -1, -1, -1 },        /* pure inertia: nothing fires */
    {  1, -1, -1 },        /* an unflagged unit */
    {  0, -1, -1 },        /* resist + vuln + immune all at once */
    {  3,  5, -1 },        /* resist-only and vuln-only in one step */
    {  7, -1, -1 },        /* immune-only */
    {  1,  2,  4 },        /* three lanes at once */
    {  1, -1, -1 },        /* repeat: the second hit must agree too */
};
enum { NTRAJ = (int)(sizeof TRAJ / sizeof TRAJ[0]) };

static world *compile_variant(const variant *v, bool bail, intern *sy)
{
    char *src = gen_source(v->tail, bail);
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    free(src);
    if (!w || dg.nerrors) {
        fprintf(stderr, "FAIL %s: compile: %s\n", v->name,
                dg.count ? di[0].msg : "?");
        if (w) { world_free(w); w = NULL; }
    }
    return w;
}

static int run_variant(const variant *v)
{
    intern *sy = intern_new();
    world *w = compile_variant(v, false, sy);
    if (!w) { intern_free(sy); return 1; }

    bool laned = world_step_lane_family_count(w) > 0;
    if (laned != v->expect_lanes) {
        fprintf(stderr, "FAIL %s: expected lanes=%d, got %d.\n", v->name,
                (int)v->expect_lanes, (int)laned);
        fprintf(stderr, "  If a bail RETIRED, flip expect_lanes here (the "
                        "differential below then covers it) and update "
                        "bench_lanes' matching row.\n");
        world_free(w); intern_free(sy);
        return 1;
    }

    /* the N=1 twin, compiled from the same shape */
    intern *sn = intern_new();
    world *N = compile_variant(v, true, sn);
    if (!N) { world_free(w); intern_free(sy); intern_free(sn); return 1; }
    if (world_step_lane_family_count(N) != 0) {
        fprintf(stderr, "FAIL %s: the bail twin still lanes — it is not an "
                        "N=1 oracle\n", v->name);
        world_free(w); world_free(N); intern_free(sy); intern_free(sn);
        return 1;
    }

    int rc = 0;
    for (int t = 0; t < NTRAJ && rc == 0; t++) {
        uint32_t aL[4], aN[4];
        int na = 0;
        for (int j = 0; j < 3; j++) {
            if (TRAJ[t][j] < 0) continue;
            char b[32];
            snprintf(b, sizeof b, "hurt(u%d)", TRAJ[t][j]);
            aL[na] = intern_id(sy, b);
            aN[na] = intern_id(sn, b);
            na++;
        }
        if (v->also) {                 /* an argument-less cast (#240) */
            aL[na] = intern_id(sy, v->also);
            aN[na] = intern_id(sn, v->also);
            na++;
        }
        /* pin 1 — per-lane next-state verdicts against the N=1 step family,
         * before anything commits. BOOLEAN fluents only (see world.h), which is
         * why pin 2 exists rather than this being the whole story. */
        if (laned) {
            bool ok = true;
            int checks = world_step_lanes_check(w, aL, na, &ok);
            if (!ok || checks <= 0) {
                fprintf(stderr, "FAIL %s: step %d lane/N=1 verdicts differ "
                                "(ok=%d, checks=%d)\n", v->name, t, (int)ok, checks);
                rc = 1;
                break;
            }
        }
        char err[160];
        int rL = world_step(w, aL, na, err, sizeof err);
        int rN = world_step(N, aN, na, err, sizeof err);
        if (rL != rN || rL != 0) {
            fprintf(stderr, "FAIL %s: step %d: rc lane=%d n1=%d (%s)\n",
                    v->name, t, rL, rN, err);
            rc = 1;
            break;
        }
        /* pin 2 — committed state, NUMERIC VALUES INCLUDED. The numeric commit
         * runs outside the verdict columns, so pin 1 cannot see a wrong delta. */
        if (cmp_state(v->name, t, w, sy, N, sn))
            rc = 1;
    }
    world_free(w); world_free(N);
    intern_free(sy); intern_free(sn);
    return rc;
}

int main(void)
{
    int laned = 0;
    for (int i = 0; i < NVARIANTS; i++) {
        if (run_variant(&VARIANTS[i])) return 1;
        if (VARIANTS[i].expect_lanes) laned++;
    }
    /* the table is only worth anything if some rows actually reach the lanes */
    CHECK(laned >= 4);
    printf("test_lanefront: all passed (%d/%d shapes laned and differentially "
           "checked)\n", laned, NVARIANTS);
    return 0;
}
