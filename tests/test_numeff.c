/* Golden test for the numeric-fluent WRITE side through the surface language
 * (DESIGN.md §5.8, M1 slice c2 — lang). Compiles examples/numeric_combat.story
 * and drives the step function, pinning that the effect operators (`:=`/`+=`/
 * `-=`), the expression VM (`max(hp, 10)`), summation of two deltas on one
 * tick, and the declared-range clamp all lower correctly — the surface twin of
 * what test_numpipe.c pins at the engine C API. The commit receipt and the
 * guard read side (a `<= 0` guard concluding `dead`) are checked end-to-end. */

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

static int test_combat(void)
{
    char *src = read_file(STORY_DIR "/numeric_combat.story");
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

    uint32_t hp_hero  = intern_id(sy, "hp(hero)"),
             hp_gob   = intern_id(sy, "hp(goblin)"),
             burn_h   = intern_id(sy, "burning(hero)"),
             dead_gob = intern_id(sy, "dead(goblin)"),
             /* ground action atoms: "name(args)" over the chosen entities */
             a_strike_hg = intern_id(sy, "strike(hero,goblin)"),
             a_immol_g   = intern_id(sy, "immolate(goblin)"),
             a_immol_h   = intern_id(sy, "immolate(hero)"),
             a_mend_g    = intern_id(sy, "mend(goblin)");
    char err[256];

    /* init values landed */
    CHECK(world_get_num(w, hp_hero) == 12);
    CHECK(world_get_num(w, hp_gob)  == 7);

    /* a constant `-=` delta: goblin 7 -> 3 */
    CHECK(world_step(w, &a_strike_hg, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp_gob) == 3);
    CHECK(world_query(w, dl_pos(dead_gob)) != DL_PROVED);

    /* the receipt itemizes the delta with its ground source rule */
    {
        long base;
        world_contrib items[4];
        int n = world_num_receipt(w, hp_gob, &base, items, 4);
        CHECK(base == 7 && n == 1);
        CHECK(items[0].op == WORLD_OP_SUB && items[0].amount == -4);
        CHECK(strcmp(items[0].rule, "strike[A=hero,T=goblin]") == 0);
    }

    /* another strike crosses the `<= 0` guard: 3 -> -1 clamps to 0, dead fires */
    CHECK(world_step(w, &a_strike_hg, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp_gob) == 0);          /* range 0..20 clamps -1 */
    CHECK(world_query(w, dl_pos(dead_gob)) == DL_PROVED);

    /* an expression RHS with a fluent read: mend goblin to max(0, 10) = 10 */
    CHECK(world_step(w, &a_mend_g, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp_gob) == 10);

    /* two effects in one action (delta + status flip): hero 12 -> 6, burning */
    CHECK(world_step(w, &a_immol_h, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp_hero) == 6);
    CHECK(world_get(w, burn_h));

    /* two damage sources on ONE tick sum, order-free: goblin 10 - 4 - 6 = 0 */
    uint32_t both[] = { a_strike_hg, a_immol_g };
    CHECK(world_step(w, both, 2, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp_gob) == 0);          /* 10 - 10, clamp floor 0 */

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

int main(void)
{
    if (test_combat()) return 1;
    printf("test_numeff: all passed\n");
    return 0;
}
