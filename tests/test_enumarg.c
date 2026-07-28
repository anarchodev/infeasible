/* Golden test for #96 (enum values in argument position) + #95 (finite-domain
 * membership in bodies).
 *
 * #96: an enum is a finite, declared, ground domain — the compiler synthesizes
 * a sort per enum whose "entities" are its values, so `resistant(actor,
 * damage_type)`, `D : damage_type` rule variables, and ground values in
 * argument position (`vulnerable(X, fire)`) all ride the existing odometer.
 * Values are NOT entities: declaring one is a located error.
 *
 * #95: `D in { a, b, c }` / `D not in { … }` is a STATIC grounding filter —
 * sugar over writing one rule per alternative ("many rules, one head" is
 * already the disjunction; this is ergonomics). It grounds to exactly the
 * instances the hand-written rules would produce: nothing enters the fixpoint,
 * a failed test drops the instance, a held test drops the conjunct. The SRD's
 * rage — resistance to three damage types — is the flagship: one sentence. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

static int step(world *w, intern *sy, const char *action)
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

/* --- the rage pattern: one sentence over a damage-type group --- */
static int test_rage(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, mora : actor )\n"
        "enum damage_type { slashing, piercing, bludgeoning, fire, cold, poison }\n"
        "state ( raging(actor)  marked(actor, damage_type) )\n"
        "init raging(bran)\n"
        "// the SRD's rage, written once: membership filters the grounding\n"
        "rule rage_resist(X: actor, D: damage_type):\n"
        "    raging(X) & D in { slashing, piercing, bludgeoning } => resistant(X, D)\n"
        "// a ground enum value in argument position\n"
        "rule brittle(X: actor): raging(X) => vulnerable(X, fire)\n"
        "// membership-only body: grounds to an unconditional conclusion per member\n"
        "rule tough(D: damage_type): D not in { fire } => shrugs(D)\n"
        "// enum value in an effect argument — stored state keyed by an enum\n"
        "action mark(T: actor): causes marked(T, cold)\n"
        "action calm(X: actor): causes ~raging(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "resistant(bran,slashing)") == DL_PROVED);
    CHECK(q(w, sy, "resistant(bran,piercing)") == DL_PROVED);
    CHECK(q(w, sy, "resistant(bran,bludgeoning)") == DL_PROVED);
    /* the filtered instance behaves EXACTLY like the rule the author would not
     * have written: no rule mentions resistant(bran,fire), so it is UNDECIDED —
     * the same verdict the hand-written three-rule version gives (derived
     * conclusions are not closed-world; declared state fluents below are) */
    CHECK(q(w, sy, "resistant(bran,fire)") == DL_UNDECIDED);
    CHECK(q(w, sy, "resistant(mora,slashing)") == DL_REFUTED); /* rule exists, body fails */
    CHECK(q(w, sy, "vulnerable(bran,fire)") == DL_PROVED);
    CHECK(q(w, sy, "shrugs(cold)") == DL_PROVED);
    CHECK(q(w, sy, "shrugs(poison)") == DL_PROVED);
    CHECK(q(w, sy, "shrugs(fire)") == DL_UNDECIDED);           /* `not in` excluded it */

    CHECK(step(w, sy, "mark(bran)") == 0);
    CHECK(q(w, sy, "marked(bran,cold)") == DL_PROVED);         /* enum-keyed state */
    CHECK(q(w, sy, "marked(bran,fire)") == DL_REFUTED);
    CHECK(step(w, sy, "calm(bran)") == 0);
    CHECK(q(w, sy, "marked(bran,cold)") == DL_PROVED);         /* inertia holds */
    CHECK(q(w, sy, "resistant(bran,slashing)") == DL_REFUTED); /* judgment followed state */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- membership inside an `unless` guard --- */
static int test_unless_membership(void)
{
    const char *src =
        "enum damage_type { fire, cold, poison }\n"
        "state threatened\n"
        "init threatened\n"
        "rule hurt_all(D: damage_type): threatened => hurt(D) unless D in { poison }\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "hurt(fire)") == DL_PROVED);
    CHECK(q(w, sy, "hurt(cold)") == DL_PROVED);
    /* for D=poison the guard is statically true: an unconditional defeat */
    CHECK(q(w, sy, "hurt(poison)") != DL_PROVED);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- superiority across membership-filtered instances --- */
static int test_sup_with_filtered(void)
{
    const char *base =
        "enum damage_type { fire, cold, poison }\n"
        "state p\n"
        "init p\n"
        "rule yes(D: damage_type): p & D in { fire, cold } => ok(D)\n"
        "rule no(D: damage_type):  p => ~ok(D)\n";
    char src[512];

    /* no > yes: the attacker wins where both exist */
    snprintf(src, sizeof src, "%sno > yes\n", base);
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "ok(fire)") == DL_REFUTED);
    CHECK(q(w, sy, "ok(poison)") == DL_REFUTED);
    world_free(w); intern_free(sy);

    /* yes > no: yes wins where it exists; where filtered, `no` is unopposed */
    snprintf(src, sizeof src, "%syes > no\n", base);
    sy = intern_new();
    w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "ok(fire)") == DL_PROVED);
    CHECK(q(w, sy, "ok(cold)") == DL_PROVED);
    CHECK(q(w, sy, "ok(poison)") == DL_REFUTED);   /* yes(poison) never grounded */
    world_free(w); intern_free(sy);
    return 0;
}

/* --- an enum as an MV fluent TYPE and an argument sort in one story --- */
static int test_type_and_arg_coexist(void)
{
    const char *src =
        "sort actor\n"
        "entity bran : actor\n"
        "enum school { evocation, abjuration }\n"
        "state (\n"
        "    tradition(actor) : school\n"        /* enum in VALUE position (§5.7) */
        "    knows(actor, school)\n"             /* enum in ARGUMENT position (#96) */
        ")\n"
        "init ( tradition(bran) = evocation  knows(bran, abjuration) )\n"
        "rule evoker(X: actor): tradition(X) = evocation => evoker(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "evoker(bran)") == DL_PROVED);
    CHECK(q(w, sy, "knows(bran,abjuration)") == DL_PROVED);
    CHECK(q(w, sy, "knows(bran,evocation)") == DL_REFUTED);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- an enum-typed `for each` binder with a membership `where` --- */
static int test_binder_membership(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, mora : actor )\n"
        "enum damage_type { fire, cold, poison }\n"
        "state ( wet(actor)  soaked(actor, damage_type) )\n"
        "init wet(bran)\n"
        "action splash:\n"
        "    causes for each T : actor, D : damage_type\n"
        "        where wet(T) & D in { fire, cold } : soaked(T, D)\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(step(w, sy, "splash") == 0);
    CHECK(q(w, sy, "soaked(bran,fire)") == DL_PROVED);
    CHECK(q(w, sy, "soaked(bran,cold)") == DL_PROVED);
    CHECK(q(w, sy, "soaked(bran,poison)") == DL_REFUTED);   /* filtered statically */
    CHECK(q(w, sy, "soaked(mora,fire)") == DL_REFUTED);     /* wet(T) failed at tick */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- misuse is a located error --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "enum damage_type { fire, cold }\nentity x : damage_type\n",
          "not entities" },
        { "sort school\nenum school { evocation, abjuration }\n",
          "both a sort and an enum" },
        { "enum damage_type { fire, cold }\nstate p\n"
          "rule r(D: damage_type): p & D in { fire, banana } => q(D)\n",
          "not a value of" },
        { "state p\nrule r: p & Z in { fire } => q\n",
          "must be a bound variable" },
        { "enum damage_type { fire, cold }\nstate p\n"
          "action a(D: damage_type): causes D in { fire }\n",
          "can't appear in a `causes` clause" },
        { "enum damage_type { fire, cold }\ninit x in { fire }\n",
          "not a fact" },
        { "sort actor\nentity fire : actor\nenum damage_type { fire, cold }\n",
          "duplicate entity" },
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
    if (test_rage()) return 1;
    if (test_unless_membership()) return 1;
    if (test_sup_with_filtered()) return 1;
    if (test_type_and_arg_coexist()) return 1;
    if (test_binder_membership()) return 1;
    if (test_errors()) return 1;
    printf("test_enumarg: all passed\n");
    return 0;
}
