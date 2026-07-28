/* Tick-time equivalence pin for the join matcher (§5.2 item 4, EPIC #26 / #28,
 * the RUNTIME half). test_matcher pins that the matcher and the eager path agree
 * at compile time; this pins that they still agree AFTER world_step changes the
 * facts — the matcher re-grounds the judgment layer against the world's *live*
 * extension index (world_fact_index), so a fact a step turned on newly satisfies
 * a rule body, and one a step turned off drops the instance it used to satisfy.
 *
 * The story: `alarm(X,Y): adj(X,Y) & awake(X) => threatens(X,Y)` — matchable (two
 * positive base boolean fluents). `wake`/`sleep` actions flip `awake`, so a step
 * both GROWS the matched set (waking b makes threatens(b,c) provable) and SHRINKS
 * it (sleeping a drops threatens(a,b)). Eager grounds every sort^k instance once
 * and recomputes from the new facts for free; the matcher must re-materialize the
 * layer each tick and land on exactly the same verdicts and why-traces.
 *
 * The head carries BOTH join vars on purpose: each ground atom then has a single
 * supporting instance, so the eager and matched why-traces are byte-identical. A
 * head that projected a join var away (`=> alerted(Y)`) would still agree on
 * PROVABILITY, but the eager trace would additionally list the inert same-head
 * instances (`alarm[b,b]`, …) the matcher never grounds — a presentation
 * difference, not a verdict one. Provability is the contract; the why match here
 * is the stronger, single-support case.
 *
 * The action also makes build_lane_families bail (emit_step_lanes needs
 * nrules==0), so world_query routes through the JUDGMENT family where the matched
 * layer lives. Both worlds share one intern, so atom ids line up for a straight
 * verdict-by-verdict diff. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } \
    } while (0)

static const char *STORY =
    "scene tt\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    "  bird(actor)\n"
    "  penguin(actor)\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  adj(b, c)\n"
    "  awake(a)\n"
    "  bird(a)\n"
    "  penguin(a)\n"
    ")\n"
    /* matchable: a 2-var join over two base boolean fluents (head keeps both) */
    "rule alarm(X: actor, Y: actor): adj(X, Y) & awake(X) => threatens(X, Y)\n"
    /* STATIC superiority (Tweety): both rules are non-matchable (in a `>` pair),
     * so they are ground once — and the `>` must SURVIVE every re-ground. If the
     * matched-layer reset truncated the superiority array (the bug the watermark
     * fix prevents), nofly would stop beating fly and ~flies(a) would un-prove. */
    "rule fly(X: actor):   bird(X)    => flies(X)\n"
    "rule nofly(X: actor): penguin(X) => ~flies(X)\n"
    "nofly > fly\n"
    /* actions flip the fluent the matchable rule reads */
    "action wake(X: actor):  causes awake(X)\n"
    "action sleep(X: actor): causes ~awake(X)\n";

/* Capture world_why(w, q) into a malloc'd string. */
static char *why_str(world *w, dl_lit q)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    world_why(w, q, f);
    fclose(f);
    return buf;
}

/* Every atom, both polarities: the two worlds must PROVE exactly the same
 * literals (same provability contract as test_matcher). Returns the diff count. */
static int provability_diffs(world *A, world *B, intern *sy)
{
    uint32_t n = intern_count(sy);
    int diffs = 0;
    for (uint32_t id = 1; id < n; id++)
        for (int neg = 0; neg < 2; neg++) {
            dl_lit q = neg ? dl_neg(id) : dl_pos(id);
            bool pa = world_query(A, q) == DL_PROVED;
            bool pb = world_query(B, q) == DL_PROVED;
            if (pa != pb) {
                fprintf(stderr, "provability differs: %s%s  eager=%d matched=%d\n",
                        neg ? "~" : "", intern_name(sy, id), pa, pb);
                diffs++;
            }
        }
    return diffs;
}

/* world_why must be byte-identical for a proved atom (the matched instance is
 * named + provenanced exactly like the eager one). */
static int why_same(world *A, world *B, dl_lit q)
{
    char *wa = why_str(A, q), *wb = why_str(B, q);
    int ok = strcmp(wa, wb) == 0;
    if (!ok)
        fprintf(stderr, "why differs:\n--- eager ---\n%s\n--- matched ---\n%s\n", wa, wb);
    free(wa); free(wb);
    return ok;
}

int main(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    /* eager FIRST so the shared intern holds the full atom superset */
    world *A = story_compile(STORY, "tt.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(STORY, "tt.story", sy, &dgb, &B);
    CHECK(A && M && B);
    CHECK(dga.nerrors == 0 && dgb.nerrors == 0);
    CHECK(story_matcher_world(M) == B);    /* sanity: matcher's world is B */

    dl_lit t_ab = dl_pos(intern_id(sy, "threatens(a,b)"));
    dl_lit t_bc = dl_pos(intern_id(sy, "threatens(b,c)"));
    dl_lit not_flies_a = dl_neg(intern_id(sy, "flies(a)"));   /* nofly > fly decides it */

    /* t=0: only awake(a). alarm(a,b) fires -> threatens(a,b); threatens(b,c) needs awake(b). */
    CHECK(world_query(A, t_ab) == DL_PROVED);
    CHECK(world_query(B, t_ab) == DL_PROVED);
    CHECK(world_query(A, t_bc) != DL_PROVED);
    CHECK(world_query(B, t_bc) != DL_PROVED);
    CHECK(world_query(B, not_flies_a) == DL_PROVED);   /* static `>` present */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_ab));

    char err[64];
    uint32_t wake_b = intern_id(sy, "wake(b)");

    /* step 1 — wake(b): the matched set GROWS. adj(b,c) & awake(b) now holds, so
     * threatens(b,c) becomes provable. Eager recomputes for free; the matcher
     * must re-ground to see it. */
    CHECK(world_step(A, &wake_b, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &wake_b, 1, err, sizeof err) == 0);
    story_matcher_reground(M);

    CHECK(world_query(A, t_bc) == DL_PROVED);
    CHECK(world_query(B, t_bc) == DL_PROVED);        /* the tick-time payoff */
    CHECK(world_query(B, t_ab) == DL_PROVED);        /* still holds (awake(a)) */
    CHECK(world_query(B, not_flies_a) == DL_PROVED); /* static `>` survived reground */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_bc));                     /* freshly-materialized trace */

    /* step 2 — sleep(a): the matched set SHRINKS. awake(a) is now false, so the
     * only support for threatens(a,b) is gone; re-grounding must DROP that
     * instance (a broken world_matched_reset would leave it, diverging from eager). */
    uint32_t sleep_a = intern_id(sy, "sleep(a)");
    CHECK(world_step(A, &sleep_a, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &sleep_a, 1, err, sizeof err) == 0);
    story_matcher_reground(M);

    CHECK(world_query(A, t_ab) != DL_PROVED);
    CHECK(world_query(B, t_ab) != DL_PROVED);        /* dropped, not stale */
    CHECK(world_query(A, t_bc) == DL_PROVED);
    CHECK(world_query(B, t_bc) == DL_PROVED);        /* awake(b) still true */
    CHECK(world_query(B, not_flies_a) == DL_PROVED); /* `>` still intact after 2 regrounds */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_bc));

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    printf("test_ticktime: all passed\n");
    return 0;
}
