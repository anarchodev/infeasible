/* Golden test for expression guards as an AUTHORING surface (#130, #132) —
 * how they are written, and what they say when the answer surprises you.
 *
 * Both halves come from the #118 probe, and both are about the same construct:
 *
 *   #130 — a guard led by a numeric fluent read is the natural spelling
 *   (`atk_die(A) + atk_mod(A) >= ac(T)`), and it has to parse. The parser used
 *   to enter the expression path only on a lead the boolean grammar can't
 *   start with, so the bare form failed with a diagnostic about rule arrows.
 *   The plain threshold form (`hp(X) <= 0`) must KEEP taking the plain
 *   numeric-guard path — that is the atom the stratifier harvests and the
 *   primed dying trigger reads.
 *
 *   #132 — a guard compiles to a synthetic marker atom, and a trace naming the
 *   marker ("eg14[A=grunk,T=vera]") tells the reader nothing. The trace should
 *   show the guard as written and the operands the solve actually compared —
 *   the difference between a debugger and an invitation to go digging. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile(const char *src, intern *sy)
{
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "  compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

/* world_why's output as a string. */
static char *why_str(world *w, dl_lit q)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(w, q, m);
    fclose(m);
    return buf;
}

/* ---- #130: the spellings ------------------------------------------------ */

/* `atk_die + atk_mod >= ac` — arithmetic on the left of the comparison. */
static const char *BARE =
    "sort actor\n"
    "entity ( grunk, vera : actor )\n"
    "state (\n"
    "  atk_die(actor) : int in 0 .. 20\n"
    "  atk_mod(actor) : int\n"
    "  acb(actor)     : int\n"
    "  hp(actor)      : int in 0 .. 30\n"
    "  shielded(actor)\n"
    "  alive(actor)\n"
    ")\n"
    "init ( atk_die(grunk) = 15  atk_mod(grunk) = 4  acb(vera) = 15\n"
    "       hp(vera) = 10  alive(vera) )\n"
    "value ac(actor) : int\n"
    "rule ac_base(X: actor):               => ac(X) = acb(X)\n"
    "rule ac_shield(X: actor): shielded(X) => ac(X) = prior + 5\n"
    "rule incoming(A: actor, T: actor):\n"
    "  atk_die(A) + atk_mod(A) >= ac(T) => incoming_hit(A, T)\n"
    /* a numeric read compared against something COMPUTED, no arithmetic on the
     * left — the other half of the same gap */
    "rule sturdy(T: actor): hp(T) >= ac(T) => tough(T)\n"
    /* and the plain threshold form, which must stay a plain numeric guard:
     * the primed read below is only legal over one */
    "rule down(X: actor): hp(X) <= 0 -> dropped(X)\n"
    "action hit(T: actor): causes hp(T) -= 12\n"
    "rule slain(X: actor): hp(X)' <= 0 & alive(X) causes ~alive(X)\n"
    "action raise_shield(T: actor): causes shielded(T)\n";

/* The same file with the leading parens the parser used to demand. */
static const char *PARENS =
    "sort actor\n"
    "entity ( grunk, vera : actor )\n"
    "state (\n"
    "  atk_die(actor) : int in 0 .. 20\n"
    "  atk_mod(actor) : int\n"
    "  acb(actor)     : int\n"
    "  hp(actor)      : int in 0 .. 30\n"
    "  shielded(actor)\n"
    "  alive(actor)\n"
    ")\n"
    "init ( atk_die(grunk) = 15  atk_mod(grunk) = 4  acb(vera) = 15\n"
    "       hp(vera) = 10  alive(vera) )\n"
    "value ac(actor) : int\n"
    "rule ac_base(X: actor):               => ac(X) = acb(X)\n"
    "rule ac_shield(X: actor): shielded(X) => ac(X) = prior + 5\n"
    "rule incoming(A: actor, T: actor):\n"
    "  (atk_die(A) + atk_mod(A)) >= ac(T) => incoming_hit(A, T)\n"
    "rule sturdy(T: actor): (hp(T)) >= ac(T) => tough(T)\n"
    "rule down(X: actor): hp(X) <= 0 -> dropped(X)\n"
    "action hit(T: actor): causes hp(T) -= 12\n"
    "rule slain(X: actor): hp(X)' <= 0 & alive(X) causes ~alive(X)\n"
    "action raise_shield(T: actor): causes shielded(T)\n";

static int test_numeric_led_guard(void)
{
    intern *sy = intern_new();
    world *bare = compile(BARE, sy);
    world *par  = compile(PARENS, sy);
    CHECK(bare && par);

    uint32_t hit = intern_id(sy, "incoming_hit(grunk,vera)");
    uint32_t tough = intern_id(sy, "tough(vera)");

    /* 15 + 4 >= 15 — and the two spellings agree, which is the point */
    CHECK(world_query(bare, dl_pos(hit)) == DL_PROVED);
    CHECK(world_query(par,  dl_pos(hit)) == DL_PROVED);
    /* 10 >= 15 is false */
    CHECK(world_query(bare, dl_pos(tough)) != DL_PROVED);
    CHECK(world_query(par,  dl_pos(tough)) != DL_PROVED);

    /* the guard is LIVE, not folded: Shield raises ac(vera) to 19 and the same
     * locked roll stops clearing it */
    char err[128];
    uint32_t shield = intern_id(sy, "raise_shield(vera)");
    CHECK(world_step(bare, &shield, 1, err, sizeof err) == 0);
    CHECK(world_step(par,  &shield, 1, err, sizeof err) == 0);
    CHECK(world_query(bare, dl_pos(hit)) == DL_REFUTED);
    CHECK(world_query(par,  dl_pos(hit)) == DL_REFUTED);

    /* the plain threshold form still took the plain path: its primed twin is
     * what the dying trigger reads, and that only exists over a numeric guard */
    uint32_t strike = intern_id(sy, "hit(vera)");
    CHECK(world_step(bare, &strike, 1, err, sizeof err) == 0);
    CHECK(world_get_num(bare, intern_id(sy, "hp(vera)")) == 0);
    CHECK(!world_get(bare, intern_id(sy, "alive(vera)")));   /* the ramification */
    CHECK(world_query(bare, dl_pos(intern_id(sy, "dropped(vera)"))) == DL_PROVED);

    world_free(bare);
    world_free(par);
    intern_free(sy);
    return 0;
}

/* ---- #132: the guard explains itself ------------------------------------ */

static int test_guard_trace(void)
{
    intern *sy = intern_new();
    world *w = compile(BARE, sy);
    CHECK(w != NULL);

    uint32_t hit = intern_id(sy, "incoming_hit(grunk,vera)");
    char err[128];
    uint32_t shield = intern_id(sy, "raise_shield(vera)");
    CHECK(world_step(w, &shield, 1, err, sizeof err) == 0);   /* ac 15 -> 20 */
    CHECK(world_query(w, dl_pos(hit)) == DL_REFUTED);

    char *t = why_str(w, dl_pos(hit));
    CHECK(t != NULL);
    /* the author's own spelling, with the arguments ground */
    CHECK(strstr(t, "atk_die(grunk) + atk_mod(grunk) >= ac(vera)") != NULL);
    /* and the operands THIS solve compared: the locked 19 vs the shielded 20 … */
    CHECK(strstr(t, "[19 >= 20]") != NULL);
    /* … so the answer is in the trace, not in a second query */
    CHECK(strstr(t, "-- inapplicable") != NULL);
    /* the synthetic marker name never surfaces */
    CHECK(strstr(t, "eg") == NULL || strstr(t, "eg") > strstr(t, "atk_die"));
    free(t);

    /* undo the shield and the same guard reads the other way */
    world_set(w, intern_id(sy, "shielded(vera)"), false);
    CHECK(world_query(w, dl_pos(hit)) == DL_PROVED);
    t = why_str(w, dl_pos(hit));
    CHECK(strstr(t, "[19 >= 15]") != NULL);
    CHECK(strstr(t, "-- applicable") != NULL);
    free(t);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* A hand-built world registers no source text, so its guards render exactly as
 * they always did — the rendering is an addition, not a new requirement. */
static int test_unnamed_guard_unchanged(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), g = intern_id(sy, "eg0"),
             hurt = intern_id(sy, "hurt");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 30, true);
    world_set_num(w, hp, 3);
    expr_ins lhs[] = { { EXPR_LOAD, (long)hp } }, rhs[] = { { EXPR_CONST, 5 } };
    world_add_expr_guard(w, g, lhs, 1, rhs, 1, WORLD_CMP_LT);
    dl_lit body = dl_pos(g);
    world_add_rule(w, "r", DL_DEFEASIBLE, dl_pos(hurt), &body, 1);

    CHECK(world_query(w, dl_pos(hurt)) == DL_PROVED);
    char *t = why_str(w, dl_pos(hurt));
    CHECK(strstr(t, "eg0") != NULL);           /* the atom's own name */
    CHECK(strstr(t, "[3 < 5]") == NULL);       /* no source, no annotation */
    free(t);

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_numeric_led_guard()) return 1;
    if (test_guard_trace()) return 1;
    if (test_unnamed_guard_unchanged()) return 1;
    printf("test_guards: all passed\n");
    return 0;
}
