/* Golden test for dynamic numeric clamp bounds (issue #4; DESIGN.md §5.8).
 *
 * `state hp(actor) : int in 0 .. hp_max(actor)` — the clamp's upper (or lower)
 * bound is an expression over another fluent, resolved PER ENTITY at commit
 * time. "Leftover damage is lost" is then schema, not a rule anyone can forget:
 * a heal past the max is clamped to that entity's own max, a hurt past the
 * floor to the floor. Literal bounds keep working unchanged. */

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

static long hp_of(world *w, intern *sy, const char *ent)
{
    char b[32];
    snprintf(b, sizeof b, "hp(%s)", ent);
    return world_get_num(w, intern_id(sy, b));
}

static int step(world *w, intern *sy, const char *ground_action)
{
    uint32_t a = intern_id(sy, ground_action);
    char err[128];
    int r = world_step(w, &a, 1, err, sizeof err);
    if (r) fprintf(stderr, "  step %s: %s\n", ground_action, err);
    return r;
}

/* A per-entity upper bound (`0 .. hp_max(X)`): each creature clamps to its OWN
 * max, and both share the constant floor of 0. */
static int test_dynamic_upper(void)
{
    const char *src =
        "sort actor\n"
        "entity ( aria, grunk : actor )\n"
        "state (\n"
        "    hp(actor)     : int in 0 .. hp_max(actor)\n"
        "    hp_max(actor) : int\n"
        ")\n"
        "init ( hp(aria)=10 hp_max(aria)=12 hp(grunk)=5 hp_max(grunk)=8 )\n"
        "action heal(T: actor): causes hp(T) += 20\n"
        "action hurt(T: actor): causes hp(T) -= 100\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    /* heal past the max clamps to each entity's own hp_max — not a shared const */
    CHECK(step(w, sy, "heal(aria)")  == 0);
    CHECK(step(w, sy, "heal(grunk)") == 0);
    CHECK(hp_of(w, sy, "aria")  == 12);
    CHECK(hp_of(w, sy, "grunk") == 8);

    /* hurt past the floor clamps to the constant lower bound (0) */
    CHECK(step(w, sy, "hurt(aria)") == 0);
    CHECK(hp_of(w, sy, "aria") == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* A dynamic LOWER bound works symmetrically (`floor(X) .. 100`). */
static int test_dynamic_lower(void)
{
    const char *src =
        "sort actor\n"
        "entity ( aria : actor )\n"
        "state (\n"
        "    hp(actor)    : int in floor(actor) .. 100\n"
        "    floor(actor) : int\n"
        ")\n"
        "init ( hp(aria)=10 floor(aria)=3 )\n"
        "action hurt(T: actor): causes hp(T) -= 100\n";

    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);

    CHECK(step(w, sy, "hurt(aria)") == 0);
    CHECK(hp_of(w, sy, "aria") == 3);          /* clamped up to floor(aria) */

    world_free(w);
    intern_free(sy);
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
    /* a bound that reads a non-numeric / undeclared fluent */
    if (expect_error_msg(
            "sort actor\nentity ( a : actor )\n"
            "state ( hp(actor) : int in 0 .. ghost(actor) )\n",
            "numeric fluent"))
        return 1;
    /* a `roll()` in a bound — the range must be a stable value */
    if (expect_error_msg(
            "sort actor\nentity ( a : actor )\n"
            "state ( hp(actor) : int in 0 .. roll(6) )\n",
            "roll"))
        return 1;
    /* a constant empty range still errors (unchanged behaviour) */
    if (expect_error_msg(
            "state ( hp : int in 5 .. 2 )\n", "range is empty"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_dynamic_upper()) return 1;
    if (test_dynamic_lower()) return 1;
    if (test_errors())        return 1;
    printf("test_clamp: all passed\n");
    return 0;
}
