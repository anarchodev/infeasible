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

/* host geometry for `sees(X, Y)`: true iff the ordered pair is in the fixed set.
 * Deterministic and identical for both worlds, so registered atoms agree. */
typedef struct { uint32_t x[8], y[8]; int n; } seeset;
static bool sees_cb(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)pred;
    const seeset *s = ctx;
    if (nargs < 2) return false;
    for (int i = 0; i < s->n; i++)
        if (s->x[i] == args[0] && s->y[i] == args[1]) return true;
    return false;
}

static const char *STORY =
    "scene tt\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "provider sees(actor, actor)\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    "  bird(actor)\n"
    "  penguin(actor)\n"
    "  hp(actor) : int in 0 .. 20\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  adj(b, c)\n"
    "  awake(a)\n"
    "  bird(a)\n"
    "  penguin(a)\n"
    "  hp(a) = 3\n"
    "  hp(b) = 8\n"
    ")\n"
    /* matchable: a 2-var join over two base boolean fluents (head keeps both) */
    "rule alarm(X: actor, Y: actor): adj(X, Y) & awake(X) => threatens(X, Y)\n"
    /* matchable WITH a negated filter: awake(X) generates, ~penguin(X) prunes.
     * a is a penguin, so hunts(a) never holds; hunts(b) tracks awake(b). */
    "rule hostile(X: actor): awake(X) & ~penguin(X) => hunts(X)\n"
    /* matchable WITH a numeric guard filter: awake(X) generates, hp(X) <= 5 is
     * carried into the rule and solver-evaluated. hp(a)=3 passes; hp(b)=8 fails,
     * so weak(b) never holds even once b wakes. Exercises the tick-time guard
     * emit path (m_ground_lit guard branch + world_add_guard) across regrounds. */
    "rule weak(X: actor): awake(X) & hp(X) <= 5 => weak(X)\n"
    /* matchable WITH a provider filter: awake(X), awake(Y) generate; sees(X,Y) is
     * a host-answered relation carried into the rule and consulted by the solver.
     * Exercises the tick-time provider emit path (m_ground_lit provider branch +
     * world_declare_provider_atom) across regrounds. sees(a,b) is in the host set. */
    "rule spot(X: actor, Y: actor): awake(X) & awake(Y) & sees(X, Y) => spotted(X, Y)\n"
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

/* An internal LANDMARK atom — a numeric guard ("hp(b)<=5") or a provider relation
 * ("sees(a,b)") — not a host-queried judgment. Its closed-world truth is asserted
 * only once world_add_guard / world_declare_provider_atom has registered it, which
 * happens per EMITTED instance. Eager grounds every sort^k instance (registering
 * the landmark for all bindings), the matcher only the generator-satisfying ones,
 * so an unregistered landmark is PROVED in eager but UNDECIDED in the matcher.
 * That divergence never reaches a judgment: an unregistered landmark is referenced
 * by no emitted rule, so it changes no verdict a host can query. Excluded from the
 * sweep like the judgment-head artifact. */
static int is_landmark_atom(const char *name)
{
    return strpbrk(name, "<>") != NULL       /* guard: <=, <, >=, > */
        || strncmp(name, "sees(", 5) == 0;   /* the story's one provider relation */
}

/* Every (non-landmark) atom, both polarities: the two worlds must PROVE exactly
 * the same literals (same provability contract as test_matcher). Returns diffs. */
static int provability_diffs(world *A, world *B, intern *sy)
{
    uint32_t n = intern_count(sy);
    int diffs = 0;
    for (uint32_t id = 1; id < n; id++) {
        if (is_landmark_atom(intern_name(sy, id))) continue;
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

    /* the same provider callback on both worlds: sees(a,b) and sees(b,c) hold */
    seeset ss = { { intern_id(sy, "a"), intern_id(sy, "b") },
                  { intern_id(sy, "b"), intern_id(sy, "c") }, 2 };
    world_set_provider_fn(A, sees_cb, &ss);
    world_set_provider_fn(B, sees_cb, &ss);

    dl_lit t_ab = dl_pos(intern_id(sy, "threatens(a,b)"));
    dl_lit t_bc = dl_pos(intern_id(sy, "threatens(b,c)"));
    dl_lit not_flies_a = dl_neg(intern_id(sy, "flies(a)"));   /* nofly > fly decides it */
    dl_lit hunts_a = dl_pos(intern_id(sy, "hunts(a)"));       /* ~penguin filter: a is penguin */
    dl_lit hunts_b = dl_pos(intern_id(sy, "hunts(b)"));       /* tracks awake(b) */
    dl_lit weak_a = dl_pos(intern_id(sy, "weak(a)"));         /* hp(a)=3 <= 5: guard passes */
    dl_lit weak_b = dl_pos(intern_id(sy, "weak(b)"));         /* hp(b)=8 > 5: guard fails */
    dl_lit spot_ab = dl_pos(intern_id(sy, "spotted(a,b)"));   /* awake(a,b) & sees(a,b) */

    /* t=0: only awake(a). alarm(a,b) fires -> threatens(a,b); threatens(b,c) needs awake(b). */
    CHECK(world_query(A, t_ab) == DL_PROVED);
    CHECK(world_query(B, t_ab) == DL_PROVED);
    CHECK(world_query(A, t_bc) != DL_PROVED);
    CHECK(world_query(B, t_bc) != DL_PROVED);
    CHECK(world_query(B, not_flies_a) == DL_PROVED);   /* static `>` present */
    CHECK(world_query(B, hunts_a) != DL_PROVED);       /* ~penguin(a) fails: a is a penguin */
    CHECK(world_query(B, hunts_b) != DL_PROVED);       /* b not awake yet */
    CHECK(world_query(B, weak_a) == DL_PROVED);        /* awake(a) & hp(a)=3 <= 5 */
    CHECK(world_query(B, weak_b) != DL_PROVED);        /* b not awake yet */
    CHECK(world_query(B, spot_ab) != DL_PROVED);       /* b not awake: no awake(a)&awake(b) */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_ab));
    CHECK(why_same(A, B, weak_a));                     /* guard landmark atom in the trace */

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
    CHECK(world_query(B, hunts_b) == DL_PROVED);     /* awake(b) & ~penguin(b): fires now */
    CHECK(world_query(B, hunts_a) != DL_PROVED);     /* ~penguin(a) still prunes it */
    CHECK(world_query(B, weak_b) != DL_PROVED);      /* awake(b) now, but hp(b)=8 > 5: guard prunes */
    CHECK(world_query(B, weak_a) == DL_PROVED);      /* still awake, hp(a) still 3 */
    CHECK(world_query(B, spot_ab) == DL_PROVED);     /* awake(a) & awake(b) & sees(a,b): fires */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_bc));                     /* freshly-materialized trace */
    CHECK(why_same(A, B, hunts_b));                  /* negated body literal in the trace */
    CHECK(why_same(A, B, spot_ab));                  /* provider landmark atom in the trace */

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
    CHECK(world_query(B, weak_a) != DL_PROVED);      /* awake(a) gone: guard rule drops too */
    CHECK(world_query(B, spot_ab) != DL_PROVED);     /* awake(a) gone: provider rule drops too */
    CHECK(provability_diffs(A, B, sy) == 0);
    CHECK(why_same(A, B, t_bc));

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    printf("test_ticktime: all passed\n");
    return 0;
}
