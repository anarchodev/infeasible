/* Init-fact list grows to fit — no fixed 256 cap (loud failures, GENEROUS limits).
 * A story with 1000 init facts used to fail with "too many init facts (max 256)";
 * now the parser grows the list geometrically, with a runaway ceiling far above
 * any real content. Regression for the MAX_INITS fix. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#define N 1000   /* > the old 256 cap */

int main(void)
{
    /* generous buffer: N entity decls + N init facts */
    size_t cap = (size_t)N * 40 + 4096;
    char *s = malloc(cap);
    int n = 0;
    #define EMIT(...) do { n += snprintf(s + n, cap - (size_t)n, __VA_ARGS__); } while (0)
    EMIT("scene many\nsort actor\nentity (\n");
    for (int i = 0; i < N; i++) EMIT("  a%d%s", i, i + 1 < N ? "," : " : actor\n");
    EMIT(")\nstate ( flag(actor) )\ninit (\n");
    for (int i = 0; i < N; i++) EMIT("  flag(a%d)\n", i);   /* N > 256 init facts */
    EMIT(")\n");
    #undef EMIT

    intern *sy = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(s, "many.story", sy, &dg);
    if (!w) fprintf(stderr, "compile failed: %s\n", dg.count ? di[0].msg : "?");
    CHECK(w);
    CHECK(dg.nerrors == 0);

    /* the facts past the old cap are actually set */
    CHECK(world_get(w, intern_id(sy, "flag(a0)")));
    CHECK(world_get(w, intern_id(sy, "flag(a500)")));
    CHECK(world_get(w, intern_id(sy, "flag(a999)")));   /* well past 256 */
    /* an unlisted atom is closed-world false */
    world_declare_fluent(w, intern_id(sy, "flag(a0)"));  /* already declared; no-op */
    CHECK(!world_get(w, intern_id(sy, "nope(a0)")));

    world_free(w);
    intern_free(sy);
    free(s);
    printf("test_inits: all passed\n");
    return 0;
}
