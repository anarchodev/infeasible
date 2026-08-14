/* Golden test for the numeric extension index (#250).
 *
 * `world_set_fluent_struct` lets the boolean fact index invert "is this atom
 * true?" into "which tuples does this predicate have?". Numeric atoms had no
 * such structure, so nothing could answer which entities carry a value for
 * `x` — which is what an ordered-axis index (§8.3) needs before it can be
 * built over anything.
 *
 * The property worth pinning is the one that differs from the boolean index:
 * this one is BUILD-ONCE. A boolean extension holds the currently-true tuples
 * and refreshes on every state edit; a numeric fluent has no truth to change,
 * so its instances are fixed after grounding and only the values move.
 * world_set_num must therefore not disturb it, and world_declare_num must. */

#include "state/world.h"
#include "core/intern.h"
#include "lang/story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static intern *SY;

static uint32_t decl(world *w, const char *pred, const char *ent)
{
    char b[64];
    snprintf(b, sizeof b, "%s(%s)", pred, ent);
    uint32_t atom = intern_id(SY, b), p = intern_id(SY, pred), e = intern_id(SY, ent);
    world_declare_num(w, atom, 0, 100, true);
    world_set_num_struct(w, atom, p, &e, 1);
    return atom;
}

int main(void)
{
    SY = intern_new();
    world *w = world_new(SY);

    /* interleave two predicates so a per-predicate grouping that accidentally
     * relied on declaration contiguity would be caught */
    uint32_t xa = decl(w, "x", "a");
    uint32_t ya = decl(w, "y", "a");
    uint32_t xb = decl(w, "x", "b");
    uint32_t xc = decl(w, "x", "c");
    uint32_t yb = decl(w, "y", "b");
    (void)ya; (void)yb;

    const uint32_t *ents, *atoms;

    /* declaration order within a predicate, which is the order grounding walks
     * — it feeds roll-site indices and lane assignment, so it is I4 and not
     * merely tidiness */
    {
        int n = world_num_ext1(w, intern_id(SY, "x"), &ents, &atoms);
        CHECK(n == 3);
        CHECK(ents[0] == intern_id(SY, "a") && atoms[0] == xa);
        CHECK(ents[1] == intern_id(SY, "b") && atoms[1] == xb);
        CHECK(ents[2] == intern_id(SY, "c") && atoms[2] == xc);
        printf("  x: 3 instances in declaration order\n");
    }
    {
        int n = world_num_ext1(w, intern_id(SY, "y"), &ents, &atoms);
        CHECK(n == 2);
        printf("  y: 2 instances, unaffected by x's interleaving\n");
    }

    /* a predicate nobody declared answers 0 rather than failing */
    {
        int n = world_num_ext1(w, intern_id(SY, "nosuch"), &ents, &atoms);
        CHECK(n == 0 && ents == NULL && atoms == NULL);
        printf("  absent predicate: 0\n");
    }

    /* arity 0 and arity 2 have no lane axis, so they are not offered — and must
     * not disturb the arity-1 answer */
    {
        uint32_t g = intern_id(SY, "gold"), ga = intern_id(SY, "gold");
        world_declare_num(w, ga, 0, 100, true);
        world_set_num_struct(w, ga, g, NULL, 0);
        uint32_t d = intern_id(SY, "dist"), da = intern_id(SY, "dist(a,b)");
        uint32_t pair[2] = { intern_id(SY, "a"), intern_id(SY, "b") };
        world_declare_num(w, da, 0, 100, true);
        world_set_num_struct(w, da, d, pair, 2);
        CHECK(world_num_ext1(w, g, &ents, &atoms) == 0);
        CHECK(world_num_ext1(w, d, &ents, &atoms) == 0);
        CHECK(world_num_ext1(w, intern_id(SY, "x"), &ents, &atoms) == 3);
        printf("  arity 0 and 2: not offered, arity 1 intact\n");
    }

    /* BUILD-ONCE: setting values must not disturb the extension, because the
     * instances did not change — only what they hold. Invalidating here would
     * cost per tick exactly the work the index exists to save. */
    {
        int n = world_num_ext1(w, intern_id(SY, "x"), &ents, &atoms);
        const uint32_t *before = ents;
        world_set_num(w, xa, 42);
        world_set_num(w, xb, 7);
        int m = world_num_ext1(w, intern_id(SY, "x"), &ents, &atoms);
        CHECK(m == n && ents == before);          /* same array, not rebuilt */
        CHECK(world_get_num(w, xa) == 42 && world_get_num(w, xb) == 7);
        printf("  world_set_num: values move, extension does not\n");
    }

    /* declaring a new instance DOES change what the instances are */
    {
        uint32_t xd = decl(w, "x", "d");
        int n = world_num_ext1(w, intern_id(SY, "x"), &ents, &atoms);
        CHECK(n == 4);
        CHECK(ents[3] == intern_id(SY, "d") && atoms[3] == xd);
        printf("  world_declare_num: extension re-answers with 4\n");
    }

    world_free(w);
    intern_free(SY);

    /* end to end: the grounder registers structure for a real .story, so a
     * numeric predicate declared in source is reachable from the index */
    {
        static const char SRC[] =
            "sort actor\n"
            "entity ( u1, u2, u3 : actor )\n"
            "state ( hp(actor) : int in 0..30  x(actor) : int in 0..99 )\n"
            "init ( hp(u1)=30 hp(u2)=20 hp(u3)=10 )\n";
        intern *sy = intern_new();
        story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
        world *sw = story_compile(SRC, "t.story", sy, &dg);
        CHECK(sw != NULL && dg.nerrors == 0);
        const uint32_t *e2, *a2;
        int n = world_num_ext1(sw, intern_id(sy, "hp"), &e2, &a2);
        CHECK(n == 3);
        CHECK(world_get_num(sw, a2[0]) == 30);
        CHECK(n == world_num_ext1(sw, intern_id(sy, "x"), &e2, &a2));
        printf("  from .story: hp and x both reachable, 3 each\n");
        world_free(sw);
        intern_free(sy);
    }

    printf("test_numext: all passed\n");
    return 0;
}
