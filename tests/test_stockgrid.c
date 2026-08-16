/* Golden test for the stock square-grid provider (§5.6, #255).
 *
 * The acceptance test is the last case: a `.story` with spatial rules and NO
 * game code — the only C is `stock_grid_install`, which is the PLATFORM wiring
 * a library shipped with the engine, not a game answering its own questions.
 * Before #255 a hostless story could not express "next to" at all.
 *
 * The rest pins the two things a stock provider must not get wrong. It is
 * derived from ordinary state, so it must FOLLOW that state — a cached index
 * that misses a move answers yesterday's geometry and replay diverges. And it
 * offers two forms of the same question (point and generator), so they must
 * agree: enumerating changes when the host is asked, never what it answers. */

#include "stock/grid.h"
#include "state/world.h"
#include "core/intern.h"
#include "lang/story.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* ---- a hand-built world, so the geometry is checkable by inspection -------- */

enum { N = 9 };
static const int PX[N] = { 0, 1, 1, 2, 5, 5, 6, 20, 20 };
static const int PY[N] = { 0, 0, 1, 2, 5, 6, 5,  0, 40 };

static bool adj_truth(int i, int j)
{
    if (i == j) return false;
    int dx = PX[i] - PX[j], dy = PY[i] - PY[j];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= 1 && dy <= 1;
}

int main(void)
{
    intern *sy = intern_new();
    world *w = world_new(sy);
    uint32_t ent[N];
    for (int i = 0; i < N; i++) {
        char b[48];
        snprintf(b, sizeof b, "u%d", i);
        ent[i] = intern_id(sy, b);
        snprintf(b, sizeof b, "grid_x(u%d)", i);
        uint32_t xa = intern_id(sy, b);
        world_declare_num(w, xa, -1000, 1000, true);
        world_set_num(w, xa, PX[i]);
        snprintf(b, sizeof b, "grid_y(u%d)", i);
        uint32_t ya = intern_id(sy, b);
        world_declare_num(w, ya, -1000, 1000, true);
        world_set_num(w, ya, PY[i]);
        snprintf(b, sizeof b, "grid_blocks(u%d)", i);
        world_declare_fluent(w, intern_id(sy, b));
    }
    stock_grid *g = stock_grid_install(w, sy, ent, N);
    CHECK(g != NULL);
    uint32_t P_ADJ = intern_id(sy, "grid_adjacent"), P_LOS = intern_id(sy, "grid_los");

    /* measurements: a diagonal costs one on Chebyshev, two on Manhattan */
    CHECK(stock_grid_chebyshev(g, ent[0], ent[2]) == 1);
    CHECK(stock_grid_manhattan(g, ent[0], ent[2]) == 2);
    CHECK(stock_grid_chebyshev(g, ent[0], ent[7]) == 20);
    printf("  measurements: chebyshev 1 / manhattan 2 across a diagonal\n");

    /* the point form against an exhaustive check of the whole population */
    {
        int pairs = 0;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                uint32_t args[2] = { ent[i], ent[j] };
                bool got = world_provider_holds_at(w, P_ADJ, args, 2);
                if (got != adj_truth(i, j)) {
                    fprintf(stderr, "FAIL adjacency (%d,%d): got %d\n", i, j, got);
                    return 1;
                }
                pairs += got;
            }
        printf("  point form: %d ordered adjacent pairs, all correct\n", pairs);
    }

    /* the generator against the point form — the #254 differential */
    {
        bool ok = false;
        int checks = world_providers_gen_check(w, P_ADJ, ent, N, &ok);
        CHECK(ok && checks == N * N);
        printf("  generator: agrees with the point form over %d pairs\n", checks);
    }

    /* SEPARABILITY: the run must cost the answer, not the population. u8 sits
     * alone 40 cells away, so asking about it walks its own nine buckets and
     * whatever hash collisions land there — a constant, since the bucket count
     * scales with the entity count and the load factor does not. Asserting
     * zero would be asserting a perfect hash; asserting it does not grow with
     * N is the property that matters. */
    {
        uint32_t out[N];
        long before = stock_grid_probes(g);
        int n = world_provider_gen(w, P_ADJ, ent[8], out, N);
        long small = stock_grid_probes(g) - before;
        CHECK(n == 0);

        enum { CROWD = 600 };
        world *wc = world_new(sy);
        uint32_t ec[CROWD];
        for (int i = 0; i < CROWD; i++) {
            char b[48];
            snprintf(b, sizeof b, "c%d", i);
            ec[i] = intern_id(sy, b);
            snprintf(b, sizeof b, "grid_x(c%d)", i);
            uint32_t xa = intern_id(sy, b);
            world_declare_num(wc, xa, -1000, 1000, true);
            /* all but the last packed into a 4x4 block; the last alone, far off */
            world_set_num(wc, xa, i == CROWD - 1 ? 500 : i % 4);
            snprintf(b, sizeof b, "grid_y(c%d)", i);
            uint32_t ya = intern_id(sy, b);
            world_declare_num(wc, ya, -1000, 1000, true);
            world_set_num(wc, ya, i == CROWD - 1 ? 500 : (i / 4) % 4);
            snprintf(b, sizeof b, "grid_blocks(c%d)", i);
            world_declare_fluent(wc, intern_id(sy, b));
        }
        stock_grid *gc = stock_grid_install(wc, sy, ec, CROWD);
        uint32_t big_out[8];
        long b0 = stock_grid_probes(gc);
        int nc = world_provider_gen(wc, P_ADJ, ec[CROWD - 1], big_out, 8);
        long big = stock_grid_probes(gc) - b0;
        CHECK(nc == 0);
        if (big > 4 * small + 16) {
            fprintf(stderr, "FAIL separability: %ld cells at N=%d vs %ld at N=%d\n",
                    big, CROWD, small, N);
            return 1;
        }
        printf("  separability: isolated query walks %ld cells at N=%d, "
               "%ld at N=%d (population 66x)\n", small, N, big, CROWD);
        world_free(wc); stock_grid_free(gc);
    }

    /* line of sight: u4 and u6 are 1 apart on a row; drop a blocker between two
     * further apart and the segment must be refused */
    {
        uint32_t a[2] = { ent[0], ent[3] };
        CHECK(world_provider_holds_at(w, P_LOS, a, 2));      /* (0,0) -> (2,2) */
        world_set(w, intern_id(sy, "grid_blocks(u2)"), true);
        stock_grid_refresh(g);
        /* u2 at (1,1) is on the diagonal between them */
        CHECK(!world_provider_holds_at(w, P_LOS, a, 2));
        world_set(w, intern_id(sy, "grid_blocks(u2)"), false);
        stock_grid_refresh(g);
        CHECK(world_provider_holds_at(w, P_LOS, a, 2));
        printf("  line of sight: clear, blocked by an interposed entity, clear\n");
    }

    /* an entity the grid never heard of is not adjacent to anything, rather
     * than colliding with lane 0 */
    {
        uint32_t ghost = intern_id(sy, "ghost");
        uint32_t a[2] = { ent[0], ghost };
        CHECK(!world_provider_holds_at(w, P_ADJ, a, 2));
        CHECK(stock_grid_chebyshev(g, ent[0], ghost) < 0);
        printf("  unknown entity: not adjacent, distance undefined\n");
    }

    world_free(w);
    stock_grid_free(g);
    intern_free(sy);

    /* ---- THE ACCEPTANCE TEST: a spatial story with no game code ------------ */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/grid_pure.story", STORY_DIR);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)sz + 1);
        size_t rd = fread(src, 1, (size_t)sz, f); src[rd] = 0; fclose(f);

        intern *s2 = intern_new();
        story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
        world *w2 = story_compile(src, "grid_pure.story", s2, &dg);
        CHECK(w2 != NULL && dg.nerrors == 0);

        /* the platform installs the library; the GAME contributes no code */
        const char *names[] = { "scout", "sentry", "ally", "wall" };
        uint32_t e2[4];
        for (int i = 0; i < 4; i++) e2[i] = intern_id(s2, names[i]);
        stock_grid *g2 = stock_grid_install(w2, s2, e2, 4);
        CHECK(g2 != NULL);

        /* scout at (1,1), sentry at (2,1): adjacent, and the sentry is hostile */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "adjacent_to(scout,sentry)")))
              == DL_PROVED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_melee(scout)"))) == DL_PROVED);
        /* the wall at (5,1) stands between the sentry and the ally at (8,1) */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_see(sentry,ally)")))
              == DL_REFUTED);
        printf("  story: adjacency and blocked sight, with zero game code\n");

        /* MOVEMENT: an ordinary numeric effect, and the geometry must follow.
         * A cached index that misses this answers yesterday's world. */
        char err[160];
        uint32_t west = intern_id(s2, "west(scout)"), east = intern_id(s2, "east(scout)");
        CHECK(world_step(w2, &west, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w2, intern_id(s2, "grid_x(scout)")) == 0);
        /* two cells from the sentry now: the judgment must FOLLOW the move */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "adjacent_to(scout,sentry)")))
              == DL_REFUTED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_melee(scout)"))) == DL_REFUTED);
        CHECK(world_step(w2, &east, 1, err, sizeof err) == 0);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "adjacent_to(scout,sentry)")))
              == DL_PROVED);
        printf("  movement: the geometry follows state across two steps\n");

        /* the exception the library deliberately does not own: a blinded actor
         * sees nothing, however clear the line. `blind > spots` decides it. */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_see(sentry,scout)")))
              == DL_PROVED);
        world_set(w2, intern_id(s2, "blinded(sentry)"), true);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_see(sentry,scout)")))
              == DL_REFUTED);
        printf("  the story's exception overrides the measured premise\n");

        /* THE MEASUREMENT FORM (#258): the library returns a distance and the
         * STORY picks the threshold. sentry is at (2,1); after the two moves
         * above the scout is back at (1,1), and ally sits at (8,1). */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_shout(scout,sentry)")))
              == DL_PROVED);                       /* chebyshev 1 <= 3 */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_shout(scout,ally)")))
              == DL_REFUTED);                      /* chebyshev 7 > 3 */
        /* and it FOLLOWS state, like the relations do */
        for (int k = 0; k < 4; k++)
            CHECK(world_step(w2, &east, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w2, intern_id(s2, "grid_x(scout)")) == 5);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_shout(scout,ally)")))
              == DL_PROVED);                       /* chebyshev 3 <= 3 */
        printf("  measurement: the story owns the threshold, and it tracks state\n");

        world_free(w2); stock_grid_free(g2); intern_free(s2); free(src);
    }

    /* ---- the tactics slice's rule set on the same library ------------------
     *
     * grid_pure.story is a fixture; tactics.story is content — a squad rule set
     * with a band ladder, a criss-cross and ramifications, authored without
     * regard for what lanes. Running it here is what stops the stock grid from
     * being pinned only by the example written to exercise it. */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/tactics.story", STORY_DIR);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)sz + 1);
        size_t rd = fread(src, 1, (size_t)sz, f); src[rd] = 0; fclose(f);

        intern *s3 = intern_new();
        story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
        world *w3 = story_compile(src, "tactics.story", s3, &dg);
        CHECK(w3 != NULL && dg.nerrors == 0);
        static const char *NAMES[] = { "r1","r2","r3","r4","b1","b2","b3","b4" };
        uint32_t e3[8];
        for (int i = 0; i < 8; i++) e3[i] = intern_id(s3, NAMES[i]);
        stock_grid *g3 = stock_grid_install(w3, s3, e3, 8);
        CHECK(g3 != NULL);

        /* r1 (1,1) and b1 (2,1) are adjacent; r2 (1,3) and b2 (8,3) are seven
         * cells apart, past the `<= 6` the STORY chose */
        CHECK(world_query(w3, dl_pos(intern_id(s3, "engaged(r1,b1)"))) == DL_PROVED);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "sighted(r1,b1)"))) == DL_PROVED);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "engaged(r2,b2)"))) == DL_REFUTED);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "sighted(r2,b2)"))) == DL_REFUTED);

        /* the band ladder, over the same world: rooted stops a walker, and the
         * champion's authored exception outranks the condition that stops it */
        CHECK(world_query(w3, dl_pos(intern_id(s3, "advance(r2)"))) == DL_PROVED);
        world_set(w3, intern_id(s3, "rooted(r2)"), true);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "advance(r2)"))) == DL_REFUTED);
        world_set(w3, intern_id(s3, "rooted(r1)"), true);      /* r1 is champion */
        CHECK(world_query(w3, dl_pos(intern_id(s3, "advance(r1)"))) == DL_PROVED);
        /* and immunity still tops the ladder */
        world_set(w3, intern_id(s3, "stunned(r1)"), true);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "advance(r1)"))) == DL_REFUTED);

        /* movement is ordinary state, so the geometry follows it */
        char err[160];
        uint32_t west = intern_id(s3, "west(b1)");
        CHECK(world_step(w3, &west, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w3, intern_id(s3, "grid_x(b1)")) == 1);
        CHECK(world_query(w3, dl_pos(intern_id(s3, "engaged(r1,b1)"))) == DL_PROVED);
        printf("  tactics.story: grid, band ladder and champion exception\n");

        world_free(w3); stock_grid_free(g3); intern_free(s3); free(src);
    }

    /* ---- AREA OF EFFECT: the blast, over a real `cell` sort ----------------
     *
     * Three separately-landed pieces meet here for the first time: a cell
     * ENTITY as the target (an action argument cannot carry a value, and a
     * `domain point` reaches a provider as a placeholder the host resolves out
     * of band, which is no use with no host), `sort placed union actor, cell`
     * so one `grid_x` carries both (#231), and a MEASUREMENT the story
     * thresholds rather than an `in_blast` ruling compiled into C. */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/blast.story", STORY_DIR);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *src = malloc((size_t)sz + 1);
        size_t rd = fread(src, 1, (size_t)sz, f); src[rd] = 0; fclose(f);

        intern *s4 = intern_new();
        story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
        world *w4 = story_compile(src, "blast.story", s4, &dg);
        CHECK(w4 != NULL && dg.nerrors == 0);
        /* actors and cells install together — the grid reads coordinates by
         * entity and has no opinion about which member sort one belongs to */
        static const char *B[] = { "mage", "grik", "gnok", "thorn", "c22", "c55" };
        uint32_t e4[6];
        for (int i = 0; i < 6; i++) e4[i] = intern_id(s4, B[i]);
        stock_grid *g4 = stock_grid_install(w4, s4, e4, 6);
        CHECK(g4 != NULL);

        char err[200];
        uint32_t cast = intern_id(s4, "fireball(mage,c22)");
        CHECK(world_step(w4, &cast, 1, err, sizeof err) == 0);

        /* grik is ON the centre, gnok one cell out: both caught */
        CHECK(world_get_num(w4, intern_id(s4, "hp(grik)")) == 22);
        CHECK(world_get_num(w4, intern_id(s4, "hp(gnok)")) == 22);
        /* the mage at (0,0) is two cells away, thorn is nine: both spared */
        CHECK(world_get_num(w4, intern_id(s4, "hp(mage)"))  == 30);
        CHECK(world_get_num(w4, intern_id(s4, "hp(thorn)")) == 30);
        printf("  blast: the affected set is the measurement, resolved at tick time\n");

        /* the exception the LIBRARY does not own: warded thorn is spared by a
         * story condition, and would have been spared by distance anyway — so
         * move the blast onto him and check the ward is what does it */
        uint32_t cast2 = intern_id(s4, "fireball(mage,c55)");
        world_set_num(w4, intern_id(s4, "grid_x(thorn)"), 5);
        world_set_num(w4, intern_id(s4, "grid_y(thorn)"), 5);
        stock_grid_refresh(g4);
        CHECK(world_step(w4, &cast2, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w4, intern_id(s4, "hp(thorn)")) == 30);
        CHECK(world_query(w4, dl_pos(intern_id(s4, "burning(thorn)"))) == DL_REFUTED);
        printf("  ...and a warded target inside it is spared by the STORY\n");

        /* burning is a conclusion with a rule behind it, which is what shipping
         * the distance rather than the ruling buys: `why?` has something to say */
        CHECK(world_query(w4, dl_pos(intern_id(s4, "burning(grik)"))) == DL_PROVED);
        CHECK(world_query(w4, dl_pos(intern_id(s4, "singed(grik)"))) == DL_PROVED);
        printf("  ...and the story drew its own conclusion from the hit\n");

        world_free(w4); stock_grid_free(g4); intern_free(s4); free(src);
    }

    printf("test_stockgrid: all passed\n");
    return 0;
}
