/* Golden test for the stock HEX topology (§5.6, #253's hex slice).
 *
 * §5.6's claim is that hex vs. square is just the neighbour function inside
 * the provider. This file is where that claim is either true or a slogan, so
 * the load-bearing case is the one at the bottom of the geometry section: two
 * entities one step apart on BOTH axes are adjacent on a square and two hexes
 * apart on a hex. If the install were the square one under another name,
 * everything else here would still pass and that would not.
 *
 * The rest pins what the topology owes a story: a distance checked against an
 * independently written cube-coordinate truth (not against itself), the point
 * and generator forms agreeing, sight blocked by what stands on the line, and
 * an occlusion measurement that reports the outline a blocker hides rather
 * than a yes/no ruling. The acceptance test is the last one — a hex story with
 * no game code in it anywhere. */

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

/* A hand-laid field in axial coordinates. u4 sits at (1,1): one step from u0
 * on each axis, which is ADJACENT on a square grid and two hexes away here. */
enum { N = 7 };
static const int PQ[N] = { 0, 1, 2, 4, 1, 0,  3 };
static const int PR[N] = { 0, 0, 0, 0, 1, 3, -1 };

/* The truth, written from the cube-coordinate definition rather than from the
 * library's own arithmetic: |x| + |y| + |z| over 2, with y = -(q + r). */
static int hex_truth(int i, int j)
{
    int dq = PQ[i] - PQ[j], dr = PR[i] - PR[j], dy = -(dq + dr);
    int ax = dq < 0 ? -dq : dq, az = dr < 0 ? -dr : dr, ay = dy < 0 ? -dy : dy;
    return (ax + ay + az) / 2;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = malloc((size_t)sz + 1);
    size_t rd = fread(src, 1, (size_t)sz, f); src[rd] = 0;
    fclose(f);
    return src;
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
        snprintf(b, sizeof b, "hex_q(u%d)", i);
        uint32_t qa = intern_id(sy, b);
        world_declare_num(w, qa, -1000, 1000, true);
        world_set_num(w, qa, PQ[i]);
        snprintf(b, sizeof b, "hex_r(u%d)", i);
        uint32_t ra = intern_id(sy, b);
        world_declare_num(w, ra, -1000, 1000, true);
        world_set_num(w, ra, PR[i]);
        snprintf(b, sizeof b, "hex_blocks(u%d)", i);
        world_declare_fluent(w, intern_id(sy, b));
    }
    stock_grid *g = stock_hex_install(w, sy, ent, N);
    CHECK(g != NULL);
    uint32_t P_ADJ = intern_id(sy, "hex_adjacent"), P_LOS = intern_id(sy, "hex_los");

    /* the distance, against the independent truth over every ordered pair */
    {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                int got = stock_grid_distance(g, ent[i], ent[j]);
                if (got != hex_truth(i, j)) {
                    fprintf(stderr, "FAIL distance (%d,%d): got %d want %d\n",
                            i, j, got, hex_truth(i, j));
                    return 1;
                }
            }
        printf("  distance: cube metric over %d pairs\n", N * N);
    }

    /* THE TOPOLOGY CASE: u4 is at (1,1), one step from u0 on each axis. A
     * square grid calls that a diagonal and adjacent; a hex has no diagonal,
     * and the distance function is the whole of the difference. */
    {
        uint32_t a[2] = { ent[0], ent[4] };
        CHECK(stock_grid_distance(g, ent[0], ent[4]) == 2);
        CHECK(!world_provider_holds_at(w, P_ADJ, a, 2));
        uint32_t b[2] = { ent[0], ent[1] };
        CHECK(stock_grid_distance(g, ent[0], ent[1]) == 1);
        CHECK(world_provider_holds_at(w, P_ADJ, b, 2));
        printf("  six neighbours: (1,1) is two hexes away, not a diagonal\n");
    }

    /* the point form against the truth over the whole population */
    {
        int pairs = 0;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                uint32_t args[2] = { ent[i], ent[j] };
                bool got = world_provider_holds_at(w, P_ADJ, args, 2);
                bool want = i != j && hex_truth(i, j) <= 1;
                if (got != want) {
                    fprintf(stderr, "FAIL adjacency (%d,%d): got %d\n", i, j, got);
                    return 1;
                }
                pairs += got;
            }
        printf("  point form: %d ordered adjacent pairs, all correct\n", pairs);
    }

    /* the generator against the point form — the #254 differential, over a
     * topology whose 3x3 bucket box is a strict SUPERSET of its neighbours */
    {
        bool ok = false;
        int checks = world_providers_gen_check(w, P_ADJ, ent, N, &ok);
        CHECK(ok && checks == N * N);
        printf("  generator: agrees with the point form over %d pairs\n", checks);
    }

    /* line of sight: u2 at (2,0) stands on the line from u0 to u3 at (4,0) */
    {
        uint32_t a[2] = { ent[0], ent[3] };
        CHECK(world_provider_holds_at(w, P_LOS, a, 2));
        world_set(w, intern_id(sy, "hex_blocks(u2)"), true);
        stock_grid_refresh(g);
        CHECK(!world_provider_holds_at(w, P_LOS, a, 2));

        /* u6 at (3,-1) is off that line — the hex line from (0,0) bends around
         * the blocker, so sight is CLEAR while part of the outline is not */
        uint32_t b[2] = { ent[0], ent[6] };
        CHECK(world_provider_holds_at(w, P_LOS, b, 2));
        printf("  line of sight: blocked on the line, clear one hex off it\n");
    }

    /* occlusion: the measurement the cover ruling is written over. u3 is
     * directly behind the blocker; u6 is beside it; u5 is in the open. */
    {
        CHECK(stock_grid_occlusion(g, ent[0], ent[3]) == 100);
        int part = stock_grid_occlusion(g, ent[0], ent[6]);
        CHECK(part > 0 && part < 100);
        CHECK(stock_grid_occlusion(g, ent[0], ent[5]) == 0);
        /* six corners, so the resolution is sixths — and a story thresholding
         * at 50 must not see a 33 as cover */
        CHECK(part < 50);
        printf("  occlusion: 100%% behind the blocker, %d%% beside it, 0%% in "
               "the open\n", part);

        world_set(w, intern_id(sy, "hex_blocks(u2)"), false);
        stock_grid_refresh(g);
        CHECK(stock_grid_occlusion(g, ent[0], ent[3]) == 0);
        printf("  occlusion: follows state — clear the blocker and it is 0%%\n");
    }

    /* an entity the grid never heard of: infinitely far, and fully hidden
     * rather than fully exposed. A guard reads `>= n`, so a 0 would make the
     * unplaced entity the one everybody can see. */
    {
        uint32_t ghost = intern_id(sy, "ghost");
        uint32_t a[2] = { ent[0], ghost };
        CHECK(!world_provider_holds_at(w, P_ADJ, a, 2));
        CHECK(stock_grid_distance(g, ent[0], ghost) < 0);
        CHECK(stock_grid_occlusion(g, ent[0], ghost) == 100);
        CHECK(stock_grid_occlusion(g, ent[0], ent[0]) == 0);
        printf("  unknown entity: distance undefined, fully occluded\n");
    }

    /* A world that declares no hex positions at all: every read would answer 0
     * and stand the cast on one hex, where everyone is adjacent to everyone.
     * Refusing to install is the only honest answer (the story-side half of
     * this is a located compile error, #263). */
    {
        world *wb = world_new(sy);
        uint32_t e[2] = { intern_id(sy, "v0"), intern_id(sy, "v1") };
        CHECK(stock_hex_install(wb, sy, e, 2) == NULL);
        /* and the square install refuses the hex vocabulary for the same
         * reason: it reads grid_x/grid_y, which this world does not have */
        CHECK(stock_grid_install(w, sy, ent, N) == NULL);
        world_free(wb);
        printf("  no positions declared: the install refuses rather than "
               "answering from zeroes\n");
    }

    world_free(w);
    stock_grid_free(g);
    intern_free(sy);

    /* ---- THE ACCEPTANCE TEST: a hex story with no game code ---------------- */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/hexfield.story", STORY_DIR);
        char *src = slurp(path);
        CHECK(src != NULL);

        intern *s2 = intern_new();
        story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
        world *w2 = story_compile(src, "hexfield.story", s2, &dg);
        CHECK(w2 != NULL && dg.nerrors == 0);

        /* the platform installs the library; the GAME contributes no code */
        const char *names[] = { "ranger", "wolf", "boulder", "elk", "crow", "stag" };
        uint32_t e2[6];
        for (int i = 0; i < 6; i++) e2[i] = intern_id(s2, names[i]);
        stock_grid *g2 = stock_hex_install(w2, s2, e2, 6);
        CHECK(g2 != NULL);

        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_reach(ranger,wolf)")))
              == DL_PROVED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_reach(ranger,elk)")))
              == DL_REFUTED);
        /* the boulder at (2,0) stands between the ranger and the elk */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_see(ranger,elk)")))
              == DL_REFUTED);
        /* three hexes is a shout: the crow at (0,3) is in, the elk is not */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_shout(ranger,crow)")))
              == DL_PROVED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_shout(ranger,elk)")))
              == DL_REFUTED);
        printf("  story: reach, blocked sight and a shout radius, zero game code\n");

        /* THE MEASUREMENT AND THE RULING ARE DIFFERENT QUESTIONS. The stag's
         * centre line is clear — it is stalkable — while part of its outline
         * sits behind the boulder. A `has_cover` boolean would have had to
         * choose one of those answers on the story's behalf. */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_see(ranger,stag)")))
              == DL_PROVED);
        /* REFUTED, not UNDECIDED: every rule for `concealed` is inapplicable
         * here, which is exactly -∂ (§5.1). The guard did not fail to answer;
         * it answered, and 33% is under the band the story wrote. */
        CHECK(world_query(w2, dl_pos(intern_id(s2, "concealed(ranger,stag)")))
              == DL_REFUTED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_stalk(ranger,stag)")))
              == DL_PROVED);
        printf("  the story's band decides: 33%% hidden is not concealment\n");

        /* MOVEMENT: an ordinary numeric effect, and the geometry must follow.
         * Step the ranger one hex east and the wolf is no longer in reach. */
        char err[160];
        uint32_t away = intern_id(s2, "west(ranger)");
        CHECK(world_step(w2, &away, 1, err, sizeof err) == 0);
        CHECK(world_get_num(w2, intern_id(s2, "hex_q(ranger)")) == -1);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_reach(ranger,wolf)")))
              == DL_REFUTED);
        /* and back */
        uint32_t back = intern_id(s2, "east(ranger)");
        CHECK(world_step(w2, &back, 1, err, sizeof err) == 0);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "in_reach(ranger,wolf)")))
              == DL_PROVED);
        printf("  movement: the hex geometry follows state across two steps\n");

        /* Moving BEHIND the boulder is what makes concealment a live band: the
         * stag steps one hex east, from 33% hidden to 66%, and crosses the
         * story's threshold with no rule recomputing anything by hand. */
        uint32_t hide = intern_id(s2, "east(stag)");
        CHECK(world_step(w2, &hide, 1, err, sizeof err) == 0);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "concealed(ranger,stag)")))
              == DL_PROVED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "can_stalk(ranger,stag)")))
              == DL_REFUTED);
        printf("  a step into the boulder's shadow flips the ruling\n");

        stock_grid_free(g2);
        world_free(w2);
        intern_free(s2);
        free(src);
    }

    printf("test_hexgrid: all passed\n");
    return 0;
}
