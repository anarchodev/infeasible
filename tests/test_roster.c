/* Golden test for the provider roster refusal (#263, §5.6/§12).
 *
 * A provider nobody answers reads closed-world FALSE — forever, and silently.
 * The rule never fires, and the failure is indistinguishable from a world
 * where the relation simply does not hold. That was tolerable while a game
 * could ship a host to answer it; #253 retired the per-game provider, so an
 * unrecognised name now means the author named something that does not exist,
 * and the only acceptable outcome is a located error.
 *
 * The negative cases are therefore the point of this file: what must FAIL, and
 * whether the message tells an author what to do instead. A check that only
 * proves the good cases still compile would not have caught the thing that
 * motivated it. */

#include "lang/story.h"
#include "core/intern.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static const char PRELUDE[] =
    "sort actor\nentity ( a, b : actor )\n"
    "state ( grid_x(actor) : int in 0..9  grid_y(actor) : int in 0..9\n"
    "        grid_blocks(actor)  awake(actor) )\n";

static int compile_expect(const char *src, const char *needle, const char *what);

static int expect(const char *tail, const char *needle, const char *what)
{
    char src[2048];
    snprintf(src, sizeof src, "%s%s", PRELUDE, tail);
    return compile_expect(src, needle, what);
}

/* the same check over a WHOLE source: a case about how the position state is
 * declared cannot inherit the prelude's declaration of it */
static int compile_expect(const char *src, const char *needle, const char *what)
{
    intern *sy = intern_new();
    story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "r.story", sy, &dg);
    int bad = 0;
    if (needle == NULL) {                       /* must COMPILE */
        if (!w || dg.nerrors) {
            fprintf(stderr, "FAIL %s: %s\n", what, dg.count ? di[0].msg : "(no message)");
            bad = 1;
        }
    } else if (w && !dg.nerrors) {
        fprintf(stderr, "FAIL %s: compiled, should not have\n", what);
        bad = 1;
    } else {
        bool found = false;
        for (int i = 0; i < dg.count; i++)
            if (strstr(di[i].msg, needle)) found = true;
        if (!found) {
            fprintf(stderr, "FAIL %s: no diagnostic mentioning '%s' (first: %s)\n",
                    what, needle, dg.count ? di[0].msg : "none");
            bad = 1;
        }
    }
    if (w) world_free(w);
    intern_free(sy);
    return bad;
}

int main(void)
{
    int bad = 0;

    /* what the platform stocks, compiling */
    bad |= expect("provider grid_adjacent(actor, actor)\n"
                  "rule r(X: actor, Y: actor): grid_adjacent(X, Y) => near(X, Y)\n",
                  NULL, "a stock relation");
    bad |= expect("function grid_chebyshev(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): grid_chebyshev(X, Y) <= 2 => near(X, Y)\n",
                  NULL, "a stock measurement");

    /* THE case: a name nobody answers */
    bad |= expect("provider can_see(actor, actor)\n"
                  "rule r(X: actor, Y: actor): can_see(X, Y) => spots(X, Y)\n",
                  "no stock provider 'can_see'", "an invented relation");
    bad |= expect("function distance_to(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): distance_to(X, Y) <= 1 => near(X, Y)\n",
                  "no stock function 'distance_to'", "an invented measurement");

    /* a stock name at the WRONG arity is not the stock one */
    bad |= expect("provider grid_adjacent(actor)\n",
                  "no stock provider 'grid_adjacent' takes 1", "right name, wrong arity");

    /* a stock MEASUREMENT declared as a relation: the diagnostic must say so
     * rather than claim the name is unknown, because the fix is different —
     * declare the function and write the threshold (§5.6) */
    bad |= expect("provider grid_chebyshev(actor, actor)\n",
                  "is a stock MEASUREMENT, not a relation", "measurement as relation");

    /* an EMBEDDER may still answer one — the claim made out loud */
    bad |= expect("host provider can_see(actor, actor)\n"
                  "rule r(X: actor, Y: actor): can_see(X, Y) => spots(X, Y)\n",
                  NULL, "host provider");
    bad |= expect("host function distance_to(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): distance_to(X, Y) <= 1 => near(X, Y)\n",
                  NULL, "host function");

    /* `host` marks a provider, not anything else */
    bad |= expect("host state ( lit(actor) )\n",
                  "expected one of those", "host on the wrong declaration");

    /* ---- the state a stock provider reads ---------------------------------
     *
     * The roster grew a second topology, and with it the way to be wrong that
     * an arity check cannot see: `hex_adjacent` is a real name at a real
     * arity, answered from `hex_q`/`hex_r`. Over a story that keeps square
     * positions there is nothing to read, an undeclared numeric fluent reads
     * 0, and every entity stands on one hex — where everyone is adjacent to
     * everyone and every line of sight is clear. That is #263's failure with
     * the sign flipped: not silently false, silently TRUE. */
    bad |= expect("provider hex_adjacent(actor, actor)\n"
                  "rule r(X: actor, Y: actor): hex_adjacent(X, Y) => near(X, Y)\n",
                  "nothing here declares `hex_q", "hex relation over square positions");
    bad |= expect("function hex_distance(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): hex_distance(X, Y) <= 2 => near(X, Y)\n",
                  "nothing here declares `hex_q", "hex measurement, no hex state");

    /* and the hex vocabulary, declared properly, compiles */
    bad |= expect("state ( hex_q(actor) : int in 0..9  hex_r(actor) : int in 0..9 )\n"
                  "provider hex_adjacent(actor, actor)\n"
                  "function hex_occlusion(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): hex_adjacent(X, Y) => near(X, Y)\n"
                  "rule c(X: actor, Y: actor): near(X, Y) & hex_occlusion(X, Y) >= 50"
                  " => hidden(X, Y)\n",
                  NULL, "the hex vocabulary");

    /* the positions must cover the SORT the provider measures, not merely
     * exist: `grid_x(actor)` says nothing about where a beast is standing */
    bad |= expect("sort beast\nentity ( wolf, bear : beast )\n"
                  "provider grid_adjacent(beast, beast)\n"
                  "rule r(X: beast, Y: beast): grid_adjacent(X, Y) => near(X, Y)\n",
                  "has no position stored anywhere", "positions on another sort");

    /* a cover admits its members, so positions declared over one serve every
     * sort under it (#231) — and this one needs its own source, because the
     * question is where `grid_x` is declared */
    bad |= compile_expect(
        "sort actor, beast\nentity ( a : actor )\nentity ( wolf : beast )\n"
        "sort placed union actor, beast\n"
        "state ( grid_x(placed) : int in 0..9  grid_y(placed) : int in 0..9\n"
        "        awake(actor) )\n"
        "provider grid_adjacent(beast, beast)\n"
        "rule r(X: beast, Y: beast): grid_adjacent(X, Y) => near(X, Y)\n",
        NULL, "positions over a cover");

    /* the new members of the roster answer for themselves */
    bad |= expect("provider grid_occlusion(actor, actor)\n",
                  "is a stock MEASUREMENT, not a relation", "occlusion as a relation");
    bad |= expect("function grid_occlusion(actor, actor) : int\n"
                  "rule r(X: actor, Y: actor): grid_occlusion(X, Y) >= 50"
                  " => hidden(X, Y)\n",
                  NULL, "the occlusion measurement");

    /* the message lists what the platform supplies, and the list is built from
     * the roster — a hand-written one falls behind the library it describes */
    bad |= expect("provider can_see(actor, actor)\n",
                  "hex_adjacent/2", "the refusal names the whole roster");

    CHECK(bad == 0);
    printf("test_roster: all passed\n");
    return 0;
}
