/* Worked spatial scenario (DESIGN §5.6) driving examples/patrol.story end-to-end.
 *
 * The point: SPACE NEEDS NO NEW PRIMITIVE. Positions are store-backed cell
 * fluents; movement is a causal rule calling a host geometry FUNCTION provider
 * (`step`); the spatial relation `near` is a boolean PROVIDER the host answers
 * from an index over at(·). The engine owns no geometry — the host does, behind
 * the provider seam. This pins DESIGN §5.6's golden test: a move changes exactly
 * one actor's position and leaves others inert; a proximity judgment fires iff
 * the provider reports in-range and RECOMPUTES when a move changes the range
 * (providers are consulted fresh each solve — the I3 "recompute" for free). */

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

/* --- the host's grid index (lives entirely here, never in the engine) --------
 * A 5x5 grid; a cell handle encodes (x, y) as BASE + y*W + x. The .story never
 * decodes this — only step() and near() do. Swapping to hex = editing these. */
#define GBASE 1000
#define GW 5
static int cx(long h) { return (int)((h - GBASE) % GW); }
static int cy(long h) { return (int)((h - GBASE) / GW); }
static long enc(int x, int y) { return GBASE + (long)y * GW + x; }
static int iabs(int v) { return v < 0 ? -v : v; }

static intern *SY;
static world  *W;
static uint32_t STEP, NEAR;

/* value-returning function provider: the neighbour cell one step in `dir`,
 * clamped at the walls. Deterministic, seedless (I4). */
static long step_fn(void *ctx, uint32_t pred, const long *a, int n)
{
    (void)ctx;
    if (pred == STEP && n == 2) {
        int x = cx(a[0]), y = cy(a[0]);
        switch (a[1]) {
        case 0: if (x < GW - 1) x++; break;   /* east  */
        case 1: if (x > 0)      x--; break;   /* west  */
        case 2: if (y < GW - 1) y++; break;   /* south */
        case 3: if (y > 0)      y--; break;   /* north */
        }
        return enc(x, y);
    }
    return 0;
}

/* boolean provider: the two actors are within Manhattan distance 2, read from the
 * engine's stored positions (the logic owns positions; the host queries them). */
static bool near_fn(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)ctx;
    if (pred == NEAR && nargs == 2) {
        char ba[32], bb[32];
        snprintf(ba, sizeof ba, "at(%s)", intern_name(SY, args[0]));
        snprintf(bb, sizeof bb, "at(%s)", intern_name(SY, args[1]));
        long ha = world_get_num(W, intern_id(SY, ba));
        long hb = world_get_num(W, intern_id(SY, bb));
        return iabs(cx(ha) - cx(hb)) + iabs(cy(ha) - cy(hb)) <= 2;
    }
    return false;
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

static long at(const char *e) { char b[32]; snprintf(b, sizeof b, "at(%s)", e);
                                return world_get_num(W, intern_id(SY, b)); }
static int spotted(void)
{ return world_query(W, dl_pos(intern_id(SY, "spotted(intruder1)"))) == DL_PROVED; }

int main(void)
{
    char *src = slurp(STORY_DIR "/patrol.story");
    if (!src) { fprintf(stderr, "cannot read patrol.story\n"); return 1; }

    SY = intern_new();
    story_diag di[8]; story_diags dg = { di, 8, 0, 0 };
    W = story_compile(src, "patrol.story", SY, &dg);
    free(src);
    if (!W) { fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?"); return 1; }
    CHECK(dg.nerrors == 0);
    /* anchored rule: no cardinality warn; the ONE expected diagnostic is
     * #98's true positive — east and west both `:=` at(G) and a host stepping
     * both in one tick is the contested `-1` (pinned below as it happens) */
    for (int ci = 0; ci < dg.count && ci < dg.cap; ci++)
        CHECK(strstr(di[ci].msg, "both assign (`:=`) 'at'") != NULL);

    STEP = intern_id(SY, "step");
    NEAR = intern_id(SY, "near");
    world_set_fn_provider_fn(W, step_fn, NULL);
    world_set_provider_fn(W, near_fn, NULL);

    /* host mints starting positions: guard at (0,0), intruder at (4,0) */
    world_set_num(W, intern_id(SY, "at(guard1)"),    enc(0, 0));
    world_set_num(W, intern_id(SY, "at(intruder1)"), enc(4, 0));
    CHECK(!spotted());                        /* Manhattan 4 > 2 */

    char err[128];
    uint32_t east = intern_id(SY, "east(guard1)");
    uint32_t west = intern_id(SY, "west(guard1)");

    /* step east: only the guard moves; the intruder is inert (Yale-shooting) */
    CHECK(world_step(W, &east, 1, err, sizeof err) == 0);
    CHECK(at("guard1")    == enc(1, 0));
    CHECK(at("intruder1") == enc(4, 0));      /* unchanged */
    CHECK(!spotted());                        /* dist 3, still out of range */

    /* step east again: now within range — the provider recomputes, judgment fires */
    CHECK(world_step(W, &east, 1, err, sizeof err) == 0);
    CHECK(at("guard1")    == enc(2, 0));
    CHECK(at("intruder1") == enc(4, 0));      /* still inert across both steps */
    CHECK(spotted());                         /* dist 2 <= 2 */

    /* step west: range opens back up — the judgment retracts (fresh recompute) */
    CHECK(world_step(W, &west, 1, err, sizeof err) == 0);
    CHECK(at("guard1") == enc(1, 0));
    CHECK(!spotted());                        /* dist 3 again */

    world_free(W);
    intern_free(SY);
    printf("test_spatial: all passed\n");
    return 0;
}
