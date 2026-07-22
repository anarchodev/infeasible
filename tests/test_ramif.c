/* Golden test for ramifications through the surface language (DESIGN.md §5.4,
 * §11 M1 — lang). A ramification is a `rule … causes …` (a bare `causes`, no
 * arrow): a step rule with no action trigger, firing in any step whose state
 * matches. Body atoms are current-state by default; a postfix `'` reads the
 * next state, so the indirect effect cascades in the SAME step.
 *
 * Two things pinned:
 *   1. examples/ramif_cellar.story — the flagship: a slain guard drops its
 *      torch the instant it dies, because `~alive(X)'` is read next-state. The
 *      surface twin of test_world.c's hand-built torch ramification.
 *   2. the prime's scope, by rejection — `'` outside a ramification body, and a
 *      primed numeric guard / judgment (the deferred §5.8 stratification case),
 *      each a located error. */

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

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = '\0';
    fclose(f);
    return buf;
}

/* The flagship cascade: slaying the guard drops the torch in the same step. */
static int test_same_step_cascade(void)
{
    char *src = read_file(STORY_DIR "/ramif_cellar.story");
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag ditems[16];
    story_diags diags = { ditems, 16, 0, 0 };
    world *w = story_compile(src, NULL, sy, &diags);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n",
                diags.count ? diags.items[0].msg : "(no message)");
        free(src); intern_free(sy);
        return 1;
    }
    if (diags.count)
        fprintf(stderr, "unexpected diagnostic: %s\n", diags.items[0].msg);
    CHECK(diags.count == 0);

    uint32_t alive   = intern_id(sy, "alive(guard)"),
             holding = intern_id(sy, "holding(guard,torch)"),
             floor   = intern_id(sy, "on_floor(torch)"),
             a_slay  = intern_id(sy, "slay(guard)");
    char err[256];

    /* init: guard alive, holding the torch, nothing on the floor */
    CHECK(world_get(w, alive));
    CHECK(world_get(w, holding));
    CHECK(!world_get(w, floor));

    /* one step: the guard dies AND the torch hits the floor, together — the
     * ramification reacted to `~alive'` inside the same fixpoint. */
    CHECK(world_step(w, &a_slay, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, alive));      /* the action's direct effect */
    CHECK(!world_get(w, holding));    /* the ramification's indirect effect … */
    CHECK(world_get(w, floor));       /* … same step, not the next */

    /* idempotent afterward: nothing left to fall, floor stays set (a ramifica-
     * tion fires only when its match holds; a no-op `wait` step keeps state). */
    CHECK(world_step(w, NULL, 0, err, sizeof err) == 0);
    CHECK(world_get(w, floor));
    CHECK(!world_get(w, holding));

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

/* Compile `src`, expecting failure with a diagnostic whose message contains
 * `needle`. Returns 0 on the expected rejection, 1 otherwise. */
static int expect_reject(const char *what, const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag ditems[16];
    story_diags diags = { ditems, 16, 0, 0 };
    world *w = story_compile(src, NULL, sy, &diags);
    int rc = 0;
    if (w) {
        fprintf(stderr, "FAIL %s: compiled but should have been rejected\n", what);
        rc = 1;
    } else {
        bool found = false;
        for (int i = 0; i < diags.count && i < diags.cap; i++)
            if (diags.items[i].sev == STORY_ERROR &&
                strstr(diags.items[i].msg, needle)) { found = true; break; }
        if (!found) {
            fprintf(stderr, "FAIL %s: wrong diagnostic (want \"%s\"); got: %s\n",
                    what, needle, diags.count ? diags.items[0].msg : "(none)");
            rc = 1;
        }
    }
    if (w) world_free(w);
    intern_free(sy);
    return rc;
}

/* The prime's scope, pinned by rejection. Each source is otherwise well-formed;
 * only the primed atom is at fault. */
static int test_prime_scope(void)
{
    /* `'` in a judgment body — not a ramification, so the mark is meaningless */
    if (expect_reject("prime in judgment body",
        "sort actor\n"
        "entity guard : actor\n"
        "state (alive(actor) down(actor))\n"
        "rule r(X: actor): alive(X)' -> down(X)\n",
        "only allowed in a ramification body")) return 1;

    /* `'` in an action `requires` — actions guard on the current state only */
    if (expect_reject("prime in requires",
        "sort actor\n"
        "entity guard : actor\n"
        "state (alive(actor) fell(actor))\n"
        "action topple(X: actor): requires alive(X)' causes fell(X)\n",
        "only allowed in a ramification body")) return 1;

    /* a primed numeric guard — the deferred §5.8 stratification case */
    if (expect_reject("primed numeric guard",
        "sort actor\n"
        "entity guard : actor\n"
        "state (hp(actor) : int in 0..20  fell(actor))\n"
        "rule r(X: actor): hp(X)' <= 0 causes fell(X)\n",
        "primed numeric guard")) return 1;

    /* a primed judgment — a derived conclusion has no next-state form yet */
    if (expect_reject("primed judgment",
        "sort actor\n"
        "entity guard : actor\n"
        "state (poisoned(actor) alive(actor) drops(actor))\n"
        "rule weak(X: actor): poisoned(X) => hurt(X)\n"
        "rule r(X: actor): hurt(X)' & alive(X) causes drops(X)\n",
        "primes a judgment")) return 1;

    return 0;
}

int main(void)
{
    if (test_same_step_cascade()) return 1;
    if (test_prime_scope()) return 1;
    printf("test_ramif: all passed\n");
    return 0;
}
