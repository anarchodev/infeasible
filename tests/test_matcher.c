/* Equivalence pin for the join matcher (§5.2 item 4, EPIC #26 / #28 slice 2):
 * a real .story grounded BOTH ways — eager (story_compile) vs the fact-store
 * join matcher (story_compile_matched) — must yield byte-identical query
 * verdicts and why-traces. Eager grounds every sort^k rule instance (most
 * inert); the matcher grounds only the body-satisfying ones. An omitted inert
 * instance concludes nothing, so no verdict moves — that is the theorem.
 *
 * The story carries an action so build_lane_families bails (emit_step_lanes
 * needs nrules==0), forcing world_query through the JUDGMENT family — where the
 * matcher's grounding actually lives — rather than the lane path (which is built
 * identically in both worlds and would mask the matcher). Both compiles share
 * one intern, so atom ids line up for a straight verdict-by-verdict diff. */

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

/* host geometry for `sees(X, Y)`: true iff the ordered pair is in the fixed set */
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
    "scene bench\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "host provider sees(actor, actor)\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    "  marked(actor)\n"
    "  hp(actor) : int in 0 .. 10\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  adj(b, c)\n"
    "  awake(a)\n"
    "  awake(b)\n"
    "  hp(a) = 3\n"
    ")\n"
    /* matchable: a 2-var join over two base fluents */
    "rule threat(X: actor, Y: actor): adj(X, Y) & awake(X) => threat(X, Y)\n"
    /* matchable: a 1-var rule */
    "rule alert(X: actor): awake(X) => alert(X)\n"
    /* matchable WITH a negated filter: awake(X) generates, ~marked(X) prunes
     * (no marked facts, so ~marked holds for all — ready tracks awake) */
    "rule ready(X: actor): awake(X) & ~marked(X) => ready(X)\n"
    /* matchable WITH a numeric guard filter: awake(X) generates, hp(X) <= 5 is
     * carried into the rule and solver-evaluated (hp(a)=3 passes) */
    "rule critical(X: actor): awake(X) & hp(X) <= 5 => critical(X)\n"
    /* matchable WITH a provider filter: awake(X), awake(Y) generate; sees(X,Y) is
     * a host relation carried into the rule and consulted by the solver */
    "rule spot(X: actor, Y: actor): awake(X) & awake(Y) & sees(X, Y) => spotted(X, Y)\n"
    /* NOT matchable: guard-ONLY body, no positive generator to enumerate X from
     * (compute_bound_vars would bind X via the guard, but the matcher can't) */
    "rule hurt(X: actor): hp(X) <= 5 => hurt(X)\n"
    /* an action, so lanes are skipped and queries hit the judgment family */
    "action mark(X: actor): requires awake(X) causes marked(X)\n";

/* Capture world_why(w, q) into a malloc'd string. */
static char *why_str(world *w, dl_lit q)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    world_why(w, q, f);
    fclose(f);
    return buf;
}

int main(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    /* eager FIRST so the shared intern holds the full atom superset */
    world *A = story_compile(STORY, "m.story", sy, &dga);
    world *B = story_compile_matched(STORY, "m.story", sy, &dgb);
    CHECK(A && B);
    CHECK(dga.nerrors == 0 && dgb.nerrors == 0);

    /* same provider callback on both worlds: sees(a,b) holds */
    seeset ss = { { intern_id(sy, "a") }, { intern_id(sy, "b") }, 1 };
    world_set_provider_fn(A, sees_cb, &ss);
    world_set_provider_fn(B, sees_cb, &ss);

    /* sanity: the matcher actually fired (fewer or equal, and the right facts) */
    CHECK(world_query(A, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(b,c)"))) == DL_PROVED);
    /* a possible-but-unsatisfied instance: proved in NEITHER (no adj(a,c) fact) */
    CHECK(world_query(A, dl_pos(intern_id(sy, "threat(a,c)"))) != DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,c)"))) != DL_PROVED);
    /* negated filter: ready(a) fires (awake(a) & ~marked(a)); c never awake */
    CHECK(world_query(B, dl_pos(intern_id(sy, "ready(a)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "ready(c)"))) != DL_PROVED);
    /* numeric-guard filter: critical(a) fires (awake(a) & hp(a)=3 <= 5) */
    CHECK(world_query(B, dl_pos(intern_id(sy, "critical(a)"))) == DL_PROVED);
    /* provider filter: spotted(a,b) fires (awake(a) & awake(b) & sees(a,b)) */
    CHECK(world_query(B, dl_pos(intern_id(sy, "spotted(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "spotted(b,c)"))) != DL_PROVED);  /* c not awake */

    /* The equivalence: the two worlds PROVE exactly the same literals — every
     * atom, both polarities. Provability is the guarantee the matcher owes and
     * hosts consume ("is this judgment true?"). We compare on DL_PROVED rather
     * than the raw verdict because of one solver artifact: eager grounds an
     * always-failing rule for an unsatisfiable instance (e.g. `threat(b,b)`,
     * whose body adj(b,b) is closed-world false), which lets DL refute it (−∂);
     * the matcher grounds no such rule, and the scaffold solver leaves an atom
     * with no rules UNDECIDED rather than refuting it by vacuous defeat.
     * Judgments are not closed-world (only fluents are), so nothing that is
     * PROVED ever moves — that is the theorem, and it holds exactly.
     *
     * Internal LANDMARK atoms — numeric guards ("hp(b)<=5") and provider relations
     * ("sees(a,b)") — are excluded: they are closed-world only once
     * world_add_guard / world_declare_provider_atom registers them, which happens
     * per EMITTED instance, so an unregistered one is proved in eager but undecided
     * in the matcher. It never reaches a judgment (no emitted rule references it),
     * so it is a compiler-internal atom, not part of the host-visible contract. */
    uint32_t n = intern_count(sy);
    int diffs = 0;
    for (uint32_t id = 1; id < n; id++) {
        const char *nm = intern_name(sy, id);
        if (strpbrk(nm, "<>") || strncmp(nm, "sees(", 5) == 0) continue;  /* landmark */
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
    CHECK(diffs == 0);

    /* why-trace equivalence for a matcher-grounded proved atom */
    {
        dl_lit q = dl_pos(intern_id(sy, "threat(b,c)"));
        char *wa = why_str(A, q), *wb = why_str(B, q);
        if (strcmp(wa, wb) != 0)
            fprintf(stderr, "why-trace differs:\n--- eager ---\n%s\n--- matched ---\n%s\n", wa, wb);
        CHECK(strcmp(wa, wb) == 0);
        free(wa); free(wb);
    }

    world_free(A); world_free(B);
    intern_free(sy);
    printf("test_matcher: all passed\n");
    return 0;
}
