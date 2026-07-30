/* Golden test for the derived/defeasible kind stratum (#125, EPIC #123
 * slice 2): "the kind stratum is a world with no step function" — membership
 * facts + kind rules over the sealed value domain, solved AT GROUNDING by the
 * same scalar DL engine, with the same verdicts and the same why-trace.
 *
 * Pinned:
 *  - derived hierarchy: a Luck-shaped modifier on `d20(V)` catches members
 *    arriving via BOTH the save and the attack route (overlapping taxonomies
 *    coexist — the thing a tag tree cannot say);
 *  - closed-world negation in the sealed stratum: `brutal` = physical damage
 *    type & ~magical — sound because the domain closes at world-build, and
 *    only for FACTS-ONLY kinds (negating a derived kind is a located error);
 *  - the defeasible taxonomy: the thrown dagger — `thrown_not_melee >
 *    melee_by_weapon` refutes the melee membership, the ranged one proves,
 *    and the build-time why renders the defeat with the ordinary trace
 *    format (story_compile_kinds_why);
 *  - the two-valued consumer: remove the superiority and the SAME program is
 *    a located UNDECIDED error naming the competing rules;
 *  - the staging boundary: a kind rule reading a fluent is a located error;
 *  - slice-1 equivalence: a modifier selecting over a DERIVED kind produces
 *    the same layers as the equivalent fact-only spelling. */

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

/* --- hierarchy: one modifier on d20 catches saves AND attacks ------------- */
static int test_derived_hierarchy(void)
{
    const char *src =
        "sort actor\nentity bran : actor\n"
        "state ( lucky(actor)  av(actor) : int  bv(actor) : int )\n"
        "value ( save(value)  attack(value)  d20(value)\n"
        "        ss(actor) : int  atk(actor) : int )\n"
        "fact ( save(ss)  attack(atk) )\n"
        "rule saves_roll_d20(V: value):   save(V)   => d20(V)\n"
        "rule attacks_roll_d20(V: value): attack(V) => d20(V)\n"
        "rule bs1(X: actor): => ss(X) = 10\n"
        "rule bs2(X: actor): => atk(X) = 10\n"
        "rule luck(A: actor, V: value): d20(V) & lucky(A) => V(A) = prior + 1\n"
        "action snap(X: actor): causes av(X) := ss(X) & bv(X) := atk(X)\n"
        "action l(X: actor): causes lucky(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "l(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 11);           /* via the save route */
    CHECK(num(w, sy, "bv(bran)") == 11);           /* via the attack route */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- closed-world negation: "nonmagical physical damage", one predicate --- */
static int test_brutal_negation(void)
{
    const char *src =
        "sort actor\nentity bran : actor\n"
        "enum dtype { bludg, pierc, slash, fire }\n"
        "state ( raging(actor)  v1(actor) : int  v2(actor) : int  v3(actor) : int )\n"
        "value ( damage(value, dtype)  magical(value)  brutal(value)\n"
        "        club(actor) : int  ray(actor) : int  mace(actor) : int )\n"
        "fact ( damage(club, bludg)  damage(ray, fire)\n"
        "       damage(mace, bludg)  magical(mace) )\n"
        "rule br(V: value, D: dtype):\n"
        "    damage(V, D) & D in {bludg, pierc, slash} & ~magical(V) => brutal(V)\n"
        "rule bc(X: actor): => club(X) = 10\n"
        "rule brr(X: actor): => ray(X) = 10\n"
        "rule bm(X: actor): => mace(X) = 10\n"
        "rule brute(A: actor, V: value): brutal(V) & raging(A) => V(A) = prior + 3\n"
        "action snap(X: actor): causes v1(X) := club(X) & v2(X) := ray(X)\n"
        "                            & v3(X) := mace(X)\n"
        "action r(X: actor): causes raging(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "r(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "v1(bran)") == 13);           /* bludgeoning, nonmagical */
    CHECK(num(w, sy, "v2(bran)") == 10);           /* fire: not physical */
    CHECK(num(w, sy, "v3(bran)") == 10);           /* magical: closed out */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the thrown dagger: defeasible taxonomy + the build-time why ---------- */
static const char *DAGGER_FMT =
    "sort actor\nentity bran : actor\n"
    "enum reach { melee, ranged }\n"
    "state ( raging(actor)  aiming(actor)  av(actor) : int )\n"
    "value ( attack(value, reach)  uses_melee_weapon(value)  thrown_mode(value)\n"
    "        dagger_throw(actor) : int )\n"
    "fact ( uses_melee_weapon(dagger_throw)  thrown_mode(dagger_throw) )\n"
    "rule melee_by_weapon(V: value):  uses_melee_weapon(V) => attack(V, melee)\n"
    "rule thrown_not_melee(V: value): thrown_mode(V)       => ~attack(V, melee)\n"
    "rule thrown_is_ranged(V: value): thrown_mode(V)       => attack(V, ranged)\n"
    "%s"                                           /* the superiority, or not */
    "rule bd(X: actor): => dagger_throw(X) = 10\n"
    "rule ranged_mod(A: actor, V: value): attack(V, ranged) & aiming(A) => V(A) = prior + 2\n"
    "rule melee_mod(A: actor, V: value):  attack(V, melee)  & raging(A) => V(A) = prior + 7\n"
    "action snap(X: actor): causes av(X) := dagger_throw(X)\n"
    "action go(X: actor): causes raging(X) & aiming(X)\n";

static int test_thrown_dagger(void)
{
    char src[4096];
    snprintf(src, sizeof src, DAGGER_FMT,
             "thrown_not_melee > melee_by_weapon\n");

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step1(w, sy, "go(bran)") == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(num(w, sy, "av(bran)") == 12);   /* ranged +2 landed; melee +7 did NOT */
    world_free(w);
    intern_free(sy);

    /* the build-time why: the ordinary trace, showing the defeat */
    sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    w = story_compile_kinds_why(src, "t.story", sy, &d,
                                "attack(dagger_throw,melee)", m);
    fclose(m);
    CHECK(w != NULL && d.nerrors == 0);
    CHECK(buf != NULL);
    CHECK(strstr(buf, "attack(dagger_throw,melee)") != NULL);
    CHECK(strstr(buf, "REFUTED") != NULL);         /* the membership fell */
    CHECK(strstr(buf, "melee_by_weapon[V=dagger_throw]") != NULL);
    CHECK(strstr(buf, "thrown_not_melee[V=dagger_throw]") != NULL);
    CHECK(strstr(buf, "t.story:") != NULL);        /* provenance spans ride */
    free(buf);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- remove the superiority: the same program is a located UNDECIDED ------ */
static int test_undecided_error(void)
{
    char src[4096];
    snprintf(src, sizeof src, DAGGER_FMT, "");

    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    CHECK(w == NULL && d.nerrors >= 1);
    bool found = false;
    for (int k = 0; k < d.count && !found; k++)
        found = strstr(d.items[k].msg, "CONTESTED at world-build") != NULL &&
                strstr(d.items[k].msg, "attack(dagger_throw,melee)") != NULL;
    CHECK(found);
    if (w) world_free(w);
    intern_free(sy);
    return 0;
}

/* --- slice-1 equivalence: derived and fact-only spellings expand alike ---- */
static int test_derived_equals_facts(void)
{
    static const char *FMT =
        "sort actor\nentity bran : actor\n"
        "state ( lucky(actor)  av(actor) : int )\n"
        "value ( save(value)  base(value)  ss(actor) : int )\n"
        "%s"
        "rule bs(X: actor): => ss(X) = 10\n"
        "rule luck(A: actor, V: value): save(V) & lucky(A) => V(A) = prior + 1\n"
        "action snap(X: actor): causes av(X) := ss(X)\n"
        "action l(X: actor): causes lucky(X)\n";
    static const char *SPELLING[2] = {
        "fact save(ss)\n",                             /* facts-only */
        "fact base(ss)\n"
        "rule sfb(V: value): base(V) => save(V)\n",    /* derived */
    };
    long got[2];
    for (int i = 0; i < 2; i++) {
        char src[2048];
        snprintf(src, sizeof src, FMT, SPELLING[i]);
        intern *sy = intern_new();
        world *w = compile_ok(src, sy);
        CHECK(w != NULL);
        CHECK(step1(w, sy, "l(bran)") == 0);
        CHECK(step1(w, sy, "snap(bran)") == 0);
        got[i] = num(w, sy, "av(bran)");
        world_free(w);
        intern_free(sy);
    }
    CHECK(got[0] == 11 && got[1] == 11);           /* identical layers */
    return 0;
}

/* --- misuse: staging boundary, derived negation, mixed superiority -------- */
static int expect_error(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int ok = w == NULL && d.nerrors >= 1;
    bool found = false;
    for (int k = 0; k < d.count && !found; k++)
        found = strstr(d.items[k].msg, needle) != NULL;
    if (!ok || !found)
        fprintf(stderr, "FAIL expected \"%s\": got %s\n", needle,
                d.count ? d.items[0].msg : "(none)");
    if (w) world_free(w);
    intern_free(sy);
    return (ok && found) ? 0 : 1;
}

static int test_errors(void)
{
    /* a kind rule reading runtime state */
    if (expect_error(
            "sort actor\nentity a : actor\nstate blessed(actor)\n"
            "value ( save(value)  d20(value)  s(actor) : int )\n"
            "fact save(s)\n"
            "rule bs(X: actor): => s(X) = 10\n"
            "rule bad(V: value): save(V) & blessed(a) => d20(V)\n",
            "cannot read 'blessed'")) return 1;
    /* negating a DERIVED kind */
    if (expect_error(
            "value ( save(value)  d20(value)  quiet(value)  s : int )\n"
            "fact save(s)\n"
            "rule ds(V: value): save(V) => d20(V)\n"
            "rule q(V: value): save(V) & ~d20(V) => quiet(V)\n"
            "rule bs: => s = 10\n",
            "not closed-world")) return 1;
    /* ordering a kind rule against a runtime rule */
    if (expect_error(
            "sort actor\nentity a : actor\nstate blessed(actor)\n"
            "value ( save(value)  d20(value)  s(actor) : int )\n"
            "fact save(s)\n"
            "rule ds(V: value): save(V) => d20(V)\n"
            "rule bs(X: actor): => s(X) = 10\n"
            "rule j(X: actor): blessed(X) => zz(X)\n"
            "ds > j\n",
            "orders only against kind rules")) return 1;
    return 0;
}

int main(void)
{
    if (test_derived_hierarchy())    return 1;
    if (test_brutal_negation())      return 1;
    if (test_thrown_dagger())        return 1;
    if (test_undecided_error())      return 1;
    if (test_derived_equals_facts()) return 1;
    if (test_errors())               return 1;
    printf("test_taxonomy: all passed\n");
    return 0;
}
