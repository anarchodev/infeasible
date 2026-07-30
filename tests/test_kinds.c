/* Golden test for kinds-are-facts (#124, EPIC #123 slice 1): a kind is the
 * boolean case of `value` over the built-in `value` meta-sort, membership is
 * `fact`s, and a modifier written once —
 *
 *     value save(value)
 *     fact ( save(spell_save)  save(contest) )
 *     rule bless_save(A: actor, V: value):
 *         save(V) & blessed(A) => V(A) = prior + 4
 *
 * — selects by ordinary body atoms and expands, per member, through #115's
 * layer machinery UNCHANGED (the functor variable `V(A)` is the HiLog move:
 * looks higher-order, grounds first-order). The prior semantic pins survive
 * the re-spell — that is the proof the machinery did:
 *  - one sentence hits members of different arities (subject = first arg);
 *  - expansions coexist unordered with value-specific layers of the same
 *    class, and mixed classes demand (and accept) explicit `>` on the
 *    expanded labels `<modifier>.<value>`;
 *  - a rolled modifier clones per member: the d4 on one save is not the d4
 *    on another (distinct sites), while readers of one value still share.
 *
 * New pins, the strictly-greater power: faceted selection (`save(V, dex)`
 * hits only dex-saves), product cross-cutting (Rage / Bracers / wand on one
 * attack space — the taxonomy is a product, not a tree), multi-membership
 * (one value in two kinds, both modifiers stack), two-role subjects
 * (`V(A, T)` — Dodge keys on the target), and the matches-nothing WARNING.
 *
 * Misuse: located errors for the meta-sort on stored state / providers /
 * `: int` values, an unselected functor variable, a non-`value` variable in
 * functor position, fact vocabulary (unknown kind, arity, non-members), kind
 * atoms in runtime rules, `value`-binders outside modifiers — and the grace
 * messages pointing the removed #115 `kind` keyword at the new spelling. */

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
        "value ( save(value)\n"
        "        spell_save(actor)       : int\n"
        "        contest(actor, actor)   : int )\n"
        "fact ( save(spell_save)  save(contest) )\n"
        "rule base_ss(X: actor):            => spell_save(X) = 10\n"
        "rule base_ct(X: actor, Y: actor):  => contest(X, Y) = 8\n"
        "// the modifier, written once\n"
        "rule bless_save(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 4\n"
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
        "value ( save(value)  spell_save(actor) : int )\n"
        "fact save(spell_save)\n"
        "rule base_ss(X: actor): => spell_save(X) = 10\n"
        "rule bless_save(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 4\n"
        "rule floor_save(A: actor, V: value): save(V) & lucky(A)   => V(A) = max(prior, 13)\n"
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

/* --- #145: kind-level superiority — order two modifiers once ------------- */
static int test_blanket_ordering(void)
{
    static const char *FMT =
        "sort actor\n"
        "entity bran : actor\n"
        "state ( blessed(actor)  lucky(actor)  av(actor) : int  bv(actor) : int )\n"
        "value ( save(value)  s1(actor) : int  s2(actor) : int )\n"
        "fact ( save(s1)  save(s2) )\n"
        "rule ba1(X: actor): => s1(X) = 10\n"
        "rule ba2(X: actor): => s2(X) = 10\n"
        "rule bless_save(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 4\n"
        "rule floor_save(A: actor, V: value): save(V) & lucky(A)   => V(A) = max(prior, 13)\n"
        "floor_save > bless_save\n"                /* ONE line, every member */
        "%s"
        "action snap(X: actor): causes av(X) := s1(X) & bv(X) := s2(X)\n"
        "action b(X: actor): causes blessed(X)\n"
        "action l(X: actor): causes lucky(X)\n";

    /* the blanket: floor above the add on BOTH members */
    {
        char src[2048];
        snprintf(src, sizeof src, FMT, "");
        intern *sy = intern_new();
        world *w = compile_ok(src, sy);
        CHECK(w != NULL);
        CHECK(step1(w, sy, "b(bran)") == 0);
        CHECK(step1(w, sy, "l(bran)") == 0);
        CHECK(step1(w, sy, "snap(bran)") == 0);
        CHECK(num(w, sy, "av(bran)") == 14);       /* max(10+4, 13) */
        CHECK(num(w, sy, "bv(bran)") == 14);
        world_free(w);
        intern_free(sy);
    }
    /* most-specific wins: an explicit dotted sup flips ONE member */
    {
        char src[2048];
        snprintf(src, sizeof src, FMT, "bless_save.s2 > floor_save.s2\n");
        intern *sy = intern_new();
        world *w = compile_ok(src, sy);
        CHECK(w != NULL);
        CHECK(step1(w, sy, "b(bran)") == 0);
        CHECK(step1(w, sy, "l(bran)") == 0);
        CHECK(step1(w, sy, "snap(bran)") == 0);
        CHECK(num(w, sy, "av(bran)") == 14);       /* blanket: max(10+4, 13) */
        CHECK(num(w, sy, "bv(bran)") == 17);       /* explicit: max(10,13)+4 */
        world_free(w);
        intern_free(sy);
    }
    /* a blanket over modifiers sharing no member warns and does nothing */
    {
        const char *src =
            "sort actor\nentity a : actor\n"
            "state ( blessed(actor)  q(actor) : int )\n"
            "value ( save(value)  atkk(value)  s(actor) : int  t(actor) : int )\n"
            "fact ( save(s)  atkk(t) )\n"
            "rule bs(X: actor): => s(X) = 10\n"
            "rule bt(X: actor): => t(X) = 10\n"
            "rule ms(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 1\n"
            "rule mt(A: actor, V: value): atkk(V) & blessed(A) => V(A) = max(prior, 2)\n"
            "mt > ms\n"
            "action p(X: actor): causes q(X) := s(X)\n";
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *w = story_compile(src, "t.story", sy, &d);
        CHECK(w != NULL && d.nerrors == 0);
        bool found = false;
        for (int k = 0; k < d.count && !found; k++)
            found = d.items[k].sev == STORY_WARNING &&
                    strstr(d.items[k].msg, "share no member") != NULL;
        CHECK(found);
        world_free(w);
        intern_free(sy);
    }
    return 0;
}

/* --- #143: cross-value links — Rage lands on the DAMAGE roll -------------
 * `dmg_of(value, value)` is a LINK predicate (a kind with two value
 * positions); the modifier binds a second value parameter through it, so
 * the selector runs over attacks while the modified value is the linked
 * damage roll. The attack itself is untouched. */
static int test_cross_value_links(void)
{
    const char *src =
        "sort actor\nentity bran : actor\n"
        "enum reach { melee, ranged }\n"
        "state ( raging(actor)  a1(actor) : int  d1v(actor) : int  d2v(actor) : int )\n"
        "value ( attack(value, reach)  dmg_of(value, value)\n"
        "        sword_atk(actor) : int  bow_atk(actor) : int\n"
        "        sword_dmg(actor) : int  bow_dmg(actor) : int )\n"
        "fact ( attack(sword_atk, melee)  attack(bow_atk, ranged)\n"
        "       dmg_of(sword_atk, sword_dmg)  dmg_of(bow_atk, bow_dmg) )\n"
        "rule w1(X: actor): => sword_atk(X) = 10\n"
        "rule w2(X: actor): => bow_atk(X) = 10\n"
        "rule w3(X: actor): => sword_dmg(X) = 10\n"
        "rule w4(X: actor): => bow_dmg(X) = 10\n"
        "rule rage(A: actor, V: value, W: value):\n"
        "    attack(V, melee) & dmg_of(V, W) & raging(A) => W(A) = prior + 2\n"
        "action snap(X: actor): causes a1(X) := sword_atk(X)\n"
        "                            & d1v(X) := sword_dmg(X) & d2v(X) := bow_dmg(X)\n"
        "action r(X: actor): causes raging(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "r(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "d1v(bran)") == 12);  /* melee-linked damage: +2      */
    CHECK(num(w, sy, "d2v(bran)") == 10);  /* ranged-linked: unselected    */
    CHECK(num(w, sy, "a1(bran)") == 10);   /* the ATTACK roll: untouched   */
    world_free(w);
    intern_free(sy);

    /* an unselected link parameter is a located error */
    {
        const char *bad =
            "sort actor\nentity a : actor\nstate ( raging(actor)  q(actor) : int )\n"
            "value ( attack(value)  s(actor) : int )\n"
            "fact attack(s)\n"
            "rule bs(X: actor): => s(X) = 10\n"
            "rule m(A: actor, V: value, W: value):\n"
            "    attack(V) & raging(A) => W(A) = prior + 2\n"
            "action p(X: actor): causes q(X) := s(X)\n";
        intern *sy2 = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *w2 = story_compile(bad, "t.story", sy2, &d);
        bool found = false;
        for (int k = 0; k < d.count && !found; k++)
            found = strstr(d.items[k].msg,
                           "value parameter 'W' is not selected") != NULL;
        CHECK((w2 == NULL || d.nerrors > 0) && found);
        if (w2) world_free(w2);
        intern_free(sy2);
    }
    return 0;
}

/* --- a rolled modifier: one die per (member, binding), cloned sites --- */
static int test_cloned_dice(void)
{
    const char *src =
        "sort actor\n"
        "entity bran : actor\n"
        "state ( blessed(actor)  waited  av(actor) : int  bv(actor) : int )\n"
        "value ( save(value)\n"
        "        s1(actor) : int\n"
        "        s2(actor) : int )\n"
        "fact ( save(s1)  save(s2) )\n"
        "rule b1(X: actor): => s1(X) = 10\n"
        "rule b2(X: actor): => s2(X) = 10\n"
        "rule bd(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + roll(4, 9)\n"
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

/* --- facets + products: the cross-cutting selection a flat kind can't say --- */
static int test_facets_and_products(void)
{
    const char *src =
        "sort actor\n"
        "enum ability { str, dex, wis }\n"
        "enum reach   { melee, ranged }\n"
        "enum source  { weapon, spell }\n"
        "entity bran : actor\n"
        "state ( raging(actor)  archer(actor)  warcaster(actor)  keen(actor)\n"
        "        v1(actor) : int  v2(actor) : int  v3(actor) : int  v4(actor) : int )\n"
        "value ( save(value, ability)\n"
        "        attack(value, reach, source)\n"
        "        dex_save(actor)  : int\n"
        "        wis_save(actor)  : int\n"
        "        sword(actor)     : int\n"
        "        bow(actor)       : int\n"
        "        firebolt(actor)  : int )\n"
        "fact ( save(dex_save, dex)  save(wis_save, wis)\n"
        "       attack(sword, melee, weapon)\n"
        "       attack(bow, ranged, weapon)\n"
        "       attack(firebolt, ranged, spell)\n"
        "       // multi-membership: the firebolt attack is also a dex-save-ish\n"
        "       // contest for the target — one value, two kinds\n"
        "       save(firebolt, dex) )\n"
        "rule bs1(X: actor): => dex_save(X) = 10\n"
        "rule bs2(X: actor): => wis_save(X) = 10\n"
        "rule bs3(X: actor): => sword(X)    = 10\n"
        "rule bs4(X: actor): => bow(X)      = 10\n"
        "rule bs5(X: actor): => firebolt(X) = 10\n"
        "// faceted: only dex-saves\n"
        "rule cat_grace(A: actor, V: value): save(V, dex) & keen(A) => V(A) = prior + 1\n"
        "// products cross-cutting one attack space on different axes\n"
        "rule rage(A: actor, V: value):    attack(V, melee, weapon)  & raging(A)    => V(A) = prior + 2\n"
        "rule bracers(A: actor, V: value): attack(V, ranged, weapon) & archer(A)    => V(A) = prior + 4\n"
        "rule wand(A: actor, V: value):    attack(V, _, spell)       & warcaster(A) => V(A) = prior + 8\n"
        "action snap(X: actor): causes v1(X) := dex_save(X) & v2(X) := sword(X)\n"
        "                            & v3(X) := bow(X)      & v4(X) := firebolt(X)\n"
        "action all(X: actor): causes raging(X) & archer(X) & warcaster(X) & keen(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "all(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "v1(bran)") == 11);   /* dex_save: cat_grace only        */
    CHECK(num(w, sy, "v2(bran)") == 12);   /* sword: rage only                */
    CHECK(num(w, sy, "v3(bran)") == 14);   /* bow: bracers only               */
    /* firebolt: wand (ranged,spell matches `_`,spell) AND cat_grace (it is
     * also a dex-save member) — multi-membership stacks, same add class */
    CHECK(num(w, sy, "v4(bran)") == 19);

    /* wis_save exists but nothing modifies it (keen is dex-facet only) */
    long wq = num(w, sy, "v1(bran)");
    (void)wq;

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- two-role subjects: V(A, T) — the Dodge shape keys on the target --- */
static int test_two_role_binding(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, grik : actor )\n"
        "state ( dodging(actor)  av(actor) : int  bv(actor) : int )\n"
        "value ( targeted(value)\n"
        "        grapple(actor, actor) : int )\n"
        "fact targeted(grapple)\n"
        "rule bg(X: actor, Y: actor): => grapple(X, Y) = 10\n"
        "// \"attacks against a dodging target\" — the guard reads the SECOND role\n"
        "rule dodge(A: actor, T: actor, V: value):\n"
        "    targeted(V) & dodging(T) => V(A, T) = prior + 5\n"
        "action snap(X: actor): causes av(X) := grapple(X, grik)\n"
        "                            & bv(X) := grapple(X, bran)\n"
        "action d(X: actor): causes dodging(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 10);
    CHECK(num(w, sy, "bv(bran)") == 10);

    CHECK(step1(w, sy, "d(grik)") == 0);           /* grik dodges */
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 15);           /* vs grik: modified */
    CHECK(num(w, sy, "bv(bran)") == 10);           /* vs bran: untouched */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a modifier that selects nothing is a WARNING, never silent --- */
static int test_matches_nothing_warning(void)
{
    const char *src =
        "sort actor\nentity a : actor\n"
        "state ( blessed(actor)  q(actor) : int )\n"
        "value ( save(value)  s(actor) : int )\n"
        "// no facts: the kind is empty\n"
        "rule bs(X: actor): => s(X) = 10\n"
        "rule m(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 4\n"
        "action p(X: actor): causes q(X) := s(X)\n";

    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    CHECK(w != NULL && d.nerrors == 0);            /* compiles */
    bool found = false;
    for (int k = 0; k < d.count && !found; k++)
        found = d.items[k].sev == STORY_WARNING &&
                strstr(d.items[k].msg, "matches no member") != NULL;
    CHECK(found);
    CHECK(step1(w, sy, "p(a)") == 0);              /* and still runs */
    CHECK(num(w, sy, "q(a)") == 10);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- misuse: located errors, and the grace messages for the old surface --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        /* the removed #115 keyword, both spellings, pointed at the new one */
        { "sort actor\nentity a : actor\n"
          "value s(actor) : int kind save\n",
          "keyword is gone" },
        { "sort actor\nentity a : actor\nstate blessed(actor)\n"
          "rule m(A: actor): blessed(A) => kind save(A) = prior + 4\n",
          "kind k(A)" },
        { "sort actor\nentity a : actor\nstate hp(actor) : int kind save\n",
          "keyword is gone" },
        /* the meta-sort never keys a runtime object */
        { "state marked(value)\n",
          "belong to kind predicates" },
        { "provider near(value)\n",
          "belong to kind predicates" },
        { "sort actor\nentity a : actor\nvalue s(value, actor) : int\n",
          "drop the `: int`" },
        /* functor discipline */
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value ( save(value)  s(actor) : int )\n"
          "fact save(s)\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor, V: value): blessed(A) => V(A) = prior + 1\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "not selected by any kind atom" },
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value ( save(value)  s(actor) : int )\n"
          "fact save(s)\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor): blessed(A) => A(A) = prior + 1\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "only a `value`-sorted parameter" },
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value ( save(value)  s(actor) : int )\n"
          "fact save(s)\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor, V: value): save(V) & blessed(A) => V(A) = 3\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "must mention `prior`" },
        /* sup targets the expanded label, as before */
        { "sort actor\nentity a : actor\nstate ( blessed(actor)  q(actor) : int )\n"
          "value ( save(value)  s(actor) : int )\n"
          "fact save(s)\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule m(A: actor, V: value): save(V) & blessed(A) => V(A) = prior + 1\n"
          "rule other(X: actor): blessed(X) => zz(X)\n"
          "m > other\n"
          "action p(X: actor): causes q(X) := s(X)\n",
          "target the expanded label" },
        /* the staging boundary: kinds are build-time */
        { "sort actor\nentity a : actor\nstate blessed(actor)\n"
          "value ( save(value)  s(actor) : int )\n"
          "fact save(s)\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule j(X: actor): save(X) => q(X)\n",
          "build-time only" },
        { "sort actor\nentity a : actor\nstate blessed(actor)\n"
          "value ( save(value)  s(actor) : int )\n"
          "rule bs(X: actor): => s(X) = 10\n"
          "rule j(V: value): blessed(a) => q(a)\n",
          "this rule is neither" },
        /* fact vocabulary */
        { "value s : int\nrule d: => s = 3\nfact zap(s)\n",
          "not a declared kind predicate" },
        { "value ( save(value)  s : int )\nrule d: => s = 3\n"
          "fact save(s, s)\n",
          "takes 1 arguments" },
        { "value ( save(value)  s : int )\nrule d: => s = 3\n"
          "fact save(nope)\n",
          "not a declared value" },
        { "sort actor\nentity a : actor\n"
          "enum ability { dex, wis }\n"
          "value ( save(value, ability)  s(actor) : int )\n"
          "rule d(X: actor): => s(X) = 10\n"
          "fact save(s, str)\n",
          "not a member of sort" },
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
    if (test_blanket_ordering()) return 1;
    if (test_cross_value_links()) return 1;
    if (test_cloned_dice()) return 1;
    if (test_facets_and_products()) return 1;
    if (test_two_role_binding()) return 1;
    if (test_matches_nothing_warning()) return 1;
    if (test_errors()) return 1;
    printf("test_kinds: all passed\n");
    return 0;
}
