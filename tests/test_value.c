/* Golden test for engine-derived values, slice 1 of #82: `value v(…) : int`
 * declarations + one unconditional definition per value
 * (`rule L(…): => v(…) = expr`), inlined at every read site.
 *
 * The headline semantic is ROLL SHARING (§5.10): a value's definition is one
 * expression tree, so every reader of `v(a,b)` emits the same EX_ROLL node
 * under the same binding — the same site key — and therefore sees the SAME
 * draw within a tick. One die, testable twice: the crit pattern D&D needs
 * (`atk_roll >= 20` must test the die that resolved the hit) and the thing
 * node-keyed sites made inexpressible. Distinct bindings still key apart and
 * draw independently (the property fireball's per-target saves rely on).
 * Verdict-level consistency is pinned over many ticks, so an accidental pass
 * from two independent dice agreeing is (1/2)^40-improbable, not plausible. */

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

/* --- one die, many readers: sharing, arithmetic, and the crit pattern --- */
static int test_shared_d20(void)
{
    const char *src =
        "sort actor\n"
        "entity ( bran, grik, mora : actor )\n"
        "state (\n"
        "    waited\n"
        "    atk(actor) : int\n"
        "    ac(actor)  : int\n"
        ")\n"
        "init ( atk(bran) = 3  ac(grik) = 12  ac(mora) = 12 )\n"
        "value atk_roll(actor, actor) : int\n"
        "rule the_d20(A: actor, T: actor): => atk_roll(A, T) = roll(20)\n"
        "// the die used in guard arithmetic…\n"
        "rule hit(A: actor, T: actor):  atk_roll(A, T) + atk(A) >= ac(T) => hit(A, T)\n"
        "// …is the same die tested alone (hit ≡ ge9 iff both read ONE roll)\n"
        "rule ge9(A: actor, T: actor):  atk_roll(A, T) >= 9  => ge9(A, T)\n"
        "// the crit: the same d20 that resolved the hit, tested against 20\n"
        "rule crit(A: actor, T: actor): atk_roll(A, T) >= 20 => crit(A, T)\n"
        "rule hi(A: actor, T: actor):   atk_roll(A, T) >= 11 => hi(A, T)\n"
        "rule hi2(A: actor, T: actor):  atk_roll(A, T) >= 11 => hi2(A, T)\n"
        "action wait: causes waited\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    world_set_seed(w, 0xC0FFEEuLL);

    bool saw_hi = false, saw_lo = false, saw_split = false, saw_crit = false;
    for (int t = 0; t < 60; t++) {
        dl_verdict vhit  = q(w, sy, "hit(bran,grik)");
        dl_verdict vge9  = q(w, sy, "ge9(bran,grik)");
        dl_verdict vcrit = q(w, sy, "crit(bran,grik)");
        dl_verdict vhi   = q(w, sy, "hi(bran,grik)");
        dl_verdict vhi2  = q(w, sy, "hi2(bran,grik)");
        dl_verdict vhim  = q(w, sy, "hi(bran,mora)");

        CHECK(vhi == vhi2);            /* two rules, one die */
        CHECK(vhit == vge9);           /* +3 >= 12  ≡  >= 9: shared through arithmetic */
        if (vcrit == DL_PROVED) {      /* a nat 20 always hits and is always `hi` */
            CHECK(vhit == DL_PROVED);
            CHECK(vhi == DL_PROVED);
            saw_crit = true;
        }
        if (vhi == DL_PROVED) saw_hi = true; else saw_lo = true;
        if (vhi != vhim) saw_split = true;   /* distinct bindings draw independently */

        CHECK(step(w, sy, "wait") == 0);
    }
    CHECK(saw_hi && saw_lo);           /* the die actually varies across ticks */
    CHECK(saw_split);                  /* …and per binding within a tick */
    CHECK(saw_crit);                   /* 60 ticks of d20: a 20 shows up (seeded, deterministic) */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the same draw serves a judgment guard and an effect in one tick --- */
static int test_guard_effect_coherence(void)
{
    const char *src =
        "sort actor\n"
        "entity grik : actor\n"
        "state ( struck  hp(actor) : int in 0 .. 200 )\n"
        "init hp(grik) = 200\n"
        "value dmg(actor) : int\n"
        "rule the_d6(T: actor): => dmg(T) = roll(6) + 2\n"
        "rule heavy(T: actor): dmg(T) >= 6 => heavy(T)\n"
        "action strike(T: actor): causes hp(T) -= dmg(T) & struck\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    world_set_seed(w, 7uLL);
    uint32_t hp = intern_id(sy, "hp(grik)");

    bool saw_heavy = false, saw_light = false;
    for (int t = 0; t < 25; t++) {
        dl_verdict vheavy = q(w, sy, "heavy(grik)");   /* judged pre-step, tick T */
        long before = world_get_num(w, hp);
        CHECK(step(w, sy, "strike(grik)") == 0);       /* effect drawn in tick T too */
        long dealt = before - world_get_num(w, hp);
        CHECK(dealt >= 3 && dealt <= 8);               /* roll(6)+2 */
        CHECK((dealt >= 6) == (vheavy == DL_PROVED));  /* same die both places */
        if (dealt >= 6) saw_heavy = true; else saw_light = true;
    }
    CHECK(saw_heavy && saw_light);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- values reading values; a constant value folds into effects --- */
static int test_nested_and_folded(void)
{
    const char *src =
        "state ( waited  gold : int )\n"
        "init gold = 10\n"
        "value ( base : int  tot : int  bonus : int )\n"
        "rule b0: => base = roll(4)\n"
        "rule t0: => tot = base + 1\n"
        "rule k0: => bonus = 3\n"
        "rule big:  tot >= 3  => big_pot\n"
        "rule big2: base >= 2 => big_pot2\n"
        "action pay: causes gold += bonus & waited\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    world_set_seed(w, 99uLL);
    uint32_t gold = intern_id(sy, "gold");

    bool saw_big = false, saw_small = false;
    for (int t = 0; t < 40; t++) {
        /* tot = base + 1, so tot >= 3 ≡ base >= 2 — iff `tot` inlines `base`'s
         * definition and both land on base's ONE die */
        dl_verdict a = q(w, sy, "big_pot"), b = q(w, sy, "big_pot2");
        CHECK(a == b);
        if (a == DL_PROVED) saw_big = true; else saw_small = true;
        CHECK(step(w, sy, "pay") == 0);
    }
    CHECK(saw_big && saw_small);
    CHECK(world_get_num(w, gold) == 10 + 40 * 3);   /* the folded constant value */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- misuse is a located error, never silence --- */
static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "state p\nvalue v : int\nrule r: v >= 1 => q\nrule s: p => q\n",
          "has no definition" },
        { "value v : int\nrule d1: => v = 3\nrule d2: => v = 4\n"
          "rule r: v >= 1 => q\n",
          "two unconditional definitions" },
        /* a value with only guarded definitions is PARTIAL since #116
         * (inferred, no keyword — pinned in test_partial.c); but one whose
         * every definition reads `prior` can never be defined at all */
        { "state p\nvalue v : int\nrule d: p => v = prior + 1\n"
          "rule r: v >= 1 => q\n",
          "can never be defined" },
        { "value v : int\nrule d: -> v = 3\nrule r: v >= 1 => q\n",
          "write '=>'" },
        { "value v : int\nrule d: => v = 3\naction a: causes v := 4\n",
          "cannot be written" },
        { "value ( x : int  y : int )\nrule dx: => x = y + 1\n"
          "rule dy: => y = x + 1\nrule r: x >= 1 => q\n",
          "cannot be cyclic" },
        { "state p\nvalue v : int\nrule d1: => v = 3\nrule q1: p => q\n"
          "d1 > q1\nrule use: v >= 1 => used\n",
          "mixes a value definition with an ordinary rule" },
        { "value v : int in 0..5\nrule d: => v = 3\n",
          "no clamp range" },
        { "sort actor\nentity a : actor\nvalue w(actor, actor) : int\n"
          "rule dw(X: actor, Y: actor): => w(X, X) = 1\n"
          "rule r: w(a, a) >= 1 => q\n",
          "distinct rule parameter" },
        { "state p\nrule empty: => q\n",
          "a rule needs a body" },
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

/* --- a derived number a CLIENT can read (#82) -----------------------------
 *
 * The engine has derived booleans (judgments) and stored numbers (fluents).
 * This is the third thing, and it exists so arithmetic can stay in the story
 * instead of migrating into whichever client draws it: a bar's width is
 * `48 * hp / hp_max`, and with nothing to ask for that number, the only place
 * that expression can live is inside the widget — one formula, one source
 * fluent, frozen for every game that ever uses it. */
static int test_readable_value(void)
{
    static const char *SRC =
        "sort actor\n"
        "entity ( hero, ogre : actor )\n"
        "state ( hp(actor) : int in 0 .. 40   cap(actor) : int in 0 .. 40\n"
        "        ward(actor) : int in 0 .. 40   hurt(actor) )\n"
        "init ( hp(hero) = 30  cap(hero) = 40  ward(hero) = 5\n"
        "       hp(ogre) = 8   cap(ogre) = 20  ward(ogre) = 0 )\n"
        "value ( shown(actor) : int   width(actor) : int )\n"
        "rule sh(X: actor): => shown(X) = hp(X) + ward(X)\n"
        "rule wd(X: actor): => width(X) = 48 * shown(X) / cap(X)\n"
        "action wound(X: actor): causes hp(X) -= 10 & hurt(X)\n";

    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    long v = 0;
    /* a value nothing in the world reads is still readable — which is the
     * whole point, since a bar is computed for a client and for nobody else */
    CHECK(world_get_value(w, intern_id(sy, "shown(hero)"), &v) && v == 35);
    CHECK(world_get_value(w, intern_id(sy, "shown(ogre)"), &v) && v == 8);

    /* values compose: `width` reads `shown`, which reads two fluents */
    CHECK(world_get_value(w, intern_id(sy, "width(hero)"), &v) && v == 42);
    CHECK(world_get_value(w, intern_id(sy, "width(ogre)"), &v) && v == 19);

    /* ...and it is a function of CURRENT state, not of load time */
    CHECK(step(w, sy, "wound(hero)") == 0);
    CHECK(world_get_value(w, intern_id(sy, "shown(hero)"), &v) && v == 25);
    CHECK(world_get_value(w, intern_id(sy, "width(hero)"), &v) && v == 30);

    /* an atom that is not a registered value says so rather than reading 0 —
     * the difference between "no such number" and "the number is zero" */
    CHECK(!world_get_value(w, intern_id(sy, "hp(hero)"), &v));
    CHECK(!world_get_value(w, intern_id(sy, "shown(nobody)"), &v));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a value as a LOOKUP TABLE (#94) --------------------------------------
 *
 * A value's head argument may be a CONSTANT instead of a parameter, and then
 * the definition speaks for that instance alone. Without it a value is only
 * ever a function — one formula for every instance — which is fine for
 * `damage(W)` and useless for the thing presentation actually needs, where
 * each shape has its own geometry and there is no formula relating them.
 *
 * "Exactly one unconditional base" (#94) then means one PER INSTANCE: rows
 * never collide with each other, and a catch-all is the default beneath them. */
static int test_value_table(void)
{
    static const char *SRC =
        "enum shape { sh_bar, sh_icon, sh_panel }\n"
        "sort actor\n"
        "entity hero : actor\n"
        "state ( hp(actor) : int in 0 .. 40 )\n"
        "init  ( hp(hero) = 30 )\n"
        "value ( sx(shape) : int   sw(shape) : int )\n"
        "rule x_bar:   => sx(sh_bar)   = 8\n"
        "rule x_icon:  => sx(sh_icon)  = 40\n"
        "rule x_panel: => sx(sh_panel) = 0\n"
        "rule w_any(S: shape): => sw(S) = 16\n"
        "rule w_bar:           => sw(sh_bar) = 48 * hp(hero) / 40\n";

    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    long v = 0;
    /* every row is its own instance's base */
    CHECK(world_get_value(w, intern_id(sy, "sx(sh_bar)"), &v)   && v == 8);
    CHECK(world_get_value(w, intern_id(sy, "sx(sh_icon)"), &v)  && v == 40);
    CHECK(world_get_value(w, intern_id(sy, "sx(sh_panel)"), &v) && v == 0);

    /* a catch-all is the default, and a row overrides it for its instance */
    CHECK(world_get_value(w, intern_id(sy, "sw(sh_icon)"), &v)  && v == 16);
    CHECK(world_get_value(w, intern_id(sy, "sw(sh_panel)"), &v) && v == 16);
    CHECK(world_get_value(w, intern_id(sy, "sw(sh_bar)"), &v)   && v == 36);

    /* a row is still a value: it reads state, so it follows it */
    world_set_num(w, intern_id(sy, "hp(hero)"), 10);
    CHECK(world_get_value(w, intern_id(sy, "sw(sh_bar)"), &v) && v == 12);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* A table with no catch-all is PARTIAL in the honest sense: defined exactly
 * where a row speaks. Two definitions that both apply to one instance are the
 * collision #94 has always refused. */
static int test_value_table_errors(void)
{
    static const char *DUP =
        "enum shape { sh_a, sh_b }\n"
        "value ( q(shape) : int )\n"
        "rule d1(S: shape): => q(S) = 1\n"
        "rule d2(S: shape): => q(S) = 2\n";
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(DUP, "t.story", sy, &d);
    CHECK(w == NULL || d.nerrors > 0);
    CHECK(d.count > 0 && strstr(d.items[0].msg, "two unconditional definitions"));
    if (w) world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_readable_value()) return 1;
    if (test_value_table()) return 1;
    if (test_value_table_errors()) return 1;
    if (test_shared_d20()) return 1;
    if (test_guard_effect_coherence()) return 1;
    if (test_nested_and_folded()) return 1;
    if (test_errors()) return 1;
    printf("test_value: all passed\n");
    return 0;
}
