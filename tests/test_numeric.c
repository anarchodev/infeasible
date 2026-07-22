/* Golden test for numeric fluents + landmark guards through the surface
 * (DESIGN.md §5.8, M1 slice c1 — the read side). Compiles
 * examples/numeric_hp.story and pins that comparison guards track the value
 * store exactly, in both the query path (judgments) and the columnar step path
 * (a numeric-guarded action). The value store stands in for the not-yet-built
 * effect pipeline: the test writes hp with world_set_num, exactly as
 * test_landmark.c's notional provider buckets a value. */

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

static int test_numeric_hp(void)
{
    char *src = read_file(STORY_DIR "/numeric_hp.story");
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

    uint32_t hp_hero  = intern_id(sy, "hp(hero)"),
             hp_guard = intern_id(sy, "hp(guard)"),
             down_h   = intern_id(sy, "down(hero)"),
             down_g   = intern_id(sy, "down(guard)"),
             blood_h  = intern_id(sy, "bloodied(hero)"),
             blood_g  = intern_id(sy, "bloodied(guard)"),
             fleeing_g= intern_id(sy, "fleeing(guard)"),
             a_flee_g = intern_id(sy, "flee(guard)");
    char err[256];

    /* init values landed in the store */
    CHECK(world_get_num(w, hp_hero) == 7);
    CHECK(world_get_num(w, hp_guard) == 15);

    /* hp(hero)=7: bloodied but standing; hp(guard)=15: healthy */
    CHECK(world_query(w, dl_pos(down_h))  == DL_REFUTED);
    CHECK(world_query(w, dl_pos(blood_h)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(down_g))  == DL_REFUTED);
    CHECK(world_query(w, dl_pos(blood_g)) == DL_REFUTED);

    /* drop hero to 0: the strict guard fires down, and bloodied still holds
     * (the guard atoms re-bucket from the value with no rule change) */
    world_set_num(w, hp_hero, 0);
    CHECK(world_query(w, dl_pos(down_h))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(blood_h)) == DL_PROVED);

    /* the step path reads guards too: flee(guard) needs hp(guard) < 10 &
     * alerted(guard). At 15 the guard fails, so nothing fires. */
    CHECK(world_step(w, &a_flee_g, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, fleeing_g));

    /* wound the guard below the landmark; now the same action fires */
    world_set_num(w, hp_guard, 5);
    CHECK(world_query(w, dl_pos(blood_g)) == DL_PROVED);
    CHECK(world_step(w, &a_flee_g, 1, err, sizeof err) == 0);
    CHECK(world_get(w, fleeing_g));

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

static int expect_error(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag ditems[8];
    story_diags diags = { ditems, 8, 0, 0 };
    world *w = story_compile(src, NULL, sy, &diags);
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

static int test_numeric_errors(void)
{
    /* a numeric fluent read without a comparison */
    if (expect_error("state h : int\nrule r: h => q", "read it with a comparison"))
        return 1;
    /* a comparison against a non-numeric fluent */
    if (expect_error("state p\nrule r: p <= 5 => q", "not a declared numeric fluent"))
        return 1;
    /* a numeric fluent is *written* in a `causes` clause, never compared */
    if (expect_error("state h : int\naction a: causes h = 5", "effect operator"))
        return 1;
    /* init uses `=`, not a comparison */
    if (expect_error("state h : int\ninit h <= 5", "sets a numeric fluent"))
        return 1;
    /* a rule cannot conclude a comparison */
    if (expect_error("state h : int\nrule r: q -> h <= 0", "cannot conclude"))
        return 1;
    /* a declared range with hi below lo is empty */
    if (expect_error("state h : int in 5..2", "range is empty")) return 1;
    return 0;
}

int main(void)
{
    if (test_numeric_hp())     return 1;
    if (test_numeric_errors()) return 1;
    printf("test_numeric: all passed\n");
    return 0;
}
