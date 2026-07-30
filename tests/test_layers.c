/* Golden test for layered value definitions with `prior` (#82) and their
 * well-formedness (#94).
 *
 * A value now takes ONE unconditional base plus guarded definitions that
 * either OVERRIDE (no `prior`) or LAYER (`prior + e`, `max(prior, e)`,
 * `min(prior, e)`), ordered by the existing superiority relation. Each
 * guarded definition grounds a MARKER judgment (`body => label(binding)`) —
 * an ordinary defeasible literal, queryable and why-traceable — and every
 * read site inlines the branch-free chain program
 *     v' = v + test(marker)·(f(v) − v)
 * (§5.8's evaluate-all-and-mask shape). Pinned:
 *  - the epic's AC flagship: overrides select by superiority, per entity;
 *  - unordered same-class add layers commute (order-free by construction);
 *  - `max(prior, …)` — Reliable Talent — floors a rolled check;
 *  - advantage as a LAYER (`max(prior, roll(20,3))`), with two readers of
 *    the value agreeing every tick (markers and dice shared across reads);
 *  - markers are ordinary judgments (queryable, `unless`-defeatable);
 *  - #94 rejections: no base, two bases, unordered overrides, unordered
 *    mixed-class layers, `prior` misuse. */

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

static dl_verdict q(world *w, intern *sy, const char *atom)
{
    return world_query(w, (dl_lit){ intern_id(sy, atom), false });
}

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

/* --- the epic's flagship: AC as base + two overrides under superiority --- */
static int test_ac_chain(void)
{
    const char *src =
        "sort unit\n"
        "entity ( u0, u1 : unit )\n"
        "state (\n"
        "    worn(unit)  mage(unit)  waited\n"
        "    dex(unit) : int\n"
        "    acv(unit) : int\n"
        ")\n"
        "init ( dex(u0) = 2  dex(u1) = 1  worn(u1) )\n"
        "value ac(unit) : int\n"
        "rule unarmored(X: unit):          => ac(X) = 10 + dex(X)\n"
        "rule armored(X: unit):    worn(X) => ac(X) = 14 + dex(X)\n"
        "rule mage_armor(X: unit): mage(X) => ac(X) = 13 + dex(X)\n"
        "mage_armor > armored\n"
        "// the value read in a judgment guard (markers via the two-phase solve)\n"
        "rule acq(X: unit): ac(X) >= 13 => tanky(X)\n"
        "// …and in an effect: snapshot into stored state so tests see numbers\n"
        "action snap(X: unit):    causes acv(X) := ac(X)\n"
        "action wear(X: unit):    causes worn(X)\n"
        "action strip(X: unit):   causes ~worn(X)\n"
        "exclusive wear(X), strip(X)\n"
        "action enchant(X: unit): causes mage(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    /* base vs override, per entity */
    CHECK(q(w, sy, "tanky(u0)") == DL_REFUTED);    /* 10+2 = 12 */
    CHECK(q(w, sy, "tanky(u1)") == DL_PROVED);     /* 14+1 = 15 */
    /* the marker is an ordinary judgment — queryable */
    CHECK(q(w, sy, "armored(u1)") == DL_PROVED);
    CHECK(q(w, sy, "armored(u0)") == DL_REFUTED);

    CHECK(step1(w, sy, "snap(u0)") == 0);
    CHECK(num(w, sy, "acv(u0)") == 12);
    CHECK(step1(w, sy, "snap(u1)") == 0);
    CHECK(num(w, sy, "acv(u1)") == 15);

    /* mage armor ABOVE armored: both apply, the higher override wins */
    CHECK(step1(w, sy, "enchant(u1)") == 0);
    CHECK(step1(w, sy, "snap(u1)") == 0);
    CHECK(num(w, sy, "acv(u1)") == 14);            /* 13+1, not 14+1 */

    /* armor off, enchantment stays */
    CHECK(step1(w, sy, "strip(u1)") == 0);
    CHECK(step1(w, sy, "snap(u1)") == 0);
    CHECK(num(w, sy, "acv(u1)") == 14);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- unordered same-class layers commute; ordered mixed classes compose --- */
static int test_layers_compose(void)
{
    const char *src =
        "sort unit\n"
        "entity u : unit\n"
        "state ( bless(unit)  rage(unit)  lucky(unit)  av(unit) : int )\n"
        "value atk(unit) : int\n"
        "rule base_atk(X: unit):           => atk(X) = 2\n"
        "rule blessed(X: unit):   bless(X) => atk(X) = prior + 3\n"
        "rule raging(X: unit):    rage(X)  => atk(X) = prior + 1\n"
        "// a max layer ABOVE the adds (ordered explicitly, mixed class)\n"
        "rule floor10(X: unit):   lucky(X) => atk(X) = max(prior, 10)\n"
        "floor10 > blessed\n"
        "floor10 > raging\n"
        "action snap(X: unit):  causes av(X) := atk(X)\n"
        "action b(X: unit): causes bless(X)\n"
        "action r(X: unit): causes rage(X)\n"
        "action l(X: unit): causes lucky(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 2);               /* base */
    CHECK(step1(w, sy, "b(u)") == 0);
    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 5);               /* +3 */
    CHECK(step1(w, sy, "r(u)") == 0);
    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 6);               /* +3 +1, order-free */
    CHECK(step1(w, sy, "l(u)") == 0);
    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 10);              /* max(6, 10) on top */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- Reliable Talent + advantage-as-a-layer, dice shared across readers --- */
static int test_rolled_layers(void)
{
    const char *src =
        "sort unit\n"
        "entity u : unit\n"
        "state ( reliable(unit)  advantage(unit)  waited )\n"
        "value chk(unit) : int\n"
        "rule base_chk(X: unit):                => chk(X) = roll(20)\n"
        "rule talent(X: unit):    reliable(X)   => chk(X) = max(prior, 10)\n"
        "rule adv(X: unit):       advantage(X)  => chk(X) = max(prior, roll(20, 3))\n"
        "adv > talent\n"
        "rule decent(X: unit):  chk(X) >= 10 => decent(X)\n"
        "rule decent2(X: unit): chk(X) >= 10 => decent2(X)\n"
        "action wait: causes waited\n"
        "action learn(X: unit): causes reliable(X)\n"
        "action focus(X: unit): causes advantage(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    world_set_seed(w, 5150uLL);

    bool saw_low = false;
    for (int t = 0; t < 30; t++) {                 /* raw d20: both outcomes */
        dl_verdict a = q(w, sy, "decent(u)"), b = q(w, sy, "decent2(u)");
        CHECK(a == b);                             /* two readers, one chain */
        if (a != DL_PROVED) saw_low = true;
        CHECK(step1(w, sy, "wait") == 0);
    }
    CHECK(saw_low);

    CHECK(step1(w, sy, "learn(u)") == 0);
    for (int t = 0; t < 30; t++) {                 /* floored at 10: always */
        CHECK(q(w, sy, "decent(u)") == DL_PROVED);
        CHECK(step1(w, sy, "wait") == 0);
    }
    CHECK(step1(w, sy, "focus(u)") == 0);          /* advantage stacks above */
    for (int t = 0; t < 10; t++) {
        CHECK(q(w, sy, "decent(u)") == DL_PROVED);
        CHECK(q(w, sy, "decent2(u)") == DL_PROVED);
        CHECK(step1(w, sy, "wait") == 0);
    }

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a definition's `unless` guard defeats its marker --- */
static int test_marker_unless(void)
{
    const char *src =
        "sort unit\n"
        "entity u : unit\n"
        "state ( worn(unit)  broken(unit)  av(unit) : int )\n"
        "init worn(u)\n"
        "value ac(unit) : int\n"
        "rule bare(X: unit):            => ac(X) = 10\n"
        "rule plate(X: unit):   worn(X) => ac(X) = 16 unless broken(X)\n"
        "action snap(X: unit):  causes av(X) := ac(X)\n"
        "action smash(X: unit): causes broken(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 16);
    CHECK(step1(w, sy, "smash(u)") == 0);
    CHECK(step1(w, sy, "snap(u)") == 0);
    CHECK(num(w, sy, "av(u)") == 10);              /* the defeater ate the layer */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- #94 rejections --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        /* two overrides that can both apply, unordered */
        { "sort unit\nentity u : unit\nstate ( a(unit)  b(unit) )\n"
          "value v(unit) : int\n"
          "rule base(X: unit):        => v(X) = 1\n"
          "rule oa(X: unit):   a(X)   => v(X) = 2\n"
          "rule ob(X: unit):   b(X)   => v(X) = 3\n"
          "rule r(X: unit): v(X) >= 2 => big(X)\n",
          "not ordered" },
        /* mixed classes (add vs max) unordered */
        { "sort unit\nentity u : unit\nstate ( a(unit)  b(unit) )\n"
          "value v(unit) : int\n"
          "rule base(X: unit):        => v(X) = 1\n"
          "rule la(X: unit):   a(X)   => v(X) = prior + 2\n"
          "rule lb(X: unit):   b(X)   => v(X) = max(prior, 5)\n"
          "rule r(X: unit): v(X) >= 2 => big(X)\n",
          "not ordered" },
        /* `prior` in the base */
        { "sort unit\nentity u : unit\nstate p\nvalue v(unit) : int\n"
          "rule base(X: unit): => v(X) = prior + 1\n"
          "rule r(X: unit): v(X) >= 2 => big(X)\n",
          "nothing beneath" },
        /* `prior` outside a definition */
        { "state ( gold : int )\naction a: causes gold += prior\n",
          "only meaningful inside a value definition" },
        /* superiority cycle among definitions */
        { "sort unit\nentity u : unit\nstate ( a(unit)  b(unit) )\n"
          "value v(unit) : int\n"
          "rule base(X: unit):      => v(X) = 1\n"
          "rule oa(X: unit): a(X)   => v(X) = prior + 2\n"
          "rule ob(X: unit): b(X)   => v(X) = prior + 3\n"
          "oa > ob\nob > oa\n"
          "rule r(X: unit): v(X) >= 2 => big(X)\n",
          "cyclic" },
        /* superiority across two different values */
        { "value ( x : int  y : int )\nstate p\n"
          "rule bx: => x = 1\nrule ox: p => x = 2\n"
          "rule by: => y = 1\nrule oy: p => y = 2\n"
          "ox > oy\n"
          "rule r: x >= 1 => q\nrule r2: y >= 1 => q2\n",
          "two DIFFERENT values" },
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
    if (test_ac_chain()) return 1;
    if (test_layers_compose()) return 1;
    if (test_rolled_layers()) return 1;
    if (test_marker_unless()) return 1;
    if (test_errors()) return 1;
    printf("test_layers: all passed\n");
    return 0;
}
