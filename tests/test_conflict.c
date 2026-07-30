/* Golden test for the conflictable-pair compile check (#98, hardened to
 * step-side ERRORS by #160; EPIC #154).
 *
 * Template-level, two sides at two severities:
 *
 * STEP (the contested `-1`s made static): two writers whose effects can land
 * on one ground atom with conflicting content — complementary booleans,
 * different multi-valued values, merge-less `:=` — are ERRORS (#160) unless
 * their conditions provably exclude co-firing (complementary literal,
 * different MV constants, disjoint comparison intervals, disjoint #95
 * membership lists), a #159 `exclusive` group covers them, or the collision
 * itself is impossible (distinct entity constants). A writer can also
 * collide with ITSELF when a scope variable is missing from the effect's
 * arguments and the assigned content varies with the binding.
 * Acceptance: a story that COMPILES cannot take the contested step paths.
 *
 * JUDGMENT (the silently-REFUTED null made loud) stays WARNING severity —
 * contested judgments are defined, sometimes intended semantics, and their
 * hazard is answered by visibility: complementary concluding
 * rules warn when nothing decides the conflict — no `>` covering edge (a
 * teammate's edge counts: team defeat's static shadow; band-desugared edges
 * count: they live in p->sups), no exclusive bodies. Strict-vs-defeasible is
 * decided (strict wins, quiet); strict-vs-strict can never be ordered and
 * always warns; defeaters are excluded (a blocked head is UNDECIDED by
 * intent). */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* Compile and count WARNING diagnostics containing `frag`; -1 = compile
 * error. Step-side conflicts are errors since #160 — asserted via nerr(). */
static int nwarn(const char *src, const char *frag)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int n = -1;
    if (w && d.nerrors == 0) {
        n = 0;
        for (int i = 0; i < d.count && i < d.cap; i++)
            if (strstr(di[i].msg, frag)) n++;
    } else if (d.count) {
        fprintf(stderr, "  compile: %s\n", di[0].msg);
    }
    if (w) world_free(w);
    intern_free(sy);
    return n;
}

/* Does the source FAIL to compile with an error containing `frag`? */
static int nerr(const char *src, const char *frag)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int hit = 0;
    if (w == NULL && d.nerrors > 0) {
        for (int i = 0; i < d.count && i < d.cap; i++)
            if (strstr(di[i].msg, frag)) hit = 1;
    }
    if (w) world_free(w);
    intern_free(sy);
    return hit;
}

static const char *HDR =
    "sort actor\n"
    "entity ( a : actor  b : actor )\n"
    "state ( open  locked  hp(actor) : int  total : int\n"
    "        mode : { calm, alert }  score : int merge max )\n";

static int test_step_boolean(void)
{
    char src[2048];
    /* unguarded complementary pair: warns */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action jam:  causes open\n"
             "action shut: causes ~open\n");
    CHECK(nerr(src, "conflicting effects on 'open'") == 1);
    /* complementary requires exclude co-firing: quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action jam:  requires ~open causes open\n"
             "action shut: requires open  causes ~open\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    /* same polarity never conflicts */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action jam:  causes open\n"
             "action jam2: causes open\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    /* disjoint numeric guard intervals exclude */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action jam:  requires hp(a) <= 3 causes open\n"
             "action shut: requires hp(a) >= 5 causes ~open\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    /* overlapping intervals do not */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action jam:  requires hp(a) <= 5 causes open\n"
             "action shut: requires hp(a) >= 5 causes ~open\n");
    CHECK(nerr(src, "conflicting effects on 'open'") == 1);
    /* ramification vs action: same analysis */
    snprintf(src, sizeof src, "%s%s", HDR,
             "rule spread: locked causes open\n"
             "action shut: causes ~open\n");
    CHECK(nerr(src, "conflicting effects on 'open'") == 1);
    return 0;
}

static int test_step_entity_disjoint(void)
{
    char src[2048];
    /* effects on distinct entity constants can never collide */
    snprintf(src, sizeof src,
             "sort actor\nentity ( a : actor  b : actor )\n"
             "state tagged(actor)\n"
             "action mark:  causes tagged(a)\n"
             "action clear: causes ~tagged(b)\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    /* a variable unifies with a constant: potential collision */
    snprintf(src, sizeof src,
             "sort actor\nentity ( a : actor  b : actor )\n"
             "state tagged(actor)\n"
             "action mark(X: actor): causes tagged(X)\n"
             "action clear:          causes ~tagged(b)\n");
    CHECK(nerr(src, "conflicting effects on 'tagged'") == 1);
    return 0;
}

static int test_step_mv(void)
{
    char src[2048];
    /* different values: warns */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action wake:  causes mode = alert\n"
             "action sleep: causes mode = calm\n");
    CHECK(nerr(src, "conflicting effects on 'mode'") == 1);
    /* same value: the identical effect, quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action wake:  causes mode = alert\n"
             "action rouse: causes mode = alert\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    /* different-value MV GUARDS exclude (the split idiom) */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action wake:  requires mode = calm  causes mode = alert\n"
             "action sleep: requires mode = alert causes mode = calm\n");
    CHECK(nwarn(src, "conflicting effects") == 0);
    return 0;
}

static int test_step_assign(void)
{
    char src[2048];
    /* two merge-less `:=` writers: warns */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action hit(X: actor):  causes hp(X) := 1\n"
             "action heal(X: actor): causes hp(X) := 9\n");
    CHECK(nerr(src, "both assign (`:=`) 'hp'") == 1);
    /* identical constants agree at runtime: quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action zero(X: actor):  causes hp(X) := 0\n"
             "action reset(X: actor): causes hp(X) := 0\n");
    CHECK(nwarn(src, "both assign") == 0);
    /* `merge max` absorbs concurrent assigns (#85): quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action s1: causes score := 3\n"
             "action s2: causes score := 7\n");
    CHECK(nwarn(src, "both assign") == 0);
    /* deltas sum, never contest: quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action hit(X: actor):  causes hp(X) -= 2\n"
             "action heal(X: actor): causes hp(X) += 2\n");
    CHECK(nwarn(src, "both assign") == 0);
    return 0;
}

static int test_step_self(void)
{
    char src[2048];
    /* an arity-0 target assigned a per-binding value: two instances of the
     * SAME action can co-fire and contest */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action tap(X: actor): causes total := hp(X) + 1\n");
    CHECK(nerr(src, "more than once in one step") == 1);
    /* value independent of the missing binding var: quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action tap(X: actor): requires ~open causes total := 3\n");
    CHECK(nwarn(src, "more than once") == 0);
    /* target covers the var: quiet */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action tap(X: actor): causes hp(X) := 3\n");
    CHECK(nwarn(src, "more than once") == 0);
    /* a roll varies per binding even with no explicit var read */
    snprintf(src, sizeof src, "%s%s", HDR,
             "action tap(X: actor): causes total := roll(6)\n");
    CHECK(nerr(src, "more than once in one step") == 1);
    return 0;
}

static int test_judgment_pairs(void)
{
    char src[2048];
    static const char *JH =
        "sort actor\nentity a : actor\n"
        "state ( big(actor)  small(actor) )\n";
    /* unordered complementary defeasible pair: warns */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   => strong(X)\n"
             "rule r2(X: actor): small(X) => ~strong(X)\n");
    CHECK(nwarn(src, "team defeat reads BOTH sides REFUTED") == 1);
    /* a direct `>` decides it: quiet */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   => strong(X)\n"
             "rule r2(X: actor): small(X) => ~strong(X)\n"
             "r1 > r2\n");
    CHECK(nwarn(src, "team defeat") == 0);
    /* a TEAMMATE's edge covers the pair (team defeat's static shadow) */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   => strong(X)\n"
             "rule r1b(X: actor): big(X)  => strong(X)\n"
             "rule r2(X: actor): small(X) => ~strong(X)\n"
             "r1b > r2\n");
    CHECK(nwarn(src, "team defeat") == 0);
    /* band edges live in p->sups after desugaring: quiet */
    snprintf(src, sizeof src, "%s%s", JH,
             "bands court: weak < strong_band\n"
             "rule r1(X: actor): big(X)   => strong(X)  @strong_band\n"
             "rule r2(X: actor): small(X) => ~strong(X) @weak\n");
    CHECK(nwarn(src, "team defeat") == 0);
    /* exclusive bodies (p vs ~p) can never co-fire: quiet */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)  => strong(X)\n"
             "rule r2(X: actor): ~big(X) => ~strong(X)\n");
    CHECK(nwarn(src, "team defeat") == 0);
    /* strict vs strict: superiority can never order it — always warns */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   -> strong(X)\n"
             "rule r2(X: actor): small(X) -> ~strong(X)\n"
             "r1 > r2\n");
    CHECK(nwarn(src, "definite contradiction") == 1);
    /* strict vs defeasible: strict wins, decided — quiet */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   -> strong(X)\n"
             "rule r2(X: actor): small(X) => ~strong(X)\n");
    CHECK(nwarn(src, "team defeat") == 0 &&
          nwarn(src, "definite contradiction") == 0);
    /* a defeater is not a concluding pair member: quiet */
    snprintf(src, sizeof src, "%s%s", JH,
             "rule r1(X: actor): big(X)   => strong(X)\n"
             "rule r2(X: actor): small(X) ~> ~strong(X)\n");
    CHECK(nwarn(src, "team defeat") == 0);
    return 0;
}

static int test_membership_exclusive(void)
{
    char src[2048];
    /* disjoint #95 membership lists on the shared var exclude co-firing */
    snprintf(src, sizeof src,
             "sort actor\nentity ( a : actor  b : actor )\n"
             "state seen(actor)\n"
             "rule r1(X: actor): seen(X) & X in { a } => picked(X)\n"
             "rule r2(X: actor): seen(X) & X in { b } => ~picked(X)\n");
    CHECK(nwarn(src, "team defeat") == 0);
    /* overlapping lists do not */
    snprintf(src, sizeof src,
             "sort actor\nentity ( a : actor  b : actor )\n"
             "state seen(actor)\n"
             "rule r1(X: actor): seen(X) & X in { a, b } => picked(X)\n"
             "rule r2(X: actor): seen(X) & X in { b }    => ~picked(X)\n");
    CHECK(nwarn(src, "team defeat") == 1);
    return 0;
}

int main(void)
{
    if (test_step_boolean()) return 1;
    if (test_step_entity_disjoint()) return 1;
    if (test_step_mv()) return 1;
    if (test_step_assign()) return 1;
    if (test_step_self()) return 1;
    if (test_judgment_pairs()) return 1;
    if (test_membership_exclusive()) return 1;
    printf("test_conflict: all passed\n");
    return 0;
}
