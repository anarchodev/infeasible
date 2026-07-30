/* Golden test for PARTIAL derived values (#116, EPIC #154 "regaining
 * totality").
 *
 * Partiality is INFERRED: a value with zero unconditional, `prior`-free base
 * definitions is partial — no keyword. Undecidedness lives at
 * definition-selection, never inside the integer:
 *  - a partial value with no applicable definition is UNDECIDED, not zero —
 *    a guard over it asserts NEITHER fact (genuinely tri-valued, the honest
 *    "the question does not apply");
 *  - `defined v(X)` is a first-class body atom — the disjunction of the
 *    value's prior-free layer markers (ordinary defeasible rules sharing a
 *    head), so it is queryable and why-traceable;
 *  - `prior` over nothing PROPAGATES undefinedness (Bless on a save that
 *    does not exist — still does not exist), it never traps;
 *  - the STATIC SAFETY RULE: an arithmetic position (effect RHS, clamp
 *    bound, definition expression) may read a partial value only if the same
 *    rule's condition also reads it (any guard, or `defined`) — an unguarded
 *    read is a located compile error, so the runtime trap is unreachable
 *    from a clean compile (Elm's bar; the trap survives as an assertion);
 *  - a defeater whose body is UNDECIDED cannot refute — the head goes
 *    UNDECIDED, distinct from the REFUTED an applicable unbeaten attacker
 *    produces under ambiguity blocking (contested is not undecided);
 *  - a step rule gated on definedness fires for defined subjects and is a
 *    quiet no-op for undefined ones — never a `-1`. */

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

/* The shared scenario: spell_dc exists only for casters; Bless layers
 * `prior + 2` on top of whatever is beneath — including nothing.
 *   wiz: caster, unblessed  -> 13
 *   pal: caster, blessed    -> 15
 *   ftr: blessed ONLY       -> undefined (prior propagates: 13 never held) */
static const char *DC_SRC =
    "sort actor\n"
    "entity ( wiz : actor  pal : actor  ftr : actor )\n"
    "state ( caster(actor)  blessed(actor)  dcv(actor) : int )\n"
    "init ( caster(wiz)  caster(pal)  blessed(pal)  blessed(ftr) )\n"
    "value spell_dc(actor) : int\n"
    "rule dc_base(X: actor): caster(X) => spell_dc(X) = 13\n"
    "rule dc_bless(X: actor): blessed(X) => spell_dc(X) = prior + 2\n"
    "dc_bless > dc_base\n"
    "rule hasdc(X: actor): spell_dc(X) >= 0 => has_dc(X)\n"
    "rule cancast(X: actor): defined spell_dc(X) => can_cast(X)\n";

/* --- undecided, not zero: the `>= 0` guard would hold for a silent 0 --- */
static int test_undecided_not_zero(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(DC_SRC, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "has_dc(wiz)") == DL_PROVED);
    CHECK(q(w, sy, "has_dc(pal)") == DL_PROVED);
    /* no definition applies: neither proved nor refuted — the guard asserted
     * NEITHER fact, so everything downstream of it stays undecided */
    CHECK(q(w, sy, "has_dc(ftr)") == DL_UNDECIDED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- `defined` is an ordinary queryable literal, REFUTED when nothing
 *     applies (all its marker rules provably fail) --- */
static int test_defined_atom(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(DC_SRC, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "can_cast(wiz)") == DL_PROVED);
    CHECK(q(w, sy, "can_cast(pal)") == DL_PROVED);
    CHECK(q(w, sy, "can_cast(ftr)") == DL_REFUTED);
    /* the ground atom itself, spelled the way a host would spell it */
    CHECK(q(w, sy, "defined(spell_dc(wiz))") == DL_PROVED);
    CHECK(q(w, sy, "defined(spell_dc(ftr))") == DL_REFUTED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- `prior` over nothing propagates: ftr is blessed (the layer FIRES) but
 *     the chain beneath never held, so the value still does not exist --- */
static int test_prior_propagates(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(DC_SRC, sy);
    CHECK(w != NULL);

    /* the bless marker itself is PROVED for ftr — firing is not defining */
    CHECK(q(w, sy, "dc_bless(ftr)") == DL_PROVED);
    CHECK(q(w, sy, "has_dc(ftr)") == DL_UNDECIDED);
    CHECK(q(w, sy, "can_cast(ftr)") == DL_REFUTED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- effects gated on definedness: fire when defined (both the `defined`
 *     idiom and a comparison guard), quiet no-op when not — never a -1 --- */
static int test_step_gating(void)
{
    intern *sy = intern_new();
    char src[2048];
    snprintf(src, sizeof src, "%s%s", DC_SRC,
             "action snap(X: actor): requires defined spell_dc(X)\n"
             "  causes dcv(X) := spell_dc(X)\n"
             "action snap2(X: actor): requires spell_dc(X) >= 0\n"
             "  causes dcv(X) := spell_dc(X)\n");
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "snap(wiz)") == 0);
    CHECK(num(w, sy, "dcv(wiz)") == 13);
    CHECK(step1(w, sy, "snap2(pal)") == 0);
    CHECK(num(w, sy, "dcv(pal)") == 15);           /* bless landed on the base */
    CHECK(step1(w, sy, "snap(ftr)") == 0);         /* undefined: quiet no-op */
    CHECK(num(w, sy, "dcv(ftr)") == 0);
    CHECK(step1(w, sy, "snap2(ftr)") == 0);        /* comparison guard, same */
    CHECK(num(w, sy, "dcv(ftr)") == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a defeater with an UNDECIDED body does not block --- */
static int test_defeater_undecided_body(void)
{
    intern *sy = intern_new();
    char src[2048];
    snprintf(src, sizeof src, "%s%s", DC_SRC,
             "state alive(actor)\n"
             "init ( alive(wiz)  alive(ftr) )\n"
             "rule calm_by_default(X: actor): alive(X) => calm(X)\n"
             "rule dc_panic(X: actor): spell_dc(X) >= 10 ~> ~calm(X)\n");
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    /* wiz: the defeater's body HOLDS (13 >= 10) — an applicable unbeaten
     * attacker REFUTES under ambiguity blocking (contested, not undecided) */
    CHECK(q(w, sy, "calm(wiz)") == DL_REFUTED);
    /* ftr: the body is UNDECIDED — the defeater neither fires (it cannot
     * refute calm) nor is provably inapplicable (so +d cannot counter it):
     * the head is honestly UNDECIDED, distinct from wiz's REFUTED */
    CHECK(q(w, sy, "calm(ftr)") == DL_UNDECIDED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a partial value defined FROM another partial value's definedness:
 *     `defined` in a definition body is a plain judgment atom (it settles in
 *     pass A), so partiality cascades — atk exists exactly where spell_dc
 *     does. Arithmetic over a layered value inside a definition's EXPRESSION
 *     stays rejected by the pre-existing test-flow rule (see test_errors). */
static int test_nested_partial(void)
{
    intern *sy = intern_new();
    char src[2048];
    snprintf(src, sizeof src, "%s%s", DC_SRC,
             "value atk(actor) : int\n"
             "rule ab(X: actor): defined spell_dc(X) => atk(X) = 5\n"
             "rule strong(X: actor): atk(X) >= 5 => strong_caster(X)\n");
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "strong_caster(wiz)") == DL_PROVED);
    CHECK(q(w, sy, "strong_caster(ftr)") == DL_UNDECIDED);   /* atk undefined */
    CHECK(q(w, sy, "defined(atk(pal))") == DL_PROVED);
    CHECK(q(w, sy, "defined(atk(ftr))") == DL_REFUTED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- `defined` over a TOTAL value: legal, warns, always holds --- */
static int test_defined_total_warns(void)
{
    const char *src =
        "state p\n"
        "value v : int\n"
        "rule d: => v = 3\n"
        "rule r: defined v => ok\n";
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    CHECK(w != NULL && d.nerrors == 0);
    int warned = 0;
    for (int i = 0; i < d.count && i < d.cap; i++)
        if (strstr(di[i].msg, "always holds")) warned = 1;
    CHECK(warned);
    CHECK(q(w, sy, "ok") == DL_PROVED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the static safety rule and `defined` misuse: located errors --- */
static int test_errors(void)
{
    static const char *PARTIAL_V =
        "sort actor\n"
        "entity a : actor\n"
        "state ( caster(actor)  dcv(actor) : int )\n"
        "value spell_dc(actor) : int\n"
        "rule dc_base(X: actor): caster(X) => spell_dc(X) = 13\n"
        "rule use(X: actor): spell_dc(X) >= 0 => has_dc(X)\n";
    static const struct { const char *extra, *msg; } BAD[] = {
        /* an effect RHS reading a partial value, condition silent */
        { "action bad(X: actor): causes dcv(X) := spell_dc(X)\n",
          "without its condition reading it" },
        /* same, buried in arithmetic */
        { "action bad2(X: actor): requires caster(X)\n"
          "  causes dcv(X) := 1 + spell_dc(X) * 2\n",
          "without its condition reading it" },
        /* a clamp bound has no condition to guard it */
        { "state hp(actor) : int in 0..spell_dc(actor)\n",
          "clamp bound" },
        /* a definition's expression vs its own body */
        { "value atk(actor) : int\n"
          "rule ab(X: actor): => atk(X) = spell_dc(X) + 1\n"
          "rule r2(X: actor): atk(X) >= 1 => strong(X)\n",
          "definition 'ab' reads 'spell_dc'" },
        /* `defined` misuse */
        { "rule n(X: actor): ~defined spell_dc(X) => mundane(X)\n",
          "negated `defined`" },
        { "rule n(X: actor): defined caster(X) => odd(X)\n",
          "not a declared value" },
        { "rule n(X: actor): defined spell_dc(X) >= 1 => odd(X)\n",
          "takes no comparison" },
        { "action z(X: actor): causes defined spell_dc(X)\n",
          "cannot appear in a `causes` clause" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char src[2048];
        snprintf(src, sizeof src, "%s%s", PARTIAL_V, BAD[i].extra);
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *w = story_compile(src, "t.story", sy, &d);
        if (w != NULL || d.nerrors == 0) {
            fprintf(stderr, "FAIL %s:%d: case %zu compiled but should not\n",
                    __FILE__, __LINE__, i);
            return 1;
        }
        int hit = 0;
        for (int k = 0; k < d.count && k < d.cap; k++)
            if (strstr(di[k].msg, BAD[i].msg)) hit = 1;
        if (!hit) {
            fprintf(stderr, "FAIL %s:%d: case %zu: wanted \"%s\", got \"%s\"\n",
                    __FILE__, __LINE__, i, BAD[i].msg, d.count ? di[0].msg : "");
            return 1;
        }
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_undecided_not_zero()) return 1;
    if (test_defined_atom()) return 1;
    if (test_prior_propagates()) return 1;
    if (test_step_gating()) return 1;
    if (test_defeater_undecided_body()) return 1;
    if (test_nested_partial()) return 1;
    if (test_defined_total_warns()) return 1;
    if (test_errors()) return 1;
    printf("test_partial: all passed\n");
    return 0;
}
