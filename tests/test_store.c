/* Golden test for STORE-BACKED functional fluents (issue #19 slice 2;
 * DESIGN.md §5.6/§5.8).
 *
 * `at(X) : cell` where `cell` is a `domain` — a functional fluent stored as one
 * opaque uint32 handle per entity in the value store (never |cells| atoms, the
 * §5.8 space win). Host-minted handles: the host sets and reads positions; the
 * engine copies them with `:=` (a move) and never inspects them. Equality is
 * read through a provider (same_cell), which consults the engine's stored
 * positions. Position persists across a step unless a move fires (inertia). */

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
static uint32_t SAME;

/* Spatial provider: same_cell(X, Y) reads the ENGINE's stored positions — the
 * host never stores them, it queries them (§5.6, the logic owns positions). */
static bool prov(void *ctx, uint32_t pred, const uint32_t *a, int n)
{
    world *w = ctx;
    if (pred == SAME && n == 2) {
        char bx[24], by[24];
        snprintf(bx, sizeof bx, "at(%s)", intern_name(SY, a[0]));
        snprintf(by, sizeof by, "at(%s)", intern_name(SY, a[1]));
        return world_get_num(w, intern_id(SY, bx)) == world_get_num(w, intern_id(SY, by));
    }
    return false;
}

static long at(world *w, const char *e)
{
    char b[24];
    snprintf(b, sizeof b, "at(%s)", e);
    return world_get_num(w, intern_id(SY, b));
}
static int grouped(world *w)
{
    return world_query(w, dl_pos(intern_id(SY, "grouped(aria,ally)"))) == DL_PROVED;
}

static int test_copy_move(void)
{
    const char *src =
        "domain cell\n"
        "sort actor\n"
        "provider same_cell(actor, actor)\n"
        "entity ( aria, ally : actor )\n"
        "state ( at(actor) : cell )\n"
        "action follow(X: actor, Y: actor): causes at(X) := at(Y)\n"
        "rule together(X: actor, Y: actor): same_cell(X, Y) => grouped(X, Y)\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    SAME = intern_id(SY, "same_cell");
    world_set_provider_fn(w, prov, w);

    /* the host mints opaque cell handles and sets initial positions */
    world_set_num(w, intern_id(SY, "at(aria)"), 1000);
    world_set_num(w, intern_id(SY, "at(ally)"), 1001);
    CHECK(!grouped(w));                       /* different cells */

    char err[128];
    uint32_t f = intern_id(SY, "follow(aria,ally)");
    CHECK(world_step(w, &f, 1, err, sizeof err) == 0);
    CHECK(at(w, "aria") == 1001);             /* copied ally's cell handle */
    CHECK(at(w, "ally") == 1001);
    CHECK(grouped(w));                        /* now co-located (via the provider) */

    /* inertia: a step with no move leaves position unchanged */
    uint32_t noop = intern_id(SY, "wait");
    CHECK(world_step(w, &noop, 1, err, sizeof err) == 0);
    CHECK(at(w, "aria") == 1001);

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
    /* no arithmetic on an opaque cell handle */
    if (expect_error_msg(
            "domain cell\nsort actor\nstate ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) += 1\n",
            "opaque cell handle"))
        return 1;
    /* `:=` must copy a cell fluent, not a literal/arithmetic */
    if (expect_error_msg(
            "domain cell\nsort actor\nstate ( at(actor):cell )\n"
            "action m(X:actor): causes at(X) := 5\n",
            "must copy another cell fluent"))
        return 1;
    /* a cell fluent has no numeric comparison — read it through a provider */
    if (expect_error_msg(
            "domain cell\nsort actor\nstate ( at(actor):cell )\n"
            "rule r(X:actor): at(X) <= 0 => hurt(X)\n",
            "read positions through a provider"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_copy_move()) return 1;
    if (test_errors())    return 1;
    printf("test_store: all passed\n");
    return 0;
}
