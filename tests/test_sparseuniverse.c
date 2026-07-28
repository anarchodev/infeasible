/* The sparse fluent universe (#92): ground boolean fluents exist when TOUCHED;
 * everything else answers closed-world through the registered schema hook.
 *
 * Part 1 (world API, hand-registered schema_fn) pins the state-tier contract:
 * pure query fallback (closed-world false, no declaration), declare-on-any-
 * touch (true AND false, with invalidation), the loader's closed-world fact
 * for rule-referenced untouched atoms, why-declares with byte-identical
 * traces vs an explicitly-declared twin, and unknown atoms staying outside
 * the theory.
 *
 * Part 2 (story, added with the lang wiring) pins matcher-vs-eager end to end
 * plus the economy: O(touched) declared fluents vs the cross-product. */

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

static char *why_str(world *w, dl_lit q)
{
    char *buf = NULL; size_t n = 0;
    FILE *f = open_memstream(&buf, &n);
    world_why(w, q, f);
    fclose(f);
    return buf;
}

/* hand schema: a fixed table of recognized ground atoms with their (pred, args) */
typedef struct {
    int n;
    uint32_t atom[8], pred[8], a0[8], a1[8];
} schema_tab;

static bool tab_schema(void *ctx, uint32_t atom, uint32_t *pred,
                       uint32_t *args, int *nargs)
{
    const schema_tab *t = ctx;
    for (int i = 0; i < t->n; i++)
        if (t->atom[i] == atom) {
            *pred = t->pred[i];
            args[0] = t->a0[i];
            args[1] = t->a1[i];
            *nargs = 2;
            return true;
        }
    return false;
}

/* Build one world: fluent p (declared, true), rule r1: p => q, rule
 * r2: ~rel(c,d) => calm. With `extra_decl`, also explicitly declare rel(d,e) —
 * the twin used for trace byte-comparison against the lazy-declared world. */
static world *mk(intern *sy, schema_tab *t, bool extra_decl)
{
    world *w = world_new(sy);
    uint32_t p = intern_id(sy, "p");
    world_declare_fluent(w, p);
    world_set(w, p, true);
    if (extra_decl)
        world_declare_fluent(w, intern_id(sy, "rel(d,e)"));
    dl_lit b = dl_pos(p);
    world_add_rule(w, "r1", DL_DEFEASIBLE, dl_pos(intern_id(sy, "q")), &b, 1);
    dl_lit nb = dl_neg(intern_id(sy, "rel(c,d)"));
    world_add_rule(w, "r2", DL_DEFEASIBLE, dl_pos(intern_id(sy, "calm")), &nb, 1);
    world_set_schema_fn(w, tab_schema, t);
    return w;
}

int main(void)
{
    intern *sy = intern_new();
    uint32_t rel = intern_id(sy, "rel");
    uint32_t ea = intern_id(sy, "a"), eb = intern_id(sy, "b");
    uint32_t ec = intern_id(sy, "c"), ed = intern_id(sy, "d"), ee = intern_id(sy, "e");
    uint32_t r_ab = intern_id(sy, "rel(a,b)");
    uint32_t r_bc = intern_id(sy, "rel(b,c)");
    uint32_t r_cd = intern_id(sy, "rel(c,d)");
    uint32_t r_de = intern_id(sy, "rel(d,e)");

    schema_tab t = { 4,
        { r_ab, r_bc, r_cd, r_de },
        { rel,  rel,  rel,  rel  },
        { ea,   eb,   ec,   ed   },
        { eb,   ec,   ed,   ee   } };

    world *w = mk(sy, &t, false);
    int base = world_fluent_count(w);              /* just p */

    /* pure fallback: never-touched schema atom answers closed-world false,
     * and the query DECLARES NOTHING */
    CHECK(world_query(w, dl_pos(r_ab)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(r_ab)) == DL_PROVED);
    CHECK(world_get(w, r_ab) == false);
    CHECK(world_fluent_count(w) == base);

    /* rule-referenced untouched atom: the loader supplies its closed-world
     * fact, so r2 fires; the atom itself answers through its jloc */
    CHECK(world_query(w, dl_pos(intern_id(sy, "calm"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(r_cd)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(r_cd)) == DL_PROVED);
    CHECK(world_fluent_count(w) == base);          /* still nothing declared */

    /* declare on TRUE touch */
    world_set(w, r_ab, true);
    CHECK(world_fluent_count(w) == base + 1);
    CHECK(world_query(w, dl_pos(r_ab)) == DL_PROVED);
    CHECK(world_get(w, r_ab) == true);

    /* declare on FALSE touch too — a no-change assertion still invalidates */
    world_set(w, r_bc, false);
    CHECK(world_fluent_count(w) == base + 2);
    CHECK(world_query(w, dl_pos(r_bc)) == DL_REFUTED);

    /* the touched fact flips like any declared fluent */
    world_set(w, r_ab, false);
    CHECK(world_query(w, dl_pos(r_ab)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(r_ab)) == DL_PROVED);

    /* why on a never-touched schema atom DECLARES it and renders the trace an
     * explicitly-declared twin produces — byte-identical, both polarities */
    {
        world *tw = mk(sy, &t, true);              /* rel(d,e) declared up front */
        dl_verdict v1 = world_query(w, dl_neg(r_de));
        char *lz_n = why_str(w, dl_neg(r_de)), *tw_n = why_str(tw, dl_neg(r_de));
        char *lz_p = why_str(w, dl_pos(r_de)), *tw_p = why_str(tw, dl_pos(r_de));
        if (strcmp(lz_n, tw_n) != 0)
            fprintf(stderr, "neg why differs:\n--- lazy ---\n%s--- twin ---\n%s",
                    lz_n, tw_n);
        CHECK(strcmp(lz_n, tw_n) == 0);
        CHECK(strcmp(lz_p, tw_p) == 0);
        CHECK(strstr(lz_n, "it is a base fact") != NULL);   /* the diverging line */
        free(lz_n); free(tw_n); free(lz_p); free(tw_p);
        CHECK(world_fluent_count(w) == base + 3);  /* why declared it */
        CHECK(world_query(w, dl_neg(r_de)) == v1); /* query->why->query stable */
        world_free(tw);
    }

    /* unknown atoms stay outside the theory */
    uint32_t ghost = intern_id(sy, "ghost");
    CHECK(world_query(w, dl_pos(ghost)) == DL_UNDECIDED);
    {
        char *g = why_str(w, dl_pos(ghost));
        CHECK(strstr(g, "not in the theory") != NULL);
        free(g);
    }

    world_free(w);
    intern_free(sy);
    printf("test_sparseuniverse: all passed\n");
    return 0;
}
