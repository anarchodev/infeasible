/* Golden test for the set-quantified effect binder and enum value domains
 * (DESIGN.md §13, M1 effect-surface slice). Pins that `for each T: sort where
 * <guard>: <effect> [when <cond>]` grounds one guarded step-rule per target,
 * that the `where`/`when` guards filter the affected set at tick time, that
 * boolean / numeric / multi-valued effects all lower under the bound var, and
 * that `enum` declares a reusable value domain. Compiled from inline sources so
 * the surface, grounding, and step function are checked end-to-end. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f);
    return b;
}

/* Compile `src`; expect success. Returns the world (and its intern via *sy). */
static world *compile_ok(const char *src, intern **sy)
{
    *sy = intern_new();
    story_diag di[16];
    story_diags diags = { di, 16, 0, 0 };
    world *w = story_compile(src, "test.story", *sy, &diags);
    if (!w) {
        fprintf(stderr, "unexpected compile error: %s\n",
                diags.count ? di[0].msg : "(none)");
        return NULL;
    }
    if (diags.count)
        fprintf(stderr, "unexpected diagnostic: %s\n", di[0].msg);
    return diags.count == 0 ? w : NULL;
}

/* Compile `src`; expect an error whose message contains `needle`. */
static int compile_err(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags diags = { di, 16, 0, 0 };
    world *w = story_compile(src, "test.story", sy, &diags);
    if (w) { fprintf(stderr, "expected a compile error, got success\n"); return 1; }
    int hit = 0;
    for (int i = 0; i < diags.count; i++)
        if (strstr(di[i].msg, needle)) hit = 1;
    if (!hit)
        fprintf(stderr, "wanted error containing '%s'; got '%s'\n",
                needle, diags.count ? di[0].msg : "(none)");
    intern_free(sy);
    return hit ? 0 : 1;
}

/* --- A. AoE damage over a fluent `where` guard: only foes are hit --- */
static int test_where_filter(void)
{
    static const char *src =
        "sort actor\n"
        "entity ( mage, g1, g2, ally : actor )\n"
        "state ( hp(actor) : int in 0 .. 30   foe(actor) )\n"
        "init ( hp(mage)=10 hp(g1)=8 hp(g2)=8 hp(ally)=10  foe(g1) foe(g2) )\n"
        "action fireball(C: actor):\n"
        "    causes for each T: actor where foe(T): hp(T) -= 5\n";
    intern *sy; world *w = compile_ok(src, &sy); CHECK(w);
    uint32_t a = intern_id(sy, "fireball(mage)");
    char err[128];
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    /* foes take 5; non-foes untouched (the guard filtered them out) */
    CHECK(world_get_num(w, intern_id(sy, "hp(g1)"))   == 3);
    CHECK(world_get_num(w, intern_id(sy, "hp(g2)"))   == 3);
    CHECK(world_get_num(w, intern_id(sy, "hp(mage)")) == 10);
    CHECK(world_get_num(w, intern_id(sy, "hp(ally)")) == 10);
    world_free(w); intern_free(sy);
    return 0;
}

/* --- B. block form + per-target `when`: full damage vs. half on a "save" --- */
static int test_when_branch(void)
{
    static const char *src =
        "sort actor\n"
        "entity ( mage, g1, g2 : actor )\n"
        "state ( hp(actor) : int in 0 .. 30   foe(actor)  saved(actor) )\n"
        "init ( hp(g1)=20 hp(g2)=20  foe(g1) foe(g2)  saved(g1) )\n"
        "action blast(C: actor):\n"
        "    causes for each T: actor where foe(T): {\n"
        "        hp(T) -= 8 when ~saved(T) ,\n"
        "        hp(T) -= 4 when saved(T)\n"
        "    }\n";
    intern *sy; world *w = compile_ok(src, &sy); CHECK(w);
    uint32_t a = intern_id(sy, "blast(mage)");
    char err[128];
    CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(g1)")) == 16);  /* saved -> half (4) */
    CHECK(world_get_num(w, intern_id(sy, "hp(g2)")) == 12);  /* failed -> full (8) */
    world_free(w); intern_free(sy);
    return 0;
}

/* --- C. boolean + multi-valued binder effects, and an enum value domain --- */
static int test_bool_mv_enum(void)
{
    static const char *src =
        "sort actor\n"
        "enum condition { none, outlined, burning }\n"
        "entity ( druid, g1, g2, tree : actor )\n"
        "state ( foe(actor)  hidden(actor)  status(actor) : condition )\n"
        "init ( foe(g1) foe(g2)  hidden(g1) hidden(g2)\n"
        "       status(g1)=none status(g2)=none status(druid)=none status(tree)=none )\n"
        "action faerie_fire(C: actor):\n"
        "    causes for each T: actor where foe(T): ~hidden(T)\n"
        "action ignite(C: actor):\n"
        "    causes for each T: actor where foe(T): status(T) = burning\n";
    intern *sy; world *w = compile_ok(src, &sy); CHECK(w);
    char err[128];
    uint32_t ff = intern_id(sy, "faerie_fire(druid)");
    CHECK(world_step(w, &ff, 1, err, sizeof err) == 0);
    CHECK(world_query(w, dl_pos(intern_id(sy, "hidden(g1)"))) != DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "hidden(g2)"))) != DL_PROVED);
    uint32_t ig = intern_id(sy, "ignite(druid)");
    CHECK(world_step(w, &ig, 1, err, sizeof err) == 0);
    /* MV effect: the chosen value holds, siblings are negated */
    CHECK(world_get(w, intern_id(sy, "status(g1)=burning")));
    CHECK(!world_get(w, intern_id(sy, "status(g1)=none")));
    CHECK(!world_get(w, intern_id(sy, "status(tree)=burning")));  /* not a foe */
    world_free(w); intern_free(sy);
    return 0;
}

/* --- D. diagnostics: shadowing, unknown enum, and reserved `limit` --- */
static int test_errors(void)
{
    CHECK(compile_err(
        "sort actor\nentity ( a : actor )\nstate ( foe(actor) )\n"
        "action x(T: actor): causes for each T: actor where foe(T): foe(T)\n",
        "shadows") == 0);
    CHECK(compile_err(
        "sort actor\nentity ( a : actor )\nstate ( s(actor) : spell )\n",
        "not a declared enum") == 0);
    CHECK(compile_err(
        "sort actor\nentity ( a : actor )\nstate ( foe(actor) )\n"
        "action x(C: actor): causes for each T: actor where foe(T) limit 2: foe(T)\n",
        "limit") == 0);
    return 0;
}

/* --- E. the shipped spellbook example compiles and its AoE runs --- */
static int test_spellbook_example(void)
{
    char *src = read_file(STORY_DIR "/spellbook5e.story");
    CHECK(src != NULL);
    intern *sy; story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "spellbook5e.story", sy = intern_new(), &dg);
    if (!w) { fprintf(stderr, "spellbook5e.story: %s\n", dg.count ? di[0].msg : "?"); free(src); return 1; }
    CHECK(dg.count == 0);
    /* one goblin saved, one did not: half vs full from the same cast */
    char err[128];
    uint32_t ps = intern_id(sy, "pass_save(vera,grik)");
    CHECK(world_step(w, &ps, 1, err, sizeof err) == 0);
    uint32_t fb = intern_id(sy, "fireball(vera)");
    CHECK(world_step(w, &fb, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(grik)")) == 3);   /* saved: 7 - 4 */
    CHECK(world_get_num(w, intern_id(sy, "hp(gnok)")) == 0);   /* failed: 7 - 8 -> clamp 0 */
    CHECK(world_query(w, dl_pos(intern_id(sy, "down(gnok)"))) == DL_PROVED);
    world_free(w); intern_free(sy); free(src);
    return 0;
}

int main(void)
{
    if (test_where_filter()) return 1;
    if (test_when_branch())  return 1;
    if (test_bool_mv_enum()) return 1;
    if (test_errors())       return 1;
    if (test_spellbook_example()) return 1;
    printf("test_binder: all passed\n");
    return 0;
}
