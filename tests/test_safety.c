/* Range-restriction / safety pass (§5.2 item 1, EPIC #26 child 2 / #30). Pins
 * that every rule variable must be bound by a positive body GENERATOR, with
 * sideways information passing for providers (an anchored provider generates its
 * free args; an unanchored one does not). Unbindable vars grounds over their
 * whole sort — the nᵏ blow-up bench_ground measures — so they warn (not fail:
 * typed sorts keep grounding finite until the tick-time matcher, #28, lands).
 *
 * These are the generator/filter classifications #28's join planner reuses, so
 * they are golden: changing what counts as a generator changes the matcher. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } \
    } while (0)

/* Compile `src`; assert it succeeds, produces exactly `nwarn` diagnostics (all
 * warnings, never errors), and — when `needle` is non-NULL — that the first
 * diagnostic contains it. */
static int expect(const char *src, int nwarn, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "safety.story", sy, &d);
    int rc = 0;
    if (!w) { fprintf(stderr, "compile FAILED: %s\n", d.count ? di[0].msg : "?"); rc = 1; }
    else if (d.nerrors != 0) { fprintf(stderr, "unexpected error: %s\n", di[0].msg); rc = 1; }
    else if (d.count != nwarn) {
        fprintf(stderr, "want %d diags, got %d:\n", nwarn, d.count);
        for (int i = 0; i < d.count; i++) fprintf(stderr, "  [%d] %s\n", i, di[i].msg);
        rc = 1;
    } else if (needle && (d.count == 0 || !strstr(di[0].msg, needle))) {
        fprintf(stderr, "diag missing '%s': %s\n", needle, d.count ? di[0].msg : "(none)");
        rc = 1;
    }
    if (w) world_free(w);
    intern_free(sy);
    return rc;
}

int main(void)
{
    /* --- clean: positive generators, no warnings --- */

    /* a positive fluent atom binds its arg var */
    if (expect("sort actor\nentity ( a0 : actor )\nstate ( cond(actor) )\n"
               "rule flagged(X: actor): cond(X) => flagged(X)\n", 0, NULL)) return 1;

    /* a numeric guard binds its entity arg (value store has a row per entity) */
    if (expect("sort actor\nentity ( a0 : actor )\nstate ( hp(actor) : int in 0 .. 10 )\n"
               "rule down(X: actor): hp(X) <= 0 => down(X)\n", 0, NULL)) return 1;

    /* a provider ANCHORED by a constant generates its free arg (who is near wiz) */
    if (expect("sort actor\nentity ( wiz, g0 : actor )\nprovider near(actor, actor)\n"
               "rule flagged(X: actor): near(wiz, X) => flagged(X)\n", 0, NULL)) return 1;

    /* a provider anchored by ANOTHER generator: X by the fluent, Y by X via near */
    if (expect("sort actor\nentity ( a0 : actor )\nprovider near(actor, actor)\n"
               "state ( awake(actor) )\n"
               "rule linked(X: actor, Y: actor): awake(X) & near(X, Y) => linked(X, Y)\n",
               0, NULL)) return 1;

    /* --- warns: no positive generator --- */

    /* a var only in negation cannot be enumerated (unsafe negation) */
    if (expect("sort actor\nentity ( a0 : actor )\nstate ( cond(actor) )\n"
               "rule flagged(X: actor): ~cond(X) => flagged(X)\n",
               1, "no positive generator")) return 1;

    /* an UNANCHORED provider (both args free) binds neither — two warnings */
    if (expect("sort actor\nentity ( a0, a1 : actor )\nprovider near(actor, actor)\n"
               "rule linked(X: actor, Y: actor): near(X, Y) => linked(X, Y)\n",
               2, "no positive generator")) return 1;

    /* a declared var that never occurs in the body: the classic unused-var warn */
    if (expect("sort actor\nentity ( a0 : actor )\nstate ( alarm(actor) )\n"
               "rule r(X: actor): alarm(a0) => flagged(a0)\n",
               1, "does not occur in the body")) return 1;

    printf("test_safety: all passed\n");
    return 0;
}
