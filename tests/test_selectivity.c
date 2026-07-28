/* Selectivity ordering for the tick-time matcher (§5.2 / EPIC #43, #46). The
 * join must visit the SMALLEST live extension first, so a dense-then-selective
 * rule costs ∝ the selective extension, not the dense one. `pair` joins a big
 * relation (8 tuples) with a small one (1 tuple); written big-first in the
 * source, the planner must still scan `small` first — measured by the reground's
 * probe count (fact-tuples walked). Reordering is semantically invisible, so the
 * conclusions are unchanged; only the intermediate work shrinks. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static const char *STORY =
    "scene sel\n"
    "sort actor\n"
    "entity ( a, b, c, d : actor )\n"
    "state (\n"
    "  big(actor, actor)\n"
    "  small(actor, actor)\n"
    ")\n"
    "init (\n"
    "  big(a, a) big(a, b) big(a, c) big(a, d)\n"   /* 8 dense tuples */
    "  big(b, a) big(b, b) big(b, c) big(b, d)\n"
    "  small(a, b)\n"                                /* 1 selective tuple */
    ")\n"
    /* big FIRST in source: the planner must reorder to scan `small` first */
    "rule pair(X: actor, Y: actor): big(X, Y) & small(X, Y) => linked(X, Y)\n";

int main(void)
{
    intern *sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };

    world *w = NULL;
    story_matcher *M = story_compile_matcher(STORY, "sel.story", sy, &dg, &w);
    CHECK(M && w);
    CHECK(dg.nerrors == 0);

    /* correctness: only (a,b) satisfies big & small */
    CHECK(world_query(w, dl_pos(intern_id(sy, "linked(a,b)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "linked(a,c)"))) != DL_PROVED);

    /* selectivity: the reground scans `small` (1 tuple) first, then probes `big`
     * once — ~2 tuples walked. Source order (big first) would walk all 8 + the
     * per-tuple `small` probe (~9). The gap is the whole point. */
    long probes = story_matcher_last_probes(M);
    if (probes > 4)
        fprintf(stderr, "probes=%ld (expected ~2 for small-first; ~9 is big-first)\n", probes);
    CHECK(probes <= 4);

    /* stable across regrounds (deterministic order, I4) */
    story_matcher_reground(M);
    CHECK(story_matcher_last_probes(M) == probes);
    CHECK(world_query(w, dl_pos(intern_id(sy, "linked(a,b)"))) == DL_PROVED);

    story_matcher_free(M);
    world_free(w);
    intern_free(sy);
    printf("test_selectivity: all passed\n");
    return 0;
}
