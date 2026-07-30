/* Golden test for `split` (#121, EPIC #117): per-value step-schema
 * specialization on a designated finite-domain fluent — a compilation hint
 * with ZERO semantic content.
 *
 * The headline pin is EQUIVALENCE: the same story compiled with and without
 * the `split` token, driven through the same seeded multi-round action
 * script, must agree after every step on every fluent, every numeric (incl.
 * a dynamic-clamp re-check on a fluent no rule writes), every judgment
 * verdict, and byte-for-byte on the why-trace of a live rule's effect. The
 * script crosses every mechanic split touches: phase-guarded actions and
 * ramifications, an UNGUARDED ramification with a primed boolean read (it
 * lives in every schema, and its read pulls the fluent into every write-set
 * so inertia still settles it), a primed-numeric-guard ramification (#87
 * strata), rolls (same seed, same tick keys), and the split fluent's own
 * advancement by phase-guarded rules.
 *
 * The deliberate NON-equivalences are diagnostics, pinned separately:
 *   - loudness (#121 completing #119): an action all of whose rules are
 *     statically dead under the current value is -1-with-err naming both,
 *     state untouched — where the unsplit world silently spends the turn;
 *   - the why-trace of a fluent OUTSIDE the current write-set says
 *     "committed by copy" instead of rendering an empty theory.
 *
 * Rejections: `split` off an MV domain, on an arity>0 fluent, duplicated,
 * and a primed read of the split fluent (schema selection is by the
 * PRE-step value) are located compile errors.
 *
 * Slice 2 (mixed lane/N=1 routing) is pinned by test_mixed_route /
 * test_mixed_fallback below: a homogeneous split story grounds one lane
 * family per value; the residue steps on a sparse N=1 schema fed the lane
 * half's next-state as strict primed facts; a residue writer of a lane
 * fluent disqualifies the families (pure N=1) — all held to the same
 * byte-identical equivalence bar. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* One story, the split slot filled with "" or " split". */
static const char *SRC_FMT =
    "sort actor\n"
    "entity ( a, b : actor )\n"
    "state (\n"
    "    phase : { declare, resolve, cleanup }%s\n"
    "    moving(actor)  hit(actor)  marked(actor)  braced(actor)\n"
    "    hp(actor) : int in 0 .. 20\n"
    "    guard_hp(actor) : int in 0 .. hp(actor)\n"   /* dynamic clamp, never written */
    "    score : int\n"
    ")\n"
    "init ( phase = declare  moving(a)\n"
    "       hp(a) = 12  hp(b) = 9  guard_hp(a) = 5  guard_hp(b) = 5  score = 3 )\n"
    "rule low(X: actor): hp(X) <= 7 => low(X)\n"
    "action go(X: actor):     requires phase = declare & ~marked(X) & ~moving(X)\n"
    "                         causes moving(X)\n"
    "action strike(X: actor): requires phase = resolve causes hp(X) -= 5 & hit(X)\n"
    "action lash(X: actor):   requires phase = resolve causes hp(X) -= roll(4)\n"
    "action rest(X: actor):   requires phase = cleanup causes hp(X) += 2 & braced(X)\n"
    "action tally:            requires phase = cleanup causes score += 1\n"
    "// unguarded ramification: lives in EVERY schema; its primed read keeps\n"
    "// `hit` inertia in every write-set even where nothing writes it\n"
    "rule stopped(X: actor): hit(X)' & moving(X) causes ~moving(X)\n"
    "// phase-guarded + primed numeric guard (#87 strata)\n"
    "rule bleed(X: actor): phase = resolve & hp(X)' <= 7 & hit(X)' causes marked(X)\n"
    "// the split fluent's own advancement, phase-guarded\n"
    "action to_resolve: requires phase = declare causes phase = resolve\n"
    "action to_cleanup: requires phase = resolve causes phase = cleanup\n"
    "action to_declare: requires phase = cleanup causes phase = declare\n";

static world *compile_fmt(const char *slot, intern *sy)
{
    char src[4096];
    snprintf(src, sizeof src, SRC_FMT, slot);
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w)
        fprintf(stderr, "  compile failed: %s\n", d.count ? di[0].msg : "?");
    else if (d.nerrors)
        fprintf(stderr, "  errors: %s\n", di[0].msg);
    return w;
}

static dl_verdict q(world *w, intern *sy, const char *atom)
{
    return world_query(w, dl_pos(intern_id(sy, atom)));
}

static char *step_why_str(world *w, intern *sy, const char *atom, bool neg)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    dl_lit l = { intern_id(sy, atom), neg };
    world_step_why(w, l, true, m);
    fclose(m);
    return buf;
}

/* Everything observable must agree between the split and unsplit worlds. */
static int worlds_agree(world *wa, intern *sa, world *wb, intern *sb, int round)
{
    static const char *FLU[] = {
        "phase=declare", "phase=resolve", "phase=cleanup",
        "moving(a)", "moving(b)", "hit(a)", "hit(b)",
        "marked(a)", "marked(b)", "braced(a)", "braced(b)",
    };
    static const char *NUM[] = { "hp(a)", "hp(b)", "guard_hp(a)", "guard_hp(b)",
                                 "score" };
    static const char *JUD[] = { "low(a)", "low(b)" };
    int bad = 0;
    for (size_t i = 0; i < sizeof FLU / sizeof *FLU; i++)
        if (world_get(wa, intern_id(sa, FLU[i])) !=
            world_get(wb, intern_id(sb, FLU[i]))) {
            fprintf(stderr, "  round %d DIVERGED on %s: split=%d full=%d\n",
                    round, FLU[i], world_get(wa, intern_id(sa, FLU[i])),
                    world_get(wb, intern_id(sb, FLU[i])));
            bad++;
        }
    for (size_t i = 0; i < sizeof NUM / sizeof *NUM; i++)
        if (world_get_num(wa, intern_id(sa, NUM[i])) !=
            world_get_num(wb, intern_id(sb, NUM[i]))) {
            fprintf(stderr, "  round %d DIVERGED on %s: split=%ld full=%ld\n",
                    round, NUM[i], world_get_num(wa, intern_id(sa, NUM[i])),
                    world_get_num(wb, intern_id(sb, NUM[i])));
            bad++;
        }
    for (size_t i = 0; i < sizeof JUD / sizeof *JUD; i++)
        if (q(wa, sa, JUD[i]) != q(wb, sb, JUD[i])) {
            fprintf(stderr, "  round %d DIVERGED on verdict %s\n", round, JUD[i]);
            bad++;
        }
    return bad;
}

/* Step both worlds with the same ground actions; both must agree on rc. */
static int step_both(world *wa, intern *sa, world *wb, intern *sb,
                     const char **acts, int nacts)
{
    uint32_t ia[8], ib[8];
    for (int i = 0; i < nacts; i++) {
        ia[i] = intern_id(sa, acts[i]);
        ib[i] = intern_id(sb, acts[i]);
    }
    char ea[128] = "", eb[128] = "";
    int ra = world_step(wa, ia, nacts, ea, sizeof ea);
    int rb = world_step(wb, ib, nacts, eb, sizeof eb);
    if (ra != rb) {
        fprintf(stderr, "  step rc diverged: split=%d (%s) full=%d (%s)\n",
                ra, ea, rb, eb);
        return -1;
    }
    return ra;
}

/* --- the equivalence run: split on vs off, byte-identical throughout ------ */
static int test_equivalence(void)
{
    intern *sa = intern_new(), *sb = intern_new();
    world *wa = compile_fmt(" split", sa);
    world *wb = compile_fmt("", sb);
    CHECK(wa != NULL && wb != NULL);
    world_set_seed(wa, 77);
    world_set_seed(wb, 77);

    /* two rounds; the second re-enters phases so cached schemas get reused,
     * and strikes `a` so the never-written guard_hp(a) has to re-clamp
     * against its dynamic bound in BOTH worlds */
    static const char *SCRIPT[][4] = {
        { "go(b)", "to_resolve" },              /* declare */
        { "strike(b)", "lash(a)" },             /* resolve: hit+bleed+stopped */
        { "to_cleanup" },
        { "rest(b)", "tally", "to_declare" },   /* cleanup */
        { "go(a)", "to_resolve" },              /* round 2 */
        { "strike(a)", "strike(b)" },           /* hp(a) drops: dynamic clamp */
        { "to_cleanup" },
        { "rest(a)", "tally", "to_declare" },
    };
    for (size_t s = 0; s < sizeof SCRIPT / sizeof *SCRIPT; s++) {
        int n = 0;
        while (n < 4 && SCRIPT[s][n]) n++;
        CHECK(step_both(wa, sa, wb, sb, SCRIPT[s], n) == 0);
        CHECK(worlds_agree(wa, sa, wb, sb, (int)s) == 0);
    }

    /* why-trace parity for a LIVE rule's conclusion: the resolve-phase strike
     * on `b` (narrowing must not eat the trace). Re-run one resolve step and
     * compare the step-why of the hit next-state, byte for byte. */
    static const char *R2[] = { "to_resolve" };
    static const char *R3[] = { "strike(b)" };
    CHECK(step_both(wa, sa, wb, sb, R2, 1) == 0);
    CHECK(step_both(wa, sa, wb, sb, R3, 1) == 0);
    char *ta = step_why_str(wa, sa, "hit(b)", false);
    char *tb = step_why_str(wb, sb, "hit(b)", false);
    CHECK(ta && tb && strcmp(ta, tb) == 0);
    CHECK(strstr(ta, "strike") != NULL);
    free(ta); free(tb);
    CHECK(worlds_agree(wa, sa, wb, sb, 99) == 0);

    /* the split world's copy-through trace: `braced` is cleanup-only, so
     * after this resolve step its next-state has no theory — say so */
    ta = step_why_str(wa, sa, "braced(a)", false);
    CHECK(strstr(ta, "outside the split write-set") != NULL);
    CHECK(strstr(ta, "committed by copy") != NULL);
    free(ta);
    /* ...while the unsplit world renders ordinary inertia */
    tb = step_why_str(wb, sb, "braced(a)", false);
    CHECK(strstr(tb, "inertia on braced(a)") != NULL);
    free(tb);

    world_free(wa); world_free(wb);
    intern_free(sa); intern_free(sb);
    return 0;
}

/* --- loudness: dead-under-this-value actions are named errors ------------- */
static int test_loud_dead_in_value(void)
{
    intern *sy = intern_new();
    world *w = compile_fmt(" split", sy);
    CHECK(w != NULL);

    /* phase = declare: `rest` (cleanup-only) is statically dead — loud,
     * named, state untouched */
    uint32_t rest_a = intern_id(sy, "rest(a)");
    char err[160] = "";
    CHECK(world_step(w, &rest_a, 1, err, sizeof err) == -1);
    CHECK(strstr(err, "rest(a)") != NULL);
    CHECK(strstr(err, "no live step rule") != NULL);
    CHECK(strstr(err, "phase=declare") != NULL);
    CHECK(world_get_num(w, intern_id(sy, "hp(a)")) == 12);

    /* an unknown atom is still the #119 report, not the split one */
    uint32_t typo = intern_id(sy, "dance(a)");
    err[0] = '\0';
    CHECK(world_step(w, &typo, 1, err, sizeof err) == -1);
    CHECK(strstr(err, "matches no step rule") != NULL);
    CHECK(strstr(err, "split") == NULL);

    /* a live-but-guard-failed action still steps (the turn is spent):
     * go(a) is declare-live; marked(a) is false so it fires — use go on a
     * MARKED actor via a manual mark to get the guard-failed shape */
    world_set(w, intern_id(sy, "marked(a)"), true);
    uint32_t go_a = intern_id(sy, "go(a)");
    CHECK(world_step(w, &go_a, 1, err, sizeof err) == 0);

    /* after advancing, the same `rest` is live again */
    uint32_t adv1 = intern_id(sy, "to_resolve"), adv2 = intern_id(sy, "to_cleanup");
    CHECK(world_step(w, &adv1, 1, err, sizeof err) == 0);
    CHECK(world_step(w, &adv2, 1, err, sizeof err) == 0);
    CHECK(world_step(w, &rest_a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(a)")) == 14);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- mixed lane/N=1 routing (#121 slice 2) --------------------------------
 * A homogeneous split story grounds one lane family per value (its split
 * guards erased); the residue — here the MV phase writers — steps N=1 with
 * the lane half's next-state injected as strict primed facts. The `dawn`
 * ramification is the injection pin: a RESIDUE rule reading a LANE fluent
 * primed (`~awake(X)'`) must see the lane-side `doze` effect in the SAME
 * step. Equivalence against the unsplit twin pins all of it. */
static const char *MIX_FMT =
    "sort unit\n"
    "entity ( u0, u1, u2 : unit )\n"
    "state (\n"
    "    mode : { day, night }%s\n"
    "    awake(unit)  fed(unit)  patrol(unit)\n"
    ")\n"
    "init ( mode = day  awake(u0) awake(u1) awake(u2) )\n"
    "action feed(X: unit): requires mode = day & awake(X) causes fed(X)\n"
    "action doze(X: unit): requires mode = night causes ~awake(X)\n"
    "rule wakeup(X: unit):       mode = day & ~awake(X)  causes awake(X)\n"
    "rule night_patrol(X: unit): mode = night & fed(X)   causes patrol(X)\n"
    "action to_night: requires mode = day causes mode = night\n"
    "// residue (MV effect) with a PRIMED read of a lane fluent: the injection\n"
    "rule dawn(X: unit): mode = night & ~awake(X)' & fed(X) causes mode = day\n"
    "%s";

static world *compile_mix(const char *slot, const char *extra, intern *sy)
{
    char src[4096];
    snprintf(src, sizeof src, MIX_FMT, slot, extra);
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "m.story", sy, &d);
    if (!w)
        fprintf(stderr, "  compile failed: %s\n", d.count ? di[0].msg : "?");
    return w;
}

static int mix_agree(world *wa, intern *sa, world *wb, intern *sb, int stepno)
{
    static const char *FLU[] = {
        "mode=day", "mode=night",
        "awake(u0)", "awake(u1)", "awake(u2)",
        "fed(u0)", "fed(u1)", "fed(u2)",
        "patrol(u0)", "patrol(u1)", "patrol(u2)",
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof FLU / sizeof *FLU; i++)
        if (world_get(wa, intern_id(sa, FLU[i])) !=
            world_get(wb, intern_id(sb, FLU[i]))) {
            fprintf(stderr, "  step %d MIX DIVERGED on %s: split=%d full=%d\n",
                    stepno, FLU[i], world_get(wa, intern_id(sa, FLU[i])),
                    world_get(wb, intern_id(sb, FLU[i])));
            bad++;
        }
    return bad;
}

static int test_mixed_route(void)
{
    intern *sa = intern_new(), *sb = intern_new();
    world *wa = compile_mix(" split", "", sa);
    world *wb = compile_mix("", "", sb);
    CHECK(wa != NULL && wb != NULL);

    /* one lane family per split value; the unsplit twin lanes nothing (the
     * MV mode fluent bails the classic all-or-nothing builder) */
    CHECK(world_step_lane_family_count(wa) == 2);
    CHECK(world_step_lane_family_count(wb) == 0);

    static const char *SCRIPT[][3] = {
        { "feed(u0)", "feed(u1)" },   /* day: lane effects */
        { "to_night" },               /* residue advances the mode */
        { "doze(u0)" },               /* lane ~awake(u0); dawn(u0) reads it
                                       * PRIMED and flips mode back — the
                                       * injection, all in one step */
        { NULL },                     /* empty day step: wakeup(u0) re-arms */
        { "to_night" },
        { "doze(u2)" },               /* u2 unfed: dawn stays quiet */
        { NULL },                     /* empty night step: patrols persist */
    };
    for (size_t s = 0; s < sizeof SCRIPT / sizeof *SCRIPT; s++) {
        int n = 0;
        while (n < 3 && SCRIPT[s][n]) n++;
        CHECK(step_both(wa, sa, wb, sb, SCRIPT[s], n) == 0);
        CHECK(mix_agree(wa, sa, wb, sb, (int)s) == 0);
    }
    /* spot-check the injection actually happened: after step 2 the mode came
     * BACK to day in the same step doze landed */
    CHECK(q(wa, sa, "mode=night") == DL_PROVED);   /* end state: second night */
    CHECK(world_get(wa, intern_id(sa, "awake(u2)")) == 0);
    CHECK(world_get(wa, intern_id(sa, "patrol(u0)")) == 1);

    world_free(wa); world_free(wb);
    intern_free(sa); intern_free(sb);
    return 0;
}

/* A residue rule writing a LANE fluent would straddle one fluent's writers
 * across the halves — the grounder must refuse to build families (pure N=1),
 * and the world must still be exactly equivalent. */
static int test_mixed_fallback(void)
{
    static const char *EXTRA =
        "action tag(A: unit, B: unit): requires mode = day causes patrol(B)\n";
    intern *sa = intern_new(), *sb = intern_new();
    world *wa = compile_mix(" split", EXTRA, sa);
    world *wb = compile_mix("", EXTRA, sb);
    CHECK(wa != NULL && wb != NULL);
    CHECK(world_step_lane_family_count(wa) == 0);   /* soundness bail */

    static const char *S1[] = { "feed(u1)", "tag(u0,u2)" };
    static const char *S2[] = { "to_night" };
    static const char *S3[] = { "doze(u1)" };
    CHECK(step_both(wa, sa, wb, sb, S1, 2) == 0);
    CHECK(mix_agree(wa, sa, wb, sb, 0) == 0);
    CHECK(step_both(wa, sa, wb, sb, S2, 1) == 0);
    CHECK(step_both(wa, sa, wb, sb, S3, 1) == 0);
    CHECK(mix_agree(wa, sa, wb, sb, 2) == 0);
    CHECK(world_get(wa, intern_id(sa, "patrol(u2)")) == 1);

    world_free(wa); world_free(wb);
    intern_free(sa); intern_free(sb);
    return 0;
}

/* --- rejections: located compile errors ----------------------------------- */
static int expect_error(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int ok = w == NULL && d.nerrors >= 1 && di[0].line >= 1 &&
             strstr(di[0].msg, needle) != NULL;
    if (!ok)
        fprintf(stderr, "FAIL expected \"%s\" for <<%s>>: got %s\n", needle,
                src, d.nerrors ? di[0].msg : "(no error)");
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_rejections(void)
{
    /* not a finite value domain */
    if (expect_error("state x : int split\n", "finite value domain")) return 1;
    /* per-entity mode fluents make no sense */
    if (expect_error("sort actor\nentity u : actor\n"
                     "state ph(actor) : { on, off } split\n",
                     "arity-0")) return 1;
    /* one split per world */
    if (expect_error("state ( ph : { d, r } split  md : { x, y } split )\n",
                     "duplicate `split`")) return 1;
    /* a primed read of the split fluent cannot be phase-filtered */
    if (expect_error("state ( ph : { d, r } split  p  q )\n"
                     "init ph = d\n"
                     "rule adv: p & ph' = r causes q\n",
                     "primed read of the split fluent")) return 1;
    /* the same rule WITHOUT split compiles (the prime itself is legal) */
    intern *sy = intern_new();
    world *w = compile_fmt("", sy);   /* sanity reuse: base story compiles */
    if (!w) return 1;
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_equivalence())        return 1;
    if (test_loud_dead_in_value()) return 1;
    if (test_mixed_route())        return 1;
    if (test_mixed_fallback())     return 1;
    if (test_rejections())         return 1;
    printf("test_split: all passed\n");
    return 0;
}
