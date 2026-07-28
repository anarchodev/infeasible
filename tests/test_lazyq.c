/* The lazy-query pin (#77): a fluent no rule references still answers
 * CLOSED-WORLD (refuted when false, proved when true) — never UNDECIDED — in
 * both grounding modes, its why-trace is identical across modes, and a
 * world_set flip is visible both through the pure lazy path and after a
 * world_why has materialized the atom into the judgment family. The dense
 * jfam gives all of this by sweeping every declared fluent; the sparse jfam
 * must preserve it without the sweep. */

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

/* awake(c) is declared but referenced by NO rule: the matchable rule's only
 * generator is adj, and no adj fact involves c on the left. */
static const char *STORY =
    "scene lazy\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  awake(a)\n"
    ")\n"
    "rule threat(X: actor, Y: actor): adj(X, Y) & awake(X) => threat(X, Y)\n"
    "action wake(X: actor): causes awake(X)\n";

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

    world *A = story_compile(STORY, "lazy.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(STORY, "lazy.story", sy, &dgb, &B);
    CHECK(A && M && B);
    CHECK(dga.nerrors == 0 && dgb.nerrors == 0);

    uint32_t awake_c = intern_id(sy, "awake(c)");

    /* 1. unreferenced fluent: closed-world, NOT undecided — both modes */
    CHECK(world_query(A, dl_pos(awake_c)) == DL_REFUTED);
    CHECK(world_query(A, dl_neg(awake_c)) == DL_PROVED);
    CHECK(world_query(B, dl_pos(awake_c)) == DL_REFUTED);
    CHECK(world_query(B, dl_neg(awake_c)) == DL_PROVED);

    /* 2. its why-trace is mode-independent (and byte-stable across the switch) */
    {
        char *wa = why_str(A, dl_neg(awake_c));
        char *wb = why_str(B, dl_neg(awake_c));
        if (strcmp(wa, wb) != 0)
            fprintf(stderr, "why differs:\n--- eager ---\n%s\n--- matcher ---\n%s\n",
                    wa, wb);
        CHECK(strcmp(wa, wb) == 0);
        free(wa); free(wb);
    }

    /* 3. a fact flip is visible AFTER the why above (B's atom may now be
     *    materialized in its family) ... */
    world_set(A, awake_c, true);
    world_set(B, awake_c, true);
    CHECK(world_query(A, dl_pos(awake_c)) == DL_PROVED);
    CHECK(world_query(B, dl_pos(awake_c)) == DL_PROVED);
    CHECK(world_query(B, dl_neg(awake_c)) == DL_REFUTED);

    /* ... and on a FRESH matcher world through the pure lazy path (no why) */
    {
        intern *sy2 = intern_new();
        story_diag dc[16]; story_diags dgc = { dc, 16, 0, 0 };
        world *C = NULL;
        story_matcher *M2 = story_compile_matcher(STORY, "lazy.story", sy2, &dgc, &C);
        CHECK(M2 && C && dgc.nerrors == 0);
        uint32_t ac2 = intern_id(sy2, "awake(c)");
        world_set(C, ac2, true);
        CHECK(world_query(C, dl_pos(ac2)) == DL_PROVED);
        CHECK(world_query(C, dl_neg(ac2)) == DL_REFUTED);
        story_matcher_free(M2);
        world_free(C);
        intern_free(sy2);
    }

    /* 4. an interned-but-unknown atom stays outside the theory in both modes */
    uint32_t ghost = intern_id(sy, "ghost");
    CHECK(world_query(A, dl_pos(ghost)) == DL_UNDECIDED);
    CHECK(world_query(B, dl_pos(ghost)) == DL_UNDECIDED);
    {
        char *wb = why_str(B, dl_pos(ghost));
        CHECK(strstr(wb, "not in the theory") != NULL);
        free(wb);
    }

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    printf("test_lazyq: all passed\n");
    return 0;
}
