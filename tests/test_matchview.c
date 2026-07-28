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

/* ---- Part 2: story-driven islands, eager vs tick-time matcher ----
 * The head PROJECTS the join var away, so one head atom can carry several
 * bindings — the view keeps a row list and materialize-on-why must render
 * every supporting instance. (Eager additionally grounds inert instances, so
 * eager-vs-matcher trace equality doesn't hold for projected heads even
 * before views — see test_ticktime's header; provability is the contract.) */
static const char *ISL_STORY =
    "scene isl\n"
    "sort actor\n"
    "entity ( a, b, c : actor )\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    ")\n"
    "init (\n"
    "  adj(a, b)\n"
    "  adj(a, c)\n"
    "  awake(b)\n"
    "  awake(c)\n"
    ")\n"
    "rule danger(X: actor, Y: actor): adj(X, Y) & awake(Y) => danger(X)\n"
    "action sleep(X: actor): causes ~awake(X)\n";

static int proved_diffs(world *A, world *B, intern *sy)
{
    int diffs = 0;
    uint32_t n = intern_count(sy);
    for (uint32_t id = 1; id < n; id++)
        for (int neg = 0; neg < 2; neg++) {
            dl_lit q = neg ? dl_neg(id) : dl_pos(id);
            bool pa = world_query(A, q) == DL_PROVED;
            bool pb = world_query(B, q) == DL_PROVED;
            if (pa != pb) {
                fprintf(stderr, "provability differs: %s%s eager=%d matched=%d\n",
                        neg ? "~" : "", intern_name(sy, id), pa, pb);
                diffs++;
            }
        }
    return diffs;
}

static int story_half(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    world *A = story_compile(ISL_STORY, "isl.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(ISL_STORY, "isl.story", sy, &dgb, &B);
    CHECK(A && M && B && dga.nerrors == 0 && dgb.nerrors == 0);

    dl_lit da_a = dl_pos(intern_id(sy, "danger(a)"));
    dl_lit da_b = dl_pos(intern_id(sy, "danger(b)"));

    /* two bindings support danger(a): (a,b) and (a,c) */
    CHECK(world_query(B, da_a) == DL_PROVED);
    CHECK(world_query(B, dl_neg(intern_id(sy, "danger(a)"))) == DL_REFUTED);
    CHECK(world_query(B, da_b) == DL_UNDECIDED);      /* never matched */
    CHECK(proved_diffs(A, B, sy) == 0);

    /* why on the multi-bind head materializes BOTH instances */
    {
        dl_verdict v1 = world_query(B, da_a);
        char *t = why_str(B, da_a);
        CHECK(strstr(t, "danger[X=a,Y=b]") != NULL);
        CHECK(strstr(t, "danger[X=a,Y=c]") != NULL);
        CHECK(strstr(t, "defeasible: PROVED") != NULL);
        free(t);
        CHECK(world_query(B, da_a) == v1);            /* why changed nothing */
        char *t2 = why_str(B, da_a);                  /* idempotent: no duped rules */
        char *first = strstr(t2, "danger[X=a,Y=b]");
        CHECK(first && strstr(first + 1, "danger[X=a,Y=b]") == NULL);
        free(t2);
    }

    /* shrink to one support: still proved (team of one) */
    char err[64];
    uint32_t sleep_b = intern_id(sy, "sleep(b)");
    CHECK(world_step(A, &sleep_b, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &sleep_b, 1, err, sizeof err) == 0);
    CHECK(world_query(B, da_a) == DL_PROVED);
    CHECK(proved_diffs(A, B, sy) == 0);

    /* drop the last support: seen-but-absent -> REFUTED, two-line trace */
    uint32_t sleep_c = intern_id(sy, "sleep(c)");
    CHECK(world_step(A, &sleep_c, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &sleep_c, 1, err, sizeof err) == 0);
    CHECK(world_query(B, da_a) == DL_REFUTED);
    CHECK(world_query(B, dl_neg(intern_id(sy, "danger(a)"))) == DL_REFUTED);
    CHECK(world_query(B, da_b) == DL_UNDECIDED);      /* still never seen */
    CHECK(proved_diffs(A, B, sy) == 0);
    {
        char *t = why_str(B, da_a);
        CHECK(strstr(t, "defeasible: REFUTED") != NULL);
        CHECK(strstr(t, "rules for it") == NULL);
        free(t);
        char *tn = why_str(B, da_b);
        CHECK(strstr(tn, "not in the theory") != NULL);
        free(tn);
    }

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    return 1;
}

/* ---- Part 3: guard/provider FILTER islands (phase 2) ---- */

static const char *GRD_STORY =
    "scene grd\n"
    "sort actor\n"
    "entity ( a, b : actor )\n"
    "provider sees(actor, actor)\n"
    "state (\n"
    "  awake(actor)\n"
    "  hp(actor) : int in 0 .. 10\n"
    ")\n"
    "init (\n"
    "  awake(a)\n"
    "  awake(b)\n"
    "  hp(a) = 3\n"
    "  hp(b) = 8\n"
    ")\n"
    "rule weak(X: actor): awake(X) & hp(X) <= 5 => weak(X)\n"
    "rule spot(X: actor, Y: actor): awake(X) & awake(Y) & sees(X, Y) => spotted(X, Y)\n";

static bool grd_sees_cb(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)pred;
    const uint32_t *pair = ctx;                    /* sees(a,b) only */
    return nargs >= 2 && args[0] == pair[0] && args[1] == pair[1];
}

static int guard_half(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    world *A = story_compile(GRD_STORY, "grd.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(GRD_STORY, "grd.story", sy, &dgb, &B);
    CHECK(A && M && B && dga.nerrors == 0 && dgb.nerrors == 0);

    uint32_t pair[2] = { intern_id(sy, "a"), intern_id(sy, "b") };
    world_set_provider_fn(A, grd_sees_cb, pair);
    world_set_provider_fn(B, grd_sees_cb, pair);

    /* guard filter: hp(a)=3 <= 5 passes, hp(b)=8 fails. The failed filter is
     * the documented provability asymmetry: eager refutes the inapplicable
     * instance, the island simply never matched it. */
    CHECK(world_query(B, dl_pos(intern_id(sy, "weak(a)"))) == DL_PROVED);
    CHECK(world_query(A, dl_pos(intern_id(sy, "weak(a)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "weak(b)"))) == DL_UNDECIDED);
    CHECK(world_query(A, dl_pos(intern_id(sy, "weak(b)"))) == DL_REFUTED);

    /* provider filter: sees(a,b) holds, sees(b,a) does not */
    CHECK(world_query(B, dl_pos(intern_id(sy, "spotted(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "spotted(b,a)"))) == DL_UNDECIDED);

    /* materialize-on-why re-runs the FULL body incl. landmark registration, so
     * guard/provider island traces are byte-identical to eager's */
    {
        dl_lit q = dl_pos(intern_id(sy, "weak(a)"));
        char *wa = why_str(A, q), *wb = why_str(B, q);
        if (strcmp(wa, wb) != 0)
            fprintf(stderr, "guard why differs:\n--- eager ---\n%s\n--- island ---\n%s\n",
                    wa, wb);
        CHECK(strcmp(wa, wb) == 0);
        free(wa); free(wb);
    }
    {
        dl_lit q = dl_pos(intern_id(sy, "spotted(a,b)"));
        char *wa = why_str(A, q), *wb = why_str(B, q);
        CHECK(strcmp(wa, wb) == 0);
        free(wa); free(wb);
    }

    /* the guard tracks numeric state across a re-solve: hp(a) := 9 drops weak(a) */
    world_set_num(B, intern_id(sy, "hp(a)"), 9);
    CHECK(world_query(B, dl_pos(intern_id(sy, "weak(a)"))) == DL_REFUTED); /* dropped */

    story_matcher_free(M);
    world_free(A); world_free(B);
    intern_free(sy);
    return 1;
}

int main(void)
{
    if (!world_api_half()) return 1;
    if (!story_half()) return 1;
    if (!guard_half()) return 1;
    printf("test_matchview: all passed\n");
    return 0;
}
