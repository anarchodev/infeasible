/* Golden test for PROVIDERS (§5.2/§5.6) and SEEDED ROLL (§5.10) — the two
 * mechanisms a faithful 5e spell needs: host-answered targeting and engine-side
 * dice. A provider is a computed relation consulted at solve time and never
 * stored; a roll is a keyed lookup hash(seed, tick, site), deterministic and
 * replayable. Together they let `for each T where in_blast(T): hp(T) -= roll(6)`
 * express Fireball with the host supplying only geometry + a seed. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

static world *compile(const char *src, intern *sy)
{
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = '\0';
    fclose(f); return b;
}

/* host geometry: `in_blast(T)` / `in_range(C,T)` true iff the TARGET (last arg)
 * is in the host-controlled set. ctx points at the current set. */
typedef struct { uint32_t in[8]; int n; } blastset;
static bool geom(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)pred;
    const blastset *b = ctx;
    uint32_t target = args[nargs - 1];
    for (int i = 0; i < b->n; i++) if (b->in[i] == target) return true;
    return false;
}

/* --- providers: relation gating a binder AoE + a judgment query --- */
static int test_provider(void)
{
    static const char *src =
        "sort actor\n"
        "entity ( wiz, g0, g1, g2 : actor )\n"
        "provider in_range(actor, actor)\n"
        "state ( hp(actor) : int in 0 .. 40 )\n"
        "init ( hp(wiz)=40 hp(g0)=20 hp(g1)=20 hp(g2)=20 )\n"
        "action fireball(C: actor): causes for each T: actor where in_range(C, T): hp(T) -= 8\n"
        "rule flanked(X: actor): in_range(wiz, X) => flanked(X)\n";
    intern *sy = intern_new();
    world *w = compile(src, sy); CHECK(w);

    blastset set = { { intern_id(sy, "g0"), intern_id(sy, "g1") }, 2 };
    world_set_provider_fn(w, geom, &set);

    /* judgment query consults the provider (jfam path) */
    CHECK(world_query(w, dl_pos(intern_id(sy, "flanked(g0)"))) == DL_PROVED);
    CHECK(world_query(w, dl_pos(intern_id(sy, "flanked(g2)"))) != DL_PROVED);

    /* the AoE hits exactly the provider-answered set */
    char err[128];
    uint32_t cast = intern_id(sy, "fireball(wiz)");
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(g0)"))  == 12);
    CHECK(world_get_num(w, intern_id(sy, "hp(g1)"))  == 12);
    CHECK(world_get_num(w, intern_id(sy, "hp(g2)"))  == 20);   /* not in range */
    CHECK(world_get_num(w, intern_id(sy, "hp(wiz)")) == 40);

    /* the provider is re-consulted each step: move g2 in, g0 out, recast */
    set.in[0] = intern_id(sy, "g2");                            /* now {g2, g1} */
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(g2)")) == 12);     /* now in range */
    CHECK(world_get_num(w, intern_id(sy, "hp(g0)")) == 12);     /* no longer hit */
    CHECK(world_get_num(w, intern_id(sy, "hp(g1)")) == 4);      /* hit twice */

    world_free(w); intern_free(sy);
    return 0;
}

/* --- seeded roll: range, tick advance, and exact replay from the seed --- */
static int run_zaps(uint64_t seed, long *out /* hp of g0 after each of 3 zaps */)
{
    static const char *src =
        "sort actor\n"
        "entity ( g0 : actor )\n"
        "state ( hp(actor) : int in 0 .. 100 )\n"
        "init ( hp(g0)=100 )\n"
        "action zap(T: actor): causes hp(T) -= roll(6)\n";
    intern *sy = intern_new();
    world *w = compile(src, sy); CHECK(w);
    world_set_seed(w, seed);
    uint32_t z = intern_id(sy, "zap(g0)");
    char err[128];
    long prev = 100;
    for (int i = 0; i < 3; i++) {
        CHECK((long)world_tick(w) == i);               /* monotone step counter */
        CHECK(world_step(w, &z, 1, err, sizeof err) == 0);
        long now = world_get_num(w, intern_id(sy, "hp(g0)"));
        long dmg = prev - now;
        CHECK(dmg >= 1 && dmg <= 6);                    /* a d6 */
        out[i] = now; prev = now;
    }
    CHECK((long)world_tick(w) == 3);
    world_free(w); intern_free(sy);
    return 0;
}

static int test_roll(void)
{
    long a[3], b[3], c[3];
    if (run_zaps(0xABCDEF, a)) return 1;
    if (run_zaps(0xABCDEF, b)) return 1;   /* same seed -> exact replay */
    if (run_zaps(0x123456, c)) return 1;   /* different seed */
    CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);   /* I4: replay is exact */
    /* a fresh roll each tick: the three draws are not all identical */
    long d0 = 100 - a[0], d1 = a[0] - a[1], d2 = a[1] - a[2];
    CHECK(!(d0 == d1 && d1 == d2));
    /* different seed selects a different table (extremely likely to differ) */
    CHECK(!(a[0] == c[0] && a[1] == c[1] && a[2] == c[2]));
    return 0;
}

/* --- combined: provider targeting + engine-side roll damage (mini Fireball) --- */
static int test_faithful(void)
{
    static const char *src =
        "sort actor\n"
        "entity ( wiz, g0, g1, g2 : actor )\n"
        "provider in_blast(actor)\n"
        "state ( hp(actor) : int in 0 .. 40 )\n"
        "init ( hp(g0)=40 hp(g1)=40 hp(g2)=40 )\n"
        "action fireball(C: actor): causes for each T: actor where in_blast(T): hp(T) -= roll(6)\n";
    intern *sy = intern_new();
    world *w = compile(src, sy); CHECK(w);
    blastset set = { { intern_id(sy, "g0"), intern_id(sy, "g1") }, 2 };
    world_set_provider_fn(w, geom, &set);
    world_set_seed(w, 999);

    char err[128];
    uint32_t cast = intern_id(sy, "fireball(wiz)");
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);
    long h0 = world_get_num(w, intern_id(sy, "hp(g0)"));
    long h1 = world_get_num(w, intern_id(sy, "hp(g1)"));
    CHECK(40 - h0 >= 1 && 40 - h0 <= 6);               /* g0 took 1d6 */
    CHECK(40 - h1 >= 1 && 40 - h1 <= 6);               /* g1 took 1d6 */
    CHECK(world_get_num(w, intern_id(sy, "hp(g2)")) == 40);   /* not in blast */
    world_free(w); intern_free(sy);
    return 0;
}

/* --- the shipped example: faithful Fireball (provider targeting + seeded roll) --- */
static int test_example(void)
{
    char *src = slurp(STORY_DIR "/fireball5e.story");
    CHECK(src != NULL);
    intern *sy = intern_new();
    world *w = compile(src, sy); CHECK(w);
    blastset set = { { intern_id(sy, "grik"), intern_id(sy, "gnok"), intern_id(sy, "gob") }, 3 };
    world_set_provider_fn(w, geom, &set);
    world_set_seed(w, 0x5EED);
    char err[128];
    uint32_t cast = intern_id(sy, "fireball(vera)");
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);
    /* 3d6 => 3..18 to the in-blast goblins; vera/thorn untouched (not in blast) */
    for (int i = 0; i < 3; i++) {
        const char *g[] = { "grik", "gnok", "gob" };
        char b[32]; snprintf(b, sizeof b, "hp(%s)", g[i]);
        long dmg = 14 - world_get_num(w, intern_id(sy, b));
        CHECK(dmg >= 3 && dmg <= 18);
    }
    CHECK(world_get_num(w, intern_id(sy, "hp(vera)"))  == 22);
    CHECK(world_get_num(w, intern_id(sy, "hp(thorn)")) == 30);
    world_free(w); intern_free(sy); free(src);
    return 0;
}

/* adjacency: any two distinct actors (melee brawl) */
static bool adj_any(void *ctx, uint32_t pred, const uint32_t *a, int n)
{ (void)ctx; (void)pred; return n == 2 && a[0] != a[1]; }

/* --- roll inside a guard: the d20. Varies across ticks; replays exactly. --- */
static int test_roll_guard(void)
{
    static const char *src =
        "sort actor\nentity ( a, b : actor )\n"
        "state ( dummy(actor)  atk(actor):int  ac(actor):int )\n"
        "init ( atk(a)=5 ac(b)=15 )\n"
        "rule hit(A: actor, T: actor): roll(20) + atk(A) >= ac(T) -> hit(A, T)\n"
        "action nop(A: actor): causes dummy(A)\n";
    /* two worlds, same seed: the roll-guard sequence must vary AND be identical */
    int seq[2][40];
    for (int run = 0; run < 2; run++) {
        intern *sy = intern_new(); world *w = compile(src, sy); CHECK(w);
        world_set_seed(w, 0xD20D20);
        uint32_t nop = intern_id(sy, "nop(a)"); char err[128];
        for (int i = 0; i < 40; i++) {
            seq[run][i] = world_query(w, dl_pos(intern_id(sy, "hit(a,b)"))) == DL_PROVED;
            CHECK(world_step(w, &nop, 1, err, sizeof err) == 0);
        }
        world_free(w); intern_free(sy);
    }
    int hits = 0;
    for (int i = 0; i < 40; i++) {
        CHECK(seq[0][i] == seq[1][i]);              /* I4: same seed replays exactly */
        hits += seq[0][i];
    }
    CHECK(hits > 5 && hits < 35);                   /* neither stuck-miss nor stuck-hit (~55%) */
    return 0;
}

/* --- the shipped combat example: engine-side rolls + provider, replay-exact --- */
static int test_combat_replay(void)
{
    char *src = slurp(STORY_DIR "/combat_srd.story");
    CHECK(src != NULL);
    const char *script[] = { "shove(aria,grunk)", "strike(snik,aria)",
                             "strike(aria,grunk)", "strike(aria,snik)",
                             "strike(snik,aria)", "strike(aria,snik)" };
    long fin[2][3];
    for (int run = 0; run < 2; run++) {
        intern *sy = intern_new(); world *w = compile(src, sy); CHECK(w);
        world_set_provider_fn(w, adj_any, NULL);
        world_set_seed(w, 0x5E5A11);
        char err[128];
        for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
            uint32_t a = intern_id(sy, script[i]);
            CHECK(world_step(w, &a, 1, err, sizeof err) == 0);
        }
        fin[run][0] = world_get_num(w, intern_id(sy, "hp(aria)"));
        fin[run][1] = world_get_num(w, intern_id(sy, "hp(grunk)"));
        fin[run][2] = world_get_num(w, intern_id(sy, "hp(snik)"));
        world_free(w); intern_free(sy);
    }
    /* same seed + same action log => identical final state (I4 replay) */
    for (int k = 0; k < 3; k++) CHECK(fin[0][k] == fin[1][k]);
    /* and the fight actually happened (engine-side rolls landed some blows) */
    CHECK(fin[0][0] < 24 || fin[0][1] < 7 || fin[0][2] < 7);
    free(src);
    return 0;
}

int main(void)
{
    if (test_provider())      return 1;
    if (test_roll())          return 1;
    if (test_faithful())      return 1;
    if (test_example())       return 1;
    if (test_roll_guard())    return 1;
    if (test_combat_replay()) return 1;
    printf("test_provider: all passed\n");
    return 0;
}
