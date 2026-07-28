/* Golden test for VALUE-RETURNING FUNCTION PROVIDERS (issue #19 slice 3;
 * DESIGN.md §5.6/§5.8).
 *
 * Directional movement — the one genuinely-new core primitive: a provider that
 * returns a *value* (a cell handle), not a boolean. A `function neighbor(cell,
 * int) : cell` is a host-computed geometry function; the effect-VM calls it
 * (EXPR_CALL) at commit time and stores the returned handle:
 *
 *     at(X) := neighbor(at(X), dir)
 *
 * The function is pure and seedless (I4) — geometry, not judgment. It never
 * concludes game truth; it hands the store an opaque handle the engine copies
 * (like a §5.6 cell move) but never inspects. Positions persist across a step
 * unless a move fires (inertia). */

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
static uint32_t NEIGHBOR;

/* A tiny 1-D geometry: cells are handles 100..105; `neighbor(c, dir)` steps to
 * c+dir, clamped to the [100,105] walls. Deterministic and seedless (I4). */
static long neighbor_fn(void *ctx, uint32_t pred, const long *a, int n)
{
    (void)ctx;
    if (pred == NEIGHBOR && n == 2) {
        long c = a[0] + a[1];
        if (c < 100) c = 100;
        if (c > 105) c = 105;
        return c;
    }
    return 0;
}

static long at(world *w, const char *e)
{
    char b[24];
    snprintf(b, sizeof b, "at(%s)", e);
    return world_get_num(w, intern_id(SY, b));
}

static int test_directional_move(void)
{
    const char *src =
        "domain cell\n"
        "sort actor\n"
        "function neighbor(cell, int) : cell\n"
        "entity ( hero : actor )\n"
        "state ( at(actor) : cell )\n"
        "action go_east(X: actor): causes at(X) := neighbor(at(X), 1)\n"
        "action go_west(X: actor): causes at(X) := neighbor(at(X), -1)\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    NEIGHBOR = intern_id(SY, "neighbor");
    world_set_fn_provider_fn(w, neighbor_fn, NULL);

    /* the host mints an opaque starting cell handle */
    world_set_num(w, intern_id(SY, "at(hero)"), 103);

    char err[128];
    uint32_t east = intern_id(SY, "go_east(hero)");
    uint32_t west = intern_id(SY, "go_west(hero)");

    CHECK(world_step(w, &east, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 104);              /* neighbor(103, +1) */
    CHECK(world_step(w, &east, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 105);
    CHECK(world_step(w, &east, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 105);              /* clamped at the wall */
    CHECK(world_step(w, &west, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 104);             /* neighbor(105, -1) */

    /* inertia: a step with no move leaves position unchanged */
    uint32_t noop = intern_id(SY, "wait");
    CHECK(world_step(w, &noop, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 104);

    world_free(w);
    intern_free(SY);
    return 0;
}

/* With no callback registered, a call reads 0 — the closed-world analog of a
 * false boolean provider (documented contract), never a crash. */
static int test_no_callback(void)
{
    const char *src =
        "domain cell\n"
        "sort actor\n"
        "function neighbor(cell, int) : cell\n"
        "entity ( hero : actor )\n"
        "state ( at(actor) : cell )\n"
        "action go(X: actor): causes at(X) := neighbor(at(X), 1)\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    world_set_num(w, intern_id(SY, "at(hero)"), 103);
    char err[128];
    uint32_t go = intern_id(SY, "go(hero)");
    CHECK(world_step(w, &go, 1, err, sizeof err) == 0);
    CHECK(at(w, "hero") == 0);                /* closed-world default */

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
    /* a function returning a non-cell type can't be a cell `:=` RHS */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction dist(cell) : int\n"
            "state ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := dist(at(X))\n",
            "returning that cell type"))
        return 1;
    /* arity mismatch at the call site */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction neighbor(cell, int) : cell\n"
            "state ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := neighbor(at(X))\n",
            "takes 2 arguments but 1 given"))
        return 1;
    /* arithmetic on a call result is still forbidden on an opaque cell handle */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction neighbor(cell, int) : cell\n"
            "state ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := neighbor(at(X), 1) + 1\n",
            "must copy another cell fluent"))
        return 1;
    /* an unknown return type in the declaration is a located error */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction f(cell) : nonsense\n"
            "state ( at(actor):cell )\n",
            "unknown return type"))
        return 1;
    /* a function name that clashes with a fluent */
    if (expect_error_msg(
            "domain cell\nsort actor\nstate ( at(actor):cell )\n"
            "function at(cell) : cell\n",
            "clashes with a fluent"))
        return 1;
    /* an int passed where a cell parameter is expected */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction neighbor(cell, int) : cell\n"
            "state ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := neighbor(5, 1)\n",
            "argument 1 expects cell but got int"))
        return 1;
    /* a cell passed where an int parameter is expected */
    if (expect_error_msg(
            "domain cell\nsort actor\nfunction neighbor(cell, int) : cell\n"
            "state ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := neighbor(at(X), at(X))\n",
            "argument 2 expects int but got cell"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_directional_move()) return 1;
    if (test_no_callback())      return 1;
    if (test_errors())           return 1;
    printf("test_fnprovider: all passed\n");
    return 0;
}
