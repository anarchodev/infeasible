/* Golden test for integer division (#81; DESIGN.md §5.8): EXPR_DIV in the
 * effect VM and `/` in the .story expression grammar.
 *
 * The rounding semantics are an I4 replay surface and are pinned HERE, not
 * inherited from C or libc: division is FLOORED — the quotient rounds toward
 * -inf (5e "round down"), so -7/2 = -4 where C truncation would give -3.
 * Division by zero is defined as 0 at runtime; a divisor that constant-folds
 * to 0 is a located compile error. Compile-time folding and the VM must agree
 * exactly (a folded `(0-7)/2` and an evaluated one are the same story). */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* --- the VM opcode itself: floored quotients, all four sign cases --- */
static int test_vm_floor(void)
{
    static const struct { long a, b, want; } CASES[] = {
        {  7,  2,  3 }, { -7,  2, -4 },       /* the headline pin: -7/2 = -4 */
        {  7, -2, -4 }, { -7, -2,  3 },
        {  1,  2,  0 }, { -1,  2, -1 },
        {  6,  3,  2 }, { -6,  3, -2 },       /* exact: no adjustment */
        {  0,  5,  0 },
        {  5,  0,  0 }, { -5,  0,  0 },       /* defined: x/0 = 0 */
    };
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        intern *sy = intern_new();
        uint32_t hp = intern_id(sy, "hp"), go = intern_id(sy, "go");
        world *w = world_new(sy);
        world_declare_num(w, hp, 0, 0, false);
        world_set_num(w, hp, 99);

        expr_ins code[] = {
            { EXPR_CONST, CASES[i].a },
            { EXPR_CONST, CASES[i].b },
            { EXPR_DIV,   0 },
        };
        int r = world_add_step_rule(w, "quot", go, NULL, 0, NULL, 0);
        world_add_num_effect(w, r, hp, WORLD_OP_ASSIGN, code, 3);

        char err[128];
        uint32_t acts[] = { go };
        CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
        if (world_get_num(w, hp) != CASES[i].want) {
            fprintf(stderr, "FAIL %s:%d: %ld / %ld = %ld, want %ld\n",
                    __FILE__, __LINE__, CASES[i].a, CASES[i].b,
                    world_get_num(w, hp), CASES[i].want);
            return 1;
        }
        world_free(w);
        intern_free(sy);
    }
    return 0;
}

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

/* --- the surface: `/` in effect RHSs, dynamic and folded --- */
static int test_story_div(void)
{
    const char *src =
        "state (\n"
        "    hp : int\n"
        "    d  : int\n"
        "    z  : int\n"
        ")\n"
        "init ( hp = 7  z = 0 )\n"
        "// the 5e idiom this unblocks: half of a dynamic value, rounded down\n"
        "action halve:     causes d := hp / 2\n"
        "// floored on the VM path: (0-7)/2 stays dynamic via the hp read\n"
        "action neg_halve: causes d := (0 - hp) / 2\n"
        "exclusive halve, neg_halve\n"
        "// fully constant: folds at compile time, and the fold must floor too\n"
        "action cfold:     causes d := (0 - 7) / 2\n"
        "// precedence ('/' binds like '*') and left associativity\n"
        "action prec:      causes d := 8 - 6 / 2\n"
        "action leftassoc: causes d := hp / 2 * 2\n"
        "// a divisor that is zero at runtime (not foldably zero): defined 0\n"
        "action divz:      causes d := hp / z\n"
        "exclusive halve, neg_halve, cfold, prec, leftassoc, divz\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    uint32_t d = intern_id(sy, "d");

    CHECK(step(w, sy, "halve") == 0);
    CHECK(world_get_num(w, d) == 3);           /* 7/2 rounds down */
    CHECK(step(w, sy, "neg_halve") == 0);
    CHECK(world_get_num(w, d) == -4);          /* -7/2 = -4, toward -inf */
    CHECK(step(w, sy, "cfold") == 0);
    CHECK(world_get_num(w, d) == -4);          /* the fold agrees with the VM */
    CHECK(step(w, sy, "prec") == 0);
    CHECK(world_get_num(w, d) == 5);           /* 8 - (6/2), not (8-6)/2 */
    CHECK(step(w, sy, "leftassoc") == 0);
    CHECK(world_get_num(w, d) == 6);           /* (7/2)*2, not 7/(2*2) */
    CHECK(step(w, sy, "divz") == 0);
    CHECK(world_get_num(w, d) == 0);           /* 7/0 = 0, deterministic */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- divup: ceiling division, desugared to -((-a)/b) --- */
static int test_divup(void)
{
    const char *src =
        "state (\n"
        "    hp : int\n"
        "    d  : int\n"
        "    z  : int\n"
        ")\n"
        "init ( hp = 7  z = 0 )\n"
        "// the 5e exception: \"half X, rounded up\"\n"
        "action halfup:   causes d := divup(hp, 2)\n"
        "// toward +inf on negatives, the dual of '/'\n"
        "action neghalf:  causes d := divup(0 - hp, 2)\n"
        "// fully constant: the fold takes the same desugared path\n"
        "action cfold:    causes d := divup(7, 2)\n"
        "// exact division needs no adjustment\n"
        "action exact:    causes d := divup(8, 2)\n"
        "// runtime zero divisor: still the defined 0\n"
        "action divz:     causes d := divup(hp, z)\n"
        "exclusive halfup, neghalf, cfold, exact, divz\n"
        "// usable as a guard lead, like min/max\n"
        "rule sturdy_check: divup(hp, 2) >= 4 -> sturdy\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    uint32_t d = intern_id(sy, "d");
    dl_lit sturdy = { intern_id(sy, "sturdy"), false };

    CHECK(world_query(w, sturdy) == DL_PROVED);   /* divup(7,2) = 4 >= 4 */
    CHECK(step(w, sy, "halfup") == 0);
    CHECK(world_get_num(w, d) == 4);              /* 7/2 rounds UP */
    CHECK(step(w, sy, "neghalf") == 0);
    CHECK(world_get_num(w, d) == -3);             /* -7/2 toward +inf */
    CHECK(step(w, sy, "cfold") == 0);
    CHECK(world_get_num(w, d) == 4);
    CHECK(step(w, sy, "exact") == 0);
    CHECK(world_get_num(w, d) == 4);
    CHECK(step(w, sy, "divz") == 0);
    CHECK(world_get_num(w, d) == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- `/` in an expression guard feeding a judgment --- */
static int test_guard_div(void)
{
    const char *src =
        "state hp : int\n"
        "init hp = 7\n"
        "rule fit: min(hp, 99) / 2 >= 3 -> healthy\n"
        "action bleed: causes hp -= 4\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    dl_lit healthy = { intern_id(sy, "healthy"), false };

    CHECK(world_query(w, healthy) == DL_PROVED);      /* 7/2 = 3 >= 3 */
    CHECK(step(w, sy, "bleed") == 0);
    CHECK(world_query(w, healthy) == DL_REFUTED);     /* 3/2 = 1 < 3 */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a constant-zero divisor is a compile error, foldably-zero included --- */
static int test_const_zero_rejected(void)
{
    static const char *const BAD[] = {
        "state d : int\naction a: causes d := d / 0\n",
        "state d : int\naction a: causes d := d / (2 - 2)\n",
        "state d : int\naction a: causes d := divup(d, 0)\n",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        intern *sy = intern_new();
        story_diag di[8];
        story_diags dg = { di, 8, 0, 0 };
        world *w = story_compile(BAD[i], "t.story", sy, &dg);
        CHECK(w == NULL || dg.nerrors > 0);
        bool found = false;
        for (int k = 0; k < dg.count && !found; k++)
            found = strstr(dg.items[k].msg, "division by a constant zero") != NULL;
        CHECK(found);
        if (w) world_free(w);
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_vm_floor()) return 1;
    if (test_story_div()) return 1;
    if (test_divup()) return 1;
    if (test_guard_div()) return 1;
    if (test_const_zero_rejected()) return 1;
    printf("test_div: all passed\n");
    return 0;
}
