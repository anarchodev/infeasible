/* Matched views (#80): island judgments answered by set membership.
 *
 * Part 1 (this file's world-API half) pins the state-tier contract directly:
 * present -> PROVED for the head polarity / REFUTED for the complement;
 * seen-but-dropped -> REFUTED both polarities (the located-rule-less analog);
 * never-seen -> UNDECIDED; bounded memory across many reset/refill cycles;
 * world_why renders through the materialize hook (present), the two-line
 * refuted trace (dropped), and the not-in-the-theory message (never).
 *
 * Part 2 (added with the story wiring) pins eager==matcher over island
 * stories end to end. */

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

/* Materialize hook: re-emit the instance as an ordinary matched rule — the
 * shape the story matcher uses, reduced to this test's one rule
 * link(X,Y): rel(X,Y) => linked(X,Y). */
typedef struct { intern *sy; uint32_t rel_ab; } mat_ctx;
static void materialize(void *ctx, world *w, uint32_t atom,
                        int view, const uint32_t *bind, int nvars)
{
    (void)view; (void)nvars;
    mat_ctx *m = ctx;
    char name[128];
    snprintf(name, sizeof name, "link(%s,%s)",
             intern_name(m->sy, bind[0]), intern_name(m->sy, bind[1]));
    dl_lit body = dl_pos(m->rel_ab);
    world_add_rule(w, name, DL_DEFEASIBLE, dl_pos(atom), &body, 1);
}

static int world_api_half(void)
{
    intern *sy = intern_new();
    world *w = world_new(sy);

    uint32_t a = intern_id(sy, "a"), b = intern_id(sy, "b");
    uint32_t rel_ab = intern_id(sy, "rel(a,b)");
    world_declare_fluent(w, rel_ab);
    world_set(w, rel_ab, true);

    uint32_t lab = intern_id(sy, "linked(a,b)");
    uint32_t lba = intern_id(sy, "linked(b,a)");
    uint32_t lxx = intern_id(sy, "linked(x,y)");     /* never added */
    uint32_t nab = intern_id(sy, "noisy(a,b)");      /* negative-head view */

    world_matched_checkpoint(w);                     /* matched adds -> matched_a */

    int v  = world_view_new(w, intern_id(sy, "linked"), false, DL_DEFEASIBLE);
    int vn = world_view_new(w, intern_id(sy, "noisy"), true, DL_DEFEASIBLE);
    mat_ctx mc = { sy, rel_ab };
    world_set_materialize_fn(w, materialize, &mc);

    /* tick 1: linked(a,b) and linked(b,a) present; noisy(a,b) present (neg head) */
    uint32_t bind_ab[2] = { a, b }, bind_ba[2] = { b, a };
    world_views_reset(w);
    world_view_add(w, v, lab, bind_ab, 2);
    world_view_add(w, v, lba, bind_ba, 2);
    world_view_add(w, vn, nab, bind_ab, 2);

    CHECK(world_query(w, dl_pos(lab)) == DL_PROVED);
    CHECK(world_query(w, dl_neg(lab)) == DL_REFUTED);
    CHECK(world_query(w, dl_pos(lba)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(lxx)) == DL_UNDECIDED);   /* never seen */
    CHECK(world_query(w, dl_neg(lxx)) == DL_UNDECIDED);
    CHECK(world_query(w, dl_neg(nab)) == DL_PROVED);      /* ~noisy concluded */
    CHECK(world_query(w, dl_pos(nab)) == DL_REFUTED);
    CHECK(world_view_row_count(w) == 3);

    /* why on a present view atom renders the materialized rule; the verdict is
     * unchanged by the materialization (query -> why -> query) */
    {
        dl_verdict v1 = world_query(w, dl_pos(lab));
        char *t = why_str(w, dl_pos(lab));
        CHECK(strstr(t, "link(a,b)") != NULL);            /* the rule line */
        CHECK(strstr(t, "defeasible: PROVED") != NULL);
        CHECK(strstr(t, "rel(a,b)") != NULL);             /* its body literal */
        free(t);
        CHECK(world_query(w, dl_pos(lab)) == v1);
    }

    /* tick 2: linked(b,a) dropped -> REFUTED both ways; two-line trace */
    world_views_reset(w);
    world_view_add(w, v, lab, bind_ab, 2);
    CHECK(world_query(w, dl_pos(lba)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(lba)) == DL_REFUTED);
    CHECK(world_query(w, dl_pos(lab)) == DL_PROVED);      /* survivor unaffected */
    {
        char *t = why_str(w, dl_pos(lba));
        CHECK(strstr(t, "defeasible: REFUTED") != NULL);
        CHECK(strstr(t, "rules for it") == NULL);         /* no rules rendered */
        CHECK(strstr(t, "not in the theory") == NULL);
        free(t);
        CHECK(world_query(w, dl_pos(lba)) == DL_REFUTED); /* why changed nothing */
    }

    /* never-seen atom keeps the message */
    {
        char *t = why_str(w, dl_pos(lxx));
        CHECK(strstr(t, "not in the theory") != NULL);
        free(t);
    }

    /* re-added after a drop: present again */
    world_views_reset(w);
    world_view_add(w, v, lba, bind_ba, 2);
    CHECK(world_query(w, dl_pos(lba)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(lab)) == DL_REFUTED);     /* now lab is dropped */

    /* memory is BOUNDED across many identical refills (the #48 analog) */
    world_views_reset(w);
    world_view_add(w, v, lab, bind_ab, 2);
    world_view_add(w, v, lba, bind_ba, 2);
    size_t bytes0 = world_view_bytes(w);
    for (int i = 0; i < 500; i++) {
        world_views_reset(w);
        world_view_add(w, v, lab, bind_ab, 2);
        world_view_add(w, v, lba, bind_ba, 2);
    }
    CHECK(world_view_bytes(w) == bytes0);
    CHECK(world_view_row_count(w) == 2);
    CHECK(world_query(w, dl_pos(lab)) == DL_PROVED);

    world_free(w);
    intern_free(sy);
    return 1;
}

int main(void)
{
    if (!world_api_half()) return 1;
    printf("test_matchview: all passed\n");
    return 0;
}
