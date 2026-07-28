/* Append-only schema locations (#67, EPIC #63): once an atom has a schema
 * location it KEEPS it across re-grounds — even when a *different* match is
 * dropped and the surviving matched atoms would otherwise shift. This is the
 * correctness prerequisite for reusing a cached columnar family (#68): the
 * family's columns are indexed by location, so a shifted location would misread
 * every verdict. Without the fix (memset + reassign from 0), dropping the (a,b)
 * match slides (a,c) into (a,b)'s old slot and this test fails. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* The `watch` rule READS linked(a,b), so `linked` is deliberately NOT an
 * island (#80): its matches must keep flowing through the jfam as ground
 * matched rules — which is exactly the machinery whose append-only locations
 * this test pins. (An island pred's matches never touch the location maps.) */
static const char *STORY =
    "scene loc\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "state ( rel(actor, actor) )\n"
    "rule link(X: actor, Y: actor): rel(X, Y) => linked(X, Y)\n"
    "rule watch: linked(a, b) => watched\n";

int main(void)
{
    intern *sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = NULL;
    story_matcher *M = story_compile_matcher(STORY, "loc.story", sy, &dg, &w);
    CHECK(M && w && dg.nerrors == 0);

    uint32_t rel_ab = intern_id(sy, "rel(a,b)");
    uint32_t rel_ac = intern_id(sy, "rel(a,c)");
    dl_lit   la_c   = dl_pos(intern_id(sy, "linked(a,c)"));

    /* two matches: link[a,b] and link[a,c] */
    world_set(w, rel_ab, true);
    world_set(w, rel_ac, true);
    CHECK(world_query(w, la_c) == DL_PROVED);            /* forces re-ground + assign_locs */
    uint32_t loc_before = world_atom_loc(w, intern_id(sy, "linked(a,c)"));
    CHECK(loc_before != ~0u);                            /* it got a location */

    /* drop the (a,b) match — (a,c) survives and must keep its location */
    world_set(w, rel_ab, false);
    CHECK(world_query(w, la_c) == DL_PROVED);            /* re-ground: matches now {a,c} */
    uint32_t loc_after = world_atom_loc(w, intern_id(sy, "linked(a,c)"));

    CHECK(loc_after == loc_before);                      /* APPEND-ONLY: no shift */

    story_matcher_free(M);
    world_free(w);
    intern_free(sy);
    printf("test_locstable: all passed\n");
    return 0;
}
