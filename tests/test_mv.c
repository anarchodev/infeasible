/* Golden test for multi-valued fluents through the surface (DESIGN.md §5.7,
 * M1 slice b). Compiles examples/mv_door.story and pins the erasure the
 * compiler emits: a `: { … }` domain becomes one boolean value-atom per value,
 * an `init` assignment sets exactly one true, value guards read those atoms,
 * and a `causes f = v` effect expands to the family (chosen value + sibling
 * negations) — so exactly one value commits and a flip-flop is rejected with
 * the state untouched. This mirrors tests/test_multival.c's hand-built world
 * (test_flipflop_step, test_sealed_requires_holds) through the front door. */

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

static int test_mv_door(void)
{
    char *src = read_file(STORY_DIR "/mv_door.story");
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag ditems[16];
    story_diags diags = { ditems, 16, 0, 0 };
    world *w = story_compile(src, sy, &diags);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n",
                diags.count ? diags.items[0].msg : "(no message)");
        free(src); intern_free(sy);
        return 1;
    }
    if (diags.count)
        fprintf(stderr, "unexpected diagnostic: %s\n", diags.items[0].msg);
    CHECK(diags.count == 0);

    /* value-atoms intern as "door=<v>" — the erasure the compiler emits */
    uint32_t locked   = intern_id(sy, "door=locked"),
             closed   = intern_id(sy, "door=closed"),
             open     = intern_id(sy, "door=open"),
             can_pass = intern_id(sy, "can_pass(hero)"),
             a_unlock = intern_id(sy, "unlock(hero)"),
             a_shove  = intern_id(sy, "shove(hero)"),
             a_jopen  = intern_id(sy, "jam_open"),
             a_jclose = intern_id(sy, "jam_closed");
    char err[256];

    /* init: exactly one value holds */
    CHECK(world_get(w, locked) && !world_get(w, closed) && !world_get(w, open));
    /* the value guard reads through: door != open, so can_pass is refuted */
    CHECK(world_query(w, dl_pos(can_pass)) == DL_REFUTED);

    /* unlock: has_key(hero) & door=locked both hold -> door lands on closed,
     * the family flips locked off (exactly one value) */
    CHECK(world_step(w, &a_unlock, 1, err, sizeof err) == 0);
    CHECK(world_get(w, closed) && !world_get(w, locked) && !world_get(w, open));

    /* unlock again: requires door=locked, now false -> inertia holds, no change
     * (the hard-precondition pattern — test_sealed_requires_holds analog) */
    CHECK(world_step(w, &a_unlock, 1, err, sizeof err) == 0);
    CHECK(world_get(w, closed) && !world_get(w, locked) && !world_get(w, open));

    /* shove: door=closed -> door=open; now the value guard proves can_pass */
    CHECK(world_step(w, &a_shove, 1, err, sizeof err) == 0);
    CHECK(world_get(w, open) && !world_get(w, closed) && !world_get(w, locked));
    CHECK(world_query(w, dl_pos(can_pass)) == DL_PROVED);

    /* flip-flop: two writers force different values in one step -> contested,
     * returns -1 with the state untouched (door stays open) */
    uint32_t both[2] = { a_jopen, a_jclose };
    err[0] = '\0';
    CHECK(world_step(w, both, 2, err, sizeof err) == -1);
    CHECK(err[0] != '\0');
    CHECK(world_get(w, open) && !world_get(w, closed) && !world_get(w, locked));

    /* a single writer commits exactly one value */
    CHECK(world_step(w, &a_jclose, 1, err, sizeof err) == 0);
    CHECK(world_get(w, closed) && !world_get(w, open) && !world_get(w, locked));

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

/* MV-specific error paths: value outside the domain, a bare MV atom, a `= v`
 * on a non-MV fluent, a value guard on an undeclared fluent, and concluding a
 * value from a judgment rule (deferred — needs §5.7 reification). */
static int expect_error(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag ditems[8];
    story_diags diags = { ditems, 8, 0, 0 };
    world *w = story_compile(src, sy, &diags);
    int ok = (w == NULL) && (diags.nerrors >= 1) &&
             (diags.items[0].sev == STORY_ERROR);
    if (ok && needle && diags.count)
        ok = strstr(diags.items[0].msg, needle) != NULL;
    if (!ok)
        fprintf(stderr, "FAIL expected error<<%s>> for <<%s>> (nerrors=%d, msg=%s)\n",
                needle ? needle : "", src, diags.nerrors,
                diags.count ? diags.items[0].msg : "");
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_mv_errors(void)
{
    /* value not in the declared domain */
    if (expect_error("state d : { a, b }\ninit d = c", "not a value")) return 1;
    /* an MV fluent used as a bare boolean atom */
    if (expect_error("state d : { a, b }\nrule r: d => q", "multi-valued")) return 1;
    /* `= value` on a boolean fluent */
    if (expect_error("state p\nrule r: p = a => q", "not a declared multi-valued"))
        return 1;
    /* concluding a value from a judgment rule is deferred */
    if (expect_error("state d : { a, b }\nrule r: p => d = a", "not supported yet"))
        return 1;
    /* a one-value domain is rejected */
    if (expect_error("state d : { only }", "at least two values")) return 1;
    return 0;
}

int main(void)
{
    if (test_mv_door())    return 1;
    if (test_mv_errors())  return 1;
    printf("test_mv: all passed\n");
    return 0;
}
