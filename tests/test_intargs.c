/* Golden test for numeric literal args to providers (issue #22; DESIGN.md §5.6).
 *
 * `provider near(actor, actor, int)` with a read `near(X, Y, 2)` — the literal
 * `2` is the relation's radius (near-within-2 is a different relation than
 * near-within-3). It arrives at the provider callback as the interned decimal
 * string, so the host reads it as the number. Completes the provider-arg-type
 * story begun by the opaque `domain` args (#18): the other non-entity arg. */

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

/* Host geometry: `b` is within 2 of `a`, `c` is not. The host reads the radius
 * literal straight off the third arg. */
static intern *SY;
static uint32_t NEAR, A, B;
static long seen_radius;

static bool prov(void *ctx, uint32_t pred, const uint32_t *a, int n)
{
    (void)ctx;
    if (pred == NEAR && n == 3) {
        seen_radius = atol(intern_name(SY, a[2]));   /* the int arg, as its digits */
        return a[0] == A && a[1] == B && seen_radius >= 2;
    }
    return false;
}

static long hp(world *w, const char *e)
{
    char b[16];
    snprintf(b, sizeof b, "hp(%s)", e);
    return world_get_num(w, intern_id(SY, b));
}

static int test_radius(void)
{
    const char *src =
        "sort actor\n"
        "provider near(actor, actor, int)\n"
        "entity ( a, b, c : actor )\n"
        "state ( hp(actor) : int in 0 .. 60 )\n"
        "init ( hp(a)=20 hp(b)=20 hp(c)=20 )\n"
        "action zap(X: actor): causes for each Y: actor where near(X, Y, 2): hp(Y) -= 5\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    NEAR = intern_id(SY, "near");  A = intern_id(SY, "a");  B = intern_id(SY, "b");
    world_set_provider_fn(w, prov, NULL);

    uint32_t act = intern_id(SY, "zap(a)");
    char err[128];
    CHECK(world_step(w, &act, 1, err, sizeof err) == 0);

    CHECK(seen_radius == 2);        /* the literal reached the callback as 2 */
    CHECK(hp(w, "b") == 15);        /* within radius 2: took 5 */
    CHECK(hp(w, "a") == 20);        /* self / out of range: untouched */
    CHECK(hp(w, "c") == 20);

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
    /* an int literal in a non-int (entity) position */
    if (expect_error_msg(
            "sort actor\nprovider p(actor, int)\nentity ( x : actor )\n"
            "state ( q(actor) )\nrule r(Y: actor): p(5, 3) => q(Y)\n",
            "not declared `int`"))
        return 1;
    /* an entity where an int is expected */
    if (expect_error_msg(
            "sort actor\nprovider p(actor, int)\nentity ( x : actor )\n"
            "state ( q(actor) )\nrule r(Y: actor): p(Y, x) => q(Y)\n",
            "expects an integer"))
        return 1;
    /* an int literal argument to a plain fluent */
    if (expect_error_msg(
            "sort actor\nstate ( hp(actor) )\nrule r(Y: actor): hp(2) => hp(Y)\n",
            "not declared `int`"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_radius()) return 1;
    if (test_errors()) return 1;
    printf("test_intargs: all passed\n");
    return 0;
}
