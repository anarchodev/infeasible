/* Golden test for roll kinds (#82, the second half of its title): a value
 * declares `kind save`, and a KIND MODIFIER written once —
 *
 *     rule bless_save(A: actor): blessed(A) => kind save(A) = prior + 4
 *
 * — expands into a layer on EVERY value of that kind ("selection is static",
 * #79: the grounder quantifies over the kind). Expanded labels are
 * `<modifier>.<value>`, real rule labels, so #94's ordering demands are
 * satisfiable with ordinary superiority. Pinned:
 *  - one sentence hits members of different arities (subject = first arg);
 *  - expansions coexist unordered with value-specific layers of the same
 *    class, and mixed classes demand (and accept) explicit `>` on the
 *    expanded labels;
 *  - a rolled modifier clones per member: the d4 on one save is not the d4
 *    on another (distinct sites), while readers of one value still share;
 *  - misuse: no members, non-layer shapes, extra parameters, `>` on the
 *    unexpanded label, `kind` on stored state. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

static int step1(world *w, intern *sy, const char *action)
{
    uint32_t a = intern_id(sy, action);
    char err[128];
    int r = world_step(w, &a, 1, err, sizeof err);
    if (r) fprintf(stderr, "  step %s: %s\n", action, err);
    return r;
}

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

/* --- one sentence, every save — across arities; add-layers commute --- */
static int test_bless_all_saves(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, grik : actor )\n"
        "state ( blessed(actor)  wise(actor)  av(actor) : int  bv(actor) : int )\n"
        "value ( spell_save(actor)       : int kind save\n"
        "        contest(actor, actor)   : int kind save )\n"
        "rule base_ss(X: actor):            => spell_save(X) = 10\n"
        "rule base_ct(X: actor, Y: actor):  => contest(X, Y) = 8\n"
        "// the modifier, written once\n"
        "rule bless_save(A: actor): blessed(A) => kind save(A) = prior + 4\n"
        "// a value-specific add layer: same class, unordered, commutes\n"
        "rule wisdom(X: actor): wise(X) => spell_save(X) = prior + 1\n"
        "action snap(X: actor): causes av(X) := spell_save(X)\n"
        "                            & bv(X) := contest(X, grik)\n"
        "action b(X: actor): causes blessed(X)\n"
        "action w2(X: actor): causes wise(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 10);
    CHECK(num(w, sy, "bv(bran)") == 8);

    CHECK(step1(w, sy, "b(bran)") == 0);           /* bless bran */
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 14);           /* arity-1 member */
    CHECK(num(w, sy, "bv(bran)") == 12);           /* arity-2 member too */
    CHECK(step1(w, sy, "snap(grik)") == 0);
    CHECK(num(w, sy, "av(grik)") == 10);           /* per subject */

    CHECK(step1(w, sy, "w2(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 15);           /* +4 and +1, order-free */
    CHECK(num(w, sy, "bv(bran)") == 12);           /* wisdom is spell_save-only */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- mixed classes: rejected unordered, accepted via expanded labels --- */
static int test_expanded_ordering(void)
{
    const char *base =
        "sort actor\n"
        "entity bran : actor\n"
        "state ( blessed(actor)  lucky(actor)  av(actor) : int )\n"
        "value spell_save(actor) : int kind save\n"
        "rule base_ss(X: actor): => spell_save(X) = 10\n"
        "rule bless_save(A: actor): blessed(A) => kind save(A) = prior + 4\n"
        "rule floor_save(A: actor): lucky(A)   => kind save(A) = max(prior, 13)\n"
        "action snap(X: actor): causes av(X) := spell_save(X)\n"
        "action b(X: actor): causes blessed(X)\n"
        "action l(X: actor): causes lucky(X)\n";

    /* unordered add + max on one value: #94 refuses */
    {
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *w = story_compile(base, "t.story", sy, &d);
        bool found = false;
        for (int k = 0; k < d.count && !found; k++)
            found = strstr(d.items[k].msg, "not ordered") != NULL;
        CHECK((w == NULL || d.nerrors > 0) && found);
        if (w) world_free(w);
        intern_free(sy);
    }

    /* ordered on the EXPANDED label: legal, floor applies above the add */
    char src[2048];
    snprintf(src, sizeof src, "%s%s", base,
             "floor_save.spell_save > bless_save.spell_save\n");
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "l(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 13);           /* max(10, 13) */
    CHECK(step1(w, sy, "b(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 14);           /* max(10+4, 13) */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a rolled modifier: one die per (member, binding), cloned sites --- */
static int test_cloned_dice(void)
{
    const char *src =
        "sort actor\n"
        "entity bran : actor\n"
        "state ( blessed(actor)  waited  av(actor) : int  bv(actor) : int )\n"
        "value ( s1(actor) : int kind save\n"
        "        s2(actor) : int kind save )\n"
        "rule b1(X: actor): => s1(X) = 10\n"
        "rule b2(X: actor): => s2(X) = 10\n"
        "rule bd(A: actor): blessed(A) => kind save(A) = prior + roll(4, 9)\n"
        "action snap(X: actor): causes av(X) := s1(X) & bv(X) := s2(X)\n"
        "action b(X: actor): causes blessed(X) & waited\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    world_set_seed(w, 777uLL);
    CHECK(step1(w, sy, "b(bran)") == 0);

    bool saw_diff = false;
    for (int t = 0; t < 30; t++) {
        CHECK(step1(w, sy, "snap(bran)") == 0);
        long d1 = num(w, sy, "av(bran)") - 10;
        long d2 = num(w, sy, "bv(bran)") - 10;
        CHECK(d1 >= 1 && d1 <= 4);
        CHECK(d2 >= 1 && d2 <= 4);
        if (d1 != d2) saw_diff = true;             /* distinct sites, per member */
    }
    CHECK(saw_diff);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- misuse --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "sort actor\nentity a : actor\nstate blessed(actor)\n"
          "rule m(A: actor): blessed(A) => kind save(A) = prior + 4\n",
          "no value declares" },
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value s(actor) : int kind save\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor): blessed(A) => kind save(A) = 3\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "must mention `prior`" },
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value s(actor) : int kind save\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor, B: actor): blessed(A) => kind save(A) = prior + 1\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "exactly one parameter" },
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value s(actor) : int kind save\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor): blessed(A) => kind save(A) = prior + 1\n"
          "rule other(X: actor): blessed(X) => zz(X)\n"
          "m > other\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "target the expanded label" },
        { "sort actor\nentity a : actor\nstate hp(actor) : int kind save\n",
          "stored state" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        intern *sy = intern_new();
        story_diag di[16];
        story_diags dg = { di, 16, 0, 0 };
        world *w = story_compile(BAD[i].src, "t.story", sy, &dg);
        if (w != NULL && dg.nerrors == 0) {
            fprintf(stderr, "FAIL %s:%d: case %zu compiled but should not\n",
                    __FILE__, __LINE__, i);
            return 1;
        }
        bool found = false;
        for (int k = 0; k < dg.count && !found; k++)
            found = strstr(dg.items[k].msg, BAD[i].msg) != NULL;
        if (!found) {
            fprintf(stderr, "FAIL %s:%d: case %zu missing \"%s\"; got \"%s\"\n",
                    __FILE__, __LINE__, i, BAD[i].msg,
                    dg.count ? dg.items[0].msg : "(none)");
            return 1;
        }
        if (w) world_free(w);
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_bless_all_saves()) return 1;
    if (test_expanded_ordering()) return 1;
    if (test_cloned_dice()) return 1;
    if (test_errors()) return 1;
    printf("test_kinds: all passed\n");
    return 0;
}
