/* Golden test for provenance in the why-trace (DESIGN.md §6.3). The debugger is
 * the product, and it traces machinery the author never wrote; every rule must
 * carry its source span so a trace reads in source terms, not internal ids. This
 * pins that an authored rule renders `label (kind; srcname:line): …` and that a
 * NULL srcname falls back to "<story>". The format lives once in dl_trace.c and
 * is shared by both backings, so pinning it here covers the columnar trace too. */

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

/* Capture world_why(q) into a heap string the caller frees. */
static char *why_str(world *w, dl_lit q)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(w, q, m);
    fclose(m);
    return buf;
}

/* line 1: sort actor
 * line 2: entity guard : actor
 * line 3: state (poisoned(actor) weak(actor) safe(actor))
 * line 4: rule weakens(X: actor): poisoned(X) => weak(X) unless safe(X)
 * line 5: init poisoned(guard) */
static const char *SRC =
    "sort actor\n"
    "entity guard : actor\n"
    "state (poisoned(actor) weak(actor) safe(actor))\n"
    "rule weakens(X: actor): poisoned(X) => weak(X) unless safe(X)\n"
    "init poisoned(guard)\n";

static int test_authored_span(void)
{
    intern *sy = intern_new();
    world *w = story_compile(SRC, "cellar.story", sy, NULL);
    CHECK(w != NULL);

    char *t = why_str(w, dl_pos(intern_id(sy, "weak(guard)")));
    /* the rule renders in source terms with its span (the `rule` is on line 4) */
    CHECK(strstr(t, "weakens[X=guard] (defeasible; cellar.story:4):") != NULL);
    /* the `unless` defeater is generated from the same construct — same span */
    CHECK(strstr(t, "cellar.story:4)") != NULL);
    free(t);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* A NULL srcname still produces a span, under the "<story>" fallback name. */
static int test_null_srcname(void)
{
    intern *sy = intern_new();
    world *w = story_compile(SRC, NULL, sy, NULL);
    CHECK(w != NULL);

    char *t = why_str(w, dl_pos(intern_id(sy, "weak(guard)")));
    CHECK(strstr(t, "(defeasible; <story>:4):") != NULL);
    free(t);

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_authored_span()) return 1;
    if (test_null_srcname()) return 1;
    printf("test_prov: all passed\n");
    return 0;
}
