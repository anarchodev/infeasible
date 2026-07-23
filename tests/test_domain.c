/* Golden test for opaque `domain` value types (issue #18; DESIGN.md §5.6/§13).
 *
 * A `domain` is a third type category beside `sort` and `enum`: the engine
 * never enumerates it. Its values are host-minted handles that appear only as
 * provider/action-param arg types. A domain param (`at : point`) resolves in a
 * provider read to a stable placeholder atom the host maps to the value from
 * its provider context — so a point-targeted spell works with the engine
 * staying spatially blind (§5.6), and no `world_*` change. */

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

/* Host oracle: `at` is the cast's target point (opaque to the engine — it only
 * ever sees the placeholder atom). grik and gnok are near it; thorn is not. */
static intern *SY;
static uint32_t IN_RADIUS, AT, GRIK, GNOK, THORN;
static int bad_placeholder;

static bool prov(void *ctx, uint32_t pred, const uint32_t *a, int n)
{
    (void)ctx;
    if (pred == IN_RADIUS) {
        /* the domain param arrives as its placeholder atom (`at`), not a value */
        if (n < 2 || a[1] != AT) bad_placeholder++;
        return a[0] == GRIK || a[0] == GNOK;
    }
    return false;
}

static long hp(world *w, const char *ent)
{
    char b[32];
    snprintf(b, sizeof b, "hp(%s)", ent);
    return world_get_num(w, intern_id(SY, b));
}

static int test_point_targeting(void)
{
    const char *src =
        "domain point\n"
        "sort actor\n"
        "provider in_radius(actor, point)\n"
        "entity ( vera, grik, gnok, thorn : actor )\n"
        "state ( hp(actor) : int in 0 .. 60 )\n"
        "init ( hp(grik)=14 hp(gnok)=14 hp(thorn)=30 )\n"
        "action fireball(C: actor, at: point):\n"
        "    causes for each T: actor where in_radius(T, at): hp(T) -= 5\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    IN_RADIUS = intern_id(SY, "in_radius");  AT = intern_id(SY, "at");
    GRIK = intern_id(SY, "grik");  GNOK = intern_id(SY, "gnok");  THORN = intern_id(SY, "thorn");
    world_set_provider_fn(w, prov, NULL);

    uint32_t act = intern_id(SY, "fireball(vera)");
    char err[128];
    CHECK(world_step(w, &act, 1, err, sizeof err) == 0);

    CHECK(hp(w, "grik")  == 9);    /* in radius: took 5 */
    CHECK(hp(w, "gnok")  == 9);    /* in radius: took 5 */
    CHECK(hp(w, "thorn") == 30);   /* out of radius: untouched */
    CHECK(bad_placeholder == 0);   /* the domain arg was always the placeholder */

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
    /* a domain param is only for actions, not rules */
    if (expect_error_msg(
            "domain point\nsort actor\nrule r(x: point): a => b\n",
            "only allowed on actions"))
        return 1;
    /* a domain may not key a fluent yet (#19) */
    if (expect_error_msg(
            "domain cell\nstate ( on_fire(cell) )\n",
            "keyed by the domain 'cell'"))
        return 1;
    /* a variable cannot range over an opaque domain */
    if (expect_error_msg(
            "domain point\nsort actor\nprovider in_r(point)\n"
            "entity ( a : actor )\nstate ( hp(actor):int )\n"
            "action go(C: actor): causes for each c: point where in_r(c): hp(C) -= 1\n",
            "range a variable over the opaque domain"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_point_targeting()) return 1;
    if (test_errors())          return 1;
    printf("test_domain: all passed\n");
    return 0;
}
