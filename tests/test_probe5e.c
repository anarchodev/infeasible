/* Golden test for the modernized 5e probes (#8): combat5e.story,
 * srd_probe.story, srd_probe2.story — the three files that were aspirational
 * frontier markers until the M1 surface caught up. This pins that they don't
 * just compile (the scoreboard) but that their load-bearing claims HOLD:
 *
 *  combat5e — defeat-the-definitions: a `~>` defeater against a value
 *    definition's marker blocks the min-class layer (freedom of movement
 *    unroots a restrained actor); the fey-ancestry pairwise-sup cluster
 *    (sleep blocked, zero-hp override wins); the bloodied expression guard;
 *    the stratified dying trigger + same-step death_drop cascade.
 *
 *  srd_probe — binder AoE with engine-side dice: the save judgment read
 *    pre-step agrees with the branch the step took (§5.10 same-tick
 *    coherence), full/half halves the SAME per-target draw (named values);
 *    seed-replay determinism (I4); the concentration BREAKING-EDGE retract
 *    drops the derived faerie-fire mark in the same step.
 *
 *  srd_probe2 — pool summon via function-provider placement; the provider-
 *    gated wall-of-fire tick. */

#include "lang/story.h"
#include "stock/grid.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

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
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = 0;
    return buf;
}

static world *compile_story(const char *name, intern *sy)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", STORY_DIR, name);
    char *src = read_file(path);
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, name, sy, &d);
    free(src);
    if (!w || d.nerrors) {
        fprintf(stderr, "%s: compile failed: %s\n", name,
                d.count ? di[0].msg : "?");
        return NULL;
    }
    return w;
}

static dl_verdict q(world *w, intern *sy, const char *atom)
{
    return world_query(w, (dl_lit){ intern_id(sy, atom), false });
}

static int step1(world *w, intern *sy, const char *action)
{
    uint32_t a = intern_id(sy, action);
    char err[128];
    return world_step(w, &a, 1, err, sizeof err);
}

/* ---- combat5e ------------------------------------------------------------ */

static bool always_true(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)ctx; (void)pred; (void)args; (void)nargs;
    return true;   /* adjacent / los: everyone is in reach for this test */
}

static int test_combat5e(void)
{
    intern *sy = intern_new();
    world *w = compile_story("combat5e.story", sy);
    CHECK(w != NULL);
    world_set_provider_fn(w, always_true, NULL);

    /* speed through its reader: 30 at rest */
    CHECK(q(w, sy, "rooted(aria)") == DL_REFUTED);

    /* restrained: the min-class layer takes speed to 0 */
    world_set(w, intern_id(sy, "restrained(aria)"), true);
    CHECK(q(w, sy, "rooted(aria)") == DL_PROVED);

    /* DEFEAT THE DEFINITIONS: freedom of movement blocks the layer's marker,
     * the base stands — "your speed can't be reduced" */
    world_set(w, intern_id(sy, "freedom_of_movement(aria)"), true);
    CHECK(q(w, sy, "rooted(aria)") == DL_REFUTED);

    /* fey ancestry beats sleep_takes: magical sleep doesn't take an elf */
    world_set(w, intern_id(sy, "slept(aria)"), true);
    CHECK(q(w, sy, "unconscious(aria)") == DL_REFUTED);

    /* ...but zero_hp_ko beats fey_ancestry (the pair-scoped override) */
    world_set_num(w, intern_id(sy, "hp(aria)"), 0);
    CHECK(q(w, sy, "unconscious(aria)") == DL_PROVED);
    world_set_num(w, intern_id(sy, "hp(aria)"), 20);

    /* bloodied: the parenthesized expression guard (2*hp <= hp_max) */
    CHECK(q(w, sy, "bloodied(grunk)") == DL_REFUTED);
    world_set_num(w, intern_id(sy, "hp(grunk)"), 3);
    CHECK(q(w, sy, "bloodied(grunk)") == DL_PROVED);
    CHECK(q(w, sy, "wants_flee(grunk)") == DL_PROVED);
    world_set_num(w, intern_id(sy, "hp(grunk)"), 7);

    /* the dying trigger + the same-step cascade: two strikes fell grunk;
     * the primed guard sees hp' clamped to 0, dead lands, and death_drop
     * puts the shortbow on the floor in the SAME tick */
    CHECK(step1(w, sy, "sword_strike(aria,grunk)") == 0);   /* 7 -> 1 */
    CHECK(q(w, sy, "dead(grunk)") == DL_REFUTED);
    CHECK(step1(w, sy, "sword_strike(aria,grunk)") == 0);   /* 1 -> 0 (clamp) */
    CHECK(world_get_num(w, intern_id(sy, "hp(grunk)")) == 0);
    CHECK(q(w, sy, "dead(grunk)") == DL_PROVED);
    CHECK(q(w, sy, "on_floor(shortbow)") == DL_PROVED);
    CHECK(q(w, sy, "holding(grunk,shortbow)") == DL_REFUTED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- srd_probe ----------------------------------------------------------- */

static long run_fireball(uint64_t seed, long hp_out[4], int saved_out[4])
{
    static const char *T[4] = { "grik", "gnok", "gob", "thorn" };
    intern *sy = intern_new();
    world *w = compile_story("srd_probe.story", sy);
    if (!w) return -1;
    /* the STOCK grid, over actors and the aim cells together — the story's
     * own `grid_chebyshev(T, c_pack) <= 4` decides the blast, so this test no
     * longer supplies a hand-written `in_blast` the story had to trust (#253) */
    static const char *G[] = { "vera", "grik", "gnok", "gob", "thorn",
                               "c_pack", "c_far" };
    uint32_t ge[7];
    for (int i = 0; i < 7; i++) ge[i] = intern_id(sy, G[i]);
    stock_grid *grid = stock_grid_install(w, sy, ge, 7);
    world_set_seed(w, seed);

    /* the save judgment, read PRE-step: §5.10 keys rolls per tick, so this
     * is the same draw the step's `when` branches will see */
    for (int i = 0; i < 4; i++) {
        char b[32];
        snprintf(b, sizeof b, "saved(%s)", T[i]);
        saved_out[i] = q(w, sy, b) == DL_PROVED;
    }
    if (step1(w, sy, "fireball(vera,c_pack)") != 0)
        { stock_grid_free(grid); world_free(w); intern_free(sy); return -1; }
    for (int i = 0; i < 4; i++) {
        char b[32];
        snprintf(b, sizeof b, "hp(%s)", T[i]);
        hp_out[i] = world_get_num(w, intern_id(sy, b));
    }
    long slots = world_get_num(w, intern_id(sy, "slots3(vera)"));
    world_free(w);
    intern_free(sy);
    stock_grid_free(grid);
    return slots;
}

static int test_srd_probe(void)
{
    static const long MAXHP[4] = { 7, 7, 7, 30 };
    long hp[4], hp2[4];
    int sv[4], sv2[4];

    long slots = run_fireball(42, hp, sv);
    CHECK(slots == 1);                                  /* 2 -> 1 */
    for (int i = 0; i < 4; i++) {
        long delta = MAXHP[i] - hp[i];
        if (MAXHP[i] == 7 && hp[i] == 0) continue;      /* clamped: bound only */
        if (sv[i]) CHECK(delta >= 1 && delta <= 9);     /* half of 3..18, floored */
        else       CHECK(delta >= 3 && delta <= 18);    /* the full 3d6 */
    }

    /* I4: same seed, same world, same dice — the trajectory replays exactly */
    CHECK(run_fireball(42, hp2, sv2) == 1);
    for (int i = 0; i < 4; i++) { CHECK(hp[i] == hp2[i]); CHECK(sv[i] == sv2[i]); }

    /* concentration: cast marks the relation, the derived projection holds;
     * the break retracts the caster's set on the BREAKING EDGE, same step */
    intern *sy = intern_new();
    world *w = compile_story("srd_probe.story", sy);
    CHECK(w != NULL);
    static const char *G2[] = { "vera", "grik", "gnok", "gob", "thorn",
                                "c_pack", "c_far" };
    uint32_t ge2[7];
    for (int i = 0; i < 7; i++) ge2[i] = intern_id(sy, G2[i]);
    stock_grid *grid2 = stock_grid_install(w, sy, ge2, 7);
    world_set_seed(w, 7);
    CHECK(step1(w, sy, "cast_faerie_fire(vera,c_pack)") == 0);
    CHECK(q(w, sy, "faerie_fired(grik)") == DL_PROVED);   /* derived projection */
    CHECK(q(w, sy, "faerie_fired(thorn)") == DL_REFUTED); /* not a monster */
    CHECK(q(w, sy, "hidden(grik)") == DL_REFUTED);        /* outlined */
    CHECK(step1(w, sy, "break_concentration(vera)") == 0);
    CHECK(q(w, sy, "concentrating(vera)") == DL_REFUTED);
    CHECK(q(w, sy, "faerie_fired(grik)") == DL_REFUTED);  /* retracted with it */
    stock_grid_free(grid2);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- srd_probe2 ---------------------------------------------------------- */

static int test_srd_probe2(void)
{
    intern *sy = intern_new();
    world *w = compile_story("srd_probe2.story", sy);
    CHECK(w != NULL);
    /* the STOCK grid, over actors and the aim cells. Nothing here answers
     * occupancy any more: the wall's extent is state the story writes, and
     * "standing in it" is a distance the library measures (#253). */
    static const char *G3[] = { "dara", "wolf1", "wolf2", "ogre",
                                "c_gap", "c_ford" };
    uint32_t ge3[6];
    for (int i = 0; i < 6; i++) ge3[i] = intern_id(sy, G3[i]);
    stock_grid *grid3 = stock_grid_install(w, sy, ge3, 6);

    /* summon: pool slots come up placed and provisioned */
    CHECK(q(w, sy, "active(wolf1)") == DL_REFUTED);
    CHECK(step1(w, sy, "summon_two_wolves(dara)") == 0);
    CHECK(q(w, sy, "active(wolf1)") == DL_PROVED);
    CHECK(q(w, sy, "active(wolf2)") == DL_PROVED);
    CHECK(world_get_num(w, intern_id(sy, "hp(wolf1)")) == 11);
    /* placement is arithmetic on the caster's coordinates, not a host call */
    CHECK(world_get_num(w, intern_id(sy, "grid_x(wolf1)")) == 3);
    CHECK(world_get_num(w, intern_id(sy, "grid_x(wolf2)")) == 1);
    stock_grid_refresh(grid3);

    /* THE WALL. Extent is state the cast writes, so nothing outside the world
     * has to remember where the line went — which is what makes it replay. */
    CHECK(step1(w, sy, "cast_wall_of_fire(dara,c_ford)") == 0);
    CHECK(q(w, sy, "on_fire(c_ford)") == DL_PROVED);
    char err[128];
    CHECK(world_step(w, NULL, 0, err, sizeof err) == 0);   /* an empty step */
    /* the ogre is standing on the forded cell; dara is four squares back */
    CHECK(world_get_num(w, intern_id(sy, "hp(ogre)")) == 54);   /* 59 - 5 */
    CHECK(world_get_num(w, intern_id(sy, "hp(dara)")) == 24);   /* not in it */

    stock_grid_free(grid3);
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_combat5e())  return 1;
    if (test_srd_probe()) return 1;
    if (test_srd_probe2()) return 1;
    printf("test_probe5e: all passed\n");
    return 0;
}
