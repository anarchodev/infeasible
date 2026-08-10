/* Golden test for provider legibility in why-traces (#178) — the last opaque
 * leaf.
 *
 * §5.6 makes providers load-bearing on purpose: dense computed relations are
 * host territory and the logic layer joins only sparse results. The more
 * faithfully a game follows that advice, the more of its interesting reasoning
 * ends up behind a callback that answers yes/no and explains nothing. In an
 * engine whose product is the trace, that is a black box in the middle of it.
 *
 * Two halves, one per provider kind:
 *
 *   RELATIONS get an optional render callback. `near(guard1,intruder1) [PROVED]`
 *   becomes `near(guard1,intruder1) [manhattan 2 <= 2] [PROVED]` — the host's
 *   own account of an answer it already computed. Consulted only while
 *   rendering, never during a solve, so it cannot perturb a verdict and sits
 *   outside I4. With no callback registered the trace is byte-identical to
 *   what it always was, which is what makes this additive.
 *
 *   VALUE FUNCTIONS get an opt-in call log. A receipt can say `at(guard1) := 1001`
 *   but not that `step(1000, east)` answered 1001; the log records the calls a
 *   step made, in order. Off by default — the provider boundary is the hot path
 *   — and a pure side-channel, so a run with it on and one with it off are the
 *   same run. */

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

/* --- the host's grid, exactly as test_spatial's: a cell handle encodes (x,y),
 * and only these three functions ever decode it. ------------------------- */
#define GBASE 1000
#define GW 5
static int cx(long h) { return (int)((h - GBASE) % GW); }
static int cy(long h) { return (int)((h - GBASE) / GW); }
static long enc(int x, int y) { return GBASE + (long)y * GW + x; }
static int iabs(int v) { return v < 0 ? -v : v; }

static intern *SY;
static world  *W;
static uint32_t STEP, NEAR;

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

static long pos_of(uint32_t ent)
{
    char b[64];
    snprintf(b, sizeof b, "at(%s)", intern_name(SY, ent));
    return world_get_num(W, intern_id(SY, b));
}

static int dist(uint32_t a, uint32_t b)
{
    long ha = pos_of(a), hb = pos_of(b);
    return iabs(cx(ha) - cx(hb)) + iabs(cy(ha) - cy(hb));
}

static bool near_fn(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)ctx;
    return pred == NEAR && nargs == 2 && dist(args[0], args[1]) <= 2;
}

/* The render half: the host explains the answer it just gave. Note it phrases
 * BOTH verdicts — an unexplained "no" is the more frustrating one. */
static int near_render(void *ctx, uint32_t pred, const uint32_t *args, int nargs,
                       bool holds, char *buf, size_t cap)
{
    (void)ctx;
    if (pred != NEAR || nargs != 2) return 0;
    return snprintf(buf, cap, "manhattan %d %s 2", dist(args[0], args[1]),
                    holds ? "<=" : ">");
}

static char *why_str(dl_lit q)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(W, q, m);
    fclose(m);
    return buf;
}

static const char *SRC =
    "sort actor\n"
    "domain cell\n"
    "entity ( guard1, intruder1 : actor )\n"
    "state ( at(actor) : cell   guard(actor)   intruder(actor) )\n"
    "provider near(actor, actor)\n"
    "function step(cell, int) : cell\n"
    /* the state atoms anchor both variables — a bare provider generates
     * nothing (§5.2 range restriction), which the safety check enforces */
    "rule spot(G: actor, I: actor):\n"
    "  guard(G) & intruder(I) & near(G, I) => spotted(I)\n"
    /* the direction is an int literal, as patrol.story spells it: the host
     * owns what 0 and 1 mean */
    "action east(A: actor): causes at(A) := step(at(A), 0)\n"
    "action west(A: actor): causes at(A) := step(at(A), 1)\n"
    "exclusive east(A), west(A)\n"
    "init ( guard(guard1)  intruder(intruder1) )\n";

int main(void)
{
    SY = intern_new();
    story_diag di[8];
    story_diags dg = { di, 8, 0, 0 };
    W = story_compile(SRC, "t.story", SY, &dg);
    if (!W) { fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?"); return 1; }

    STEP = intern_id(SY, "step");
    NEAR = intern_id(SY, "near");
    world_set_fn_provider_fn(W, step_fn, NULL);
    world_set_provider_fn(W, near_fn, NULL);
    /* the host mints positions: guard at (0,0), intruder at (4,0) — 4 apart */
    world_set_num(W, intern_id(SY, "at(guard1)"),    enc(0, 0));
    world_set_num(W, intern_id(SY, "at(intruder1)"), enc(4, 0));

    uint32_t spotted = intern_id(SY, "spotted(intruder1)");
    CHECK(world_query(W, dl_pos(spotted)) != DL_PROVED);

    /* --- with no render callback the trace is what it always was --------- */
    {
        char *t = why_str(dl_pos(spotted));
        CHECK(strstr(t, "near(guard1,intruder1)") != NULL);
        CHECK(strstr(t, "manhattan") == NULL);
        free(t);
    }

    /* --- registering one explains the NO ---------------------------------- */
    world_set_provider_render_fn(W, near_render, NULL);
    {
        char *t = why_str(dl_pos(spotted));
        CHECK(strstr(t, "near(guard1,intruder1) [manhattan 4 > 2]") != NULL);
        free(t);
    }

    /* --- and, after the guard closes, the YES ------------------------------ */
    char err[128];
    uint32_t east = intern_id(SY, "east(guard1)");
    for (int i = 0; i < 2; i++)
        CHECK(world_step(W, &east, 1, err, sizeof err) == 0);
    CHECK(pos_of(intern_id(SY, "guard1")) == enc(2, 0));
    CHECK(world_query(W, dl_pos(spotted)) == DL_PROVED);
    {
        char *t = why_str(dl_pos(spotted));
        CHECK(strstr(t, "near(guard1,intruder1) [manhattan 2 <= 2]") != NULL);
        CHECK(strstr(t, "-- applicable") != NULL);
        free(t);
    }

    /* --- the value-function half: off by default --------------------------- */
    int n;
    CHECK(world_fn_calls(W, &n) == NULL && n == 0);

    world_set_fn_call_log(W, true);
    CHECK(world_step(W, &east, 1, err, sizeof err) == 0);
    const world_fn_call *calls = world_fn_calls(W, &n);
    CHECK(n >= 1);
    /* the move's geometry call, with the arguments the VM passed and the
     * answer the host gave: step((2,0), east) = (3,0) */
    bool found = false;
    for (int i = 0; i < n; i++)
        if (calls[i].pred == STEP && calls[i].nargs == 2 &&
            calls[i].args[0] == enc(2, 0) && calls[i].result == enc(3, 0))
            found = true;
    CHECK(found);
    CHECK(pos_of(intern_id(SY, "guard1")) == enc(3, 0));

    /* the log is the LAST step's, like the changeset and the emissions */
    uint32_t west = intern_id(SY, "west(guard1)");
    CHECK(world_step(W, &west, 1, err, sizeof err) == 0);
    calls = world_fn_calls(W, &n);
    for (int i = 0; i < n; i++)
        CHECK(!(calls[i].pred == STEP && calls[i].args[0] == enc(2, 0) &&
                calls[i].result == enc(3, 0)));      /* the previous step's call */

    /* turning it off costs the log, not the answer: the same move again lands
     * in the same place (recording is a side-channel, never a semantic one) */
    long before = pos_of(intern_id(SY, "guard1"));
    world_set_fn_call_log(W, false);
    CHECK(world_fn_calls(W, &n) == NULL && n == 0);
    CHECK(world_step(W, &east, 1, err, sizeof err) == 0);
    CHECK(pos_of(intern_id(SY, "guard1")) == before + 1);

    world_free(W);
    intern_free(SY);
    printf("test_provtrace: all passed\n");
    return 0;
}
