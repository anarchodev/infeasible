/* Golden test for entity-valued functional fluents (issue #19 slice 1;
 * DESIGN.md §5.6/§5.7).
 *
 * `at(X) : cell` — a fluent valued in a sort's entities: exactly one cell per
 * tick, logic-backed over §5.7. Movement is a causal rule (`at(X)' = k1` or a
 * parameterized `= to`); position persists across a step unless a move fires
 * (spatial Yale-shooting); and a rule may JOIN the value to a bound variable
 * (`at(X) = c & on_fire(c)`), the read side of terrain interactions. The
 * store-backed representation for large cell domains is a follow-up. */

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

static intern *SY;

static const char *pos(world *w)
{
    const char *k[] = { "k0", "k1", "k2" };
    for (int i = 0; i < 3; i++) {
        char b[24];
        snprintf(b, sizeof b, "at(aria)=%s", k[i]);
        if (world_query(w, dl_pos(intern_id(SY, b))) == DL_PROVED) return k[i];
    }
    return "?";
}
static int burning(world *w)
{
    return world_query(w, dl_pos(intern_id(SY, "burning(aria)"))) == DL_PROVED;
}
static int step(world *w, const char *ground_action)
{
    uint32_t a = intern_id(SY, ground_action);
    char err[128];
    int r = world_step(w, &a, 1, err, sizeof err);
    if (r) fprintf(stderr, "  step %s: %s\n", ground_action, err);
    return r;
}

/* Movement changes exactly one actor's cell; position persists across a plain
 * step (inertia); the value/terrain join fires on the right cell. */
static int test_move_join_inertia(void)
{
    const char *src =
        "sort (actor, cell)\n"
        "entity ( aria : actor  k0, k1, k2 : cell )\n"
        "state ( at(actor) : cell  on_fire(cell) )\n"
        "init ( at(aria) = k0  on_fire(k1) )\n"
        "action move(X: actor, to: cell): causes at(X) = to\n"
        "rule on_burning(X: actor, c: cell): at(X) = c & on_fire(c) => burning(X)\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    CHECK(strcmp(pos(w), "k0") == 0);
    CHECK(!burning(w));

    /* parameterized move to the fire cell (destination is an action param) */
    CHECK(step(w, "move(aria,k1)") == 0);
    CHECK(strcmp(pos(w), "k1") == 0);      /* moved */
    CHECK(burning(w));                      /* join fired: on the on_fire cell */

    /* inertia: a step that fires no move leaves position unchanged */
    CHECK(step(w, "wait") == 0);
    CHECK(strcmp(pos(w), "k1") == 0);
    CHECK(burning(w));

    /* move off the fire cell — the join stops holding */
    CHECK(step(w, "move(aria,k2)") == 0);
    CHECK(strcmp(pos(w), "k2") == 0);
    CHECK(!burning(w));

    world_free(w);
    intern_free(SY);
    return 0;
}

static int expect_error_msg(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int ok = (w == NULL) && d.nerrors >= 1 &&
             d.items[0].sev == STORY_ERROR && d.items[0].line >= 1 &&
             (needle == NULL || strstr(d.items[0].msg, needle) != NULL);
    if (!ok)
        fprintf(stderr, "FAIL expected error (needle=%s) for <<%s>>: got %s\n",
                needle ? needle : "(any)", src,
                d.nerrors ? d.items[0].msg : "(no error)");
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_errors(void)
{
    /* a value not among the sort's entities */
    if (expect_error_msg(
            "sort (actor, cell)\nentity ( a : actor  k0 : cell )\n"
            "state ( at(actor) : cell )\ninit ( at(a) = nope )\n",
            "not a value of"))
        return 1;
    /* a functional fluent over an empty sort */
    if (expect_error_msg(
            "sort (actor, cell)\nentity ( a : actor )\nstate ( at(actor) : cell )\n",
            "which has no"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_move_join_inertia()) return 1;
    if (test_errors())            return 1;
    printf("test_functional: all passed\n");
    return 0;
}
