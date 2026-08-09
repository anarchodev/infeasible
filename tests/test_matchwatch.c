/* Pred-scoped matched-layer invalidation (#45): a base-fact edit re-grounds the
 * matched layer only when it touches a predicate some matchable rule READS.
 *
 * The optimization is only safe if the watch set is complete, and the failure
 * mode is silent: a missed re-ground leaves the matched layer describing a
 * world that no longer exists, and the query returns a stale verdict rather
 * than an error. So this pins it the only way that is honest — differentially,
 * against an eagerly-ground world of the same story, across a SEQUENCE of
 * world_set edits that grow the match set, shrink it again, and touch a
 * predicate no matchable rule mentions.
 *
 * Growth and retraction are both covered deliberately. A missed re-ground on
 * growth loses a conclusion (visible as UNDECIDED); a missed re-ground on
 * retraction KEEPS one that should be gone, which is the stale-fact bug I1
 * exists to prevent, wearing a cache costume. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } \
    } while (0)

static const char *STORY =
    "scene w\n"
    "sort actor\n"
    "entity ( a, b : actor )\n"
    "state (\n"
    "  adj(actor, actor)\n"
    "  awake(actor)\n"
    "  marked(actor)\n"
    "  weather(actor)\n"        /* read by NO matchable rule: the unwatched case */
    ")\n"
    "init ( adj(a, b) adj(b, a) )\n"
    "rule threat(X: actor, Y: actor): adj(X, Y) & awake(X) => threat(X, Y)\n"
    "rule ready(X: actor): awake(X) & ~marked(X) => ready(X)\n"
    /* an action, so lanes are skipped and queries reach the judgment family
     * where the matched layer actually lives */
    "action wake(X: actor): causes awake(X)\n";

/* Every conclusion either world can reach, checked after each edit.
 *
 * Compared on PROVABILITY, not the raw verdict — the same contract test_matcher
 * pins, and for the same reason: eager grounds an always-failing rule for an
 * unsatisfiable instance, which lets DL refute it, while the matcher grounds no
 * such rule and the atom stays UNDECIDED. Nothing that is PROVED ever moves,
 * and that is the guarantee a host consumes. */
static int agree(world *A, world *B, intern *sy, const char *stage)
{
    static const char *atoms[] = {
        "threat(a,b)", "threat(b,a)", "threat(a,a)", "threat(b,b)",
        "ready(a)", "ready(b)",
        "awake(a)", "awake(b)", "marked(a)", "weather(a)", "adj(a,b)",
    };
    for (int i = 0; i < (int)(sizeof atoms / sizeof atoms[0]); i++) {
        for (int neg = 0; neg < 2; neg++) {
            dl_lit q = neg ? dl_neg(intern_id(sy, atoms[i]))
                           : dl_pos(intern_id(sy, atoms[i]));
            bool pa = world_query(A, q) == DL_PROVED;
            bool pb = world_query(B, q) == DL_PROVED;
            if (pa != pb) {
                fprintf(stderr, "FAIL [%s] %s%s: eager=%d matched=%d\n",
                        stage, neg ? "~" : "", atoms[i], pa, pb);
                return 1;
            }
        }
    }
    return 0;
}

int main(void)
{
    intern *sy = intern_new();
    story_diag da[16]; story_diags dga = { da, 16, 0, 0 };
    story_diag db[16]; story_diags dgb = { db, 16, 0, 0 };

    /* eager first so the shared intern holds the full atom superset.
     *
     * story_compile_MATCHER, not story_compile_matched: only the former installs
     * the auto re-ground hook (#45), and re-grounding is precisely what this
     * test is about. A `matched` world grounds once at compile and never
     * refreshes, so a world_set against it is stale by construction. */
    world *A = story_compile(STORY, "w.story", sy, &dga);
    world *B = NULL;
    story_matcher *M = story_compile_matcher(STORY, "w.story", sy, &dgb, &B);
    CHECK(A && B && M && dga.nerrors == 0 && dgb.nerrors == 0);

    uint32_t awake_a = intern_id(sy, "awake(a)");
    uint32_t awake_b = intern_id(sy, "awake(b)");
    uint32_t marked_a = intern_id(sy, "marked(a)");
    uint32_t weather_a = intern_id(sy, "weather(a)");

    CHECK(agree(A, B, sy, "initial") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) != DL_PROVED);

    /* 1. WATCHED pred, growth: awake(a) is read by both matchable rules, so the
     *    matched layer must re-ground and threat(a,b) must appear. */
    world_set(A, awake_a, true);
    world_set(B, awake_a, true);
    CHECK(agree(A, B, sy, "awake(a)=1") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "ready(a)"))) == DL_PROVED);

    /* 2. UNWATCHED pred: no matchable rule reads weather, so no re-ground is
     *    owed — and every verdict, including the edited fact itself, must be
     *    unchanged from what the eager world says. */
    world_set(A, weather_a, true);
    world_set(B, weather_a, true);
    CHECK(agree(A, B, sy, "weather(a)=1") == 0);
    CHECK(world_query(B, dl_pos(weather_a)) == DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);

    /* 3. WATCHED pred appearing only under a NEGATED body atom: marked(a)
     *    prunes ready(a). A watch set that only collected positive generators
     *    would miss this one and keep ready(a) proved. */
    world_set(A, marked_a, true);
    world_set(B, marked_a, true);
    CHECK(agree(A, B, sy, "marked(a)=1") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "ready(a)"))) != DL_PROVED);

    /* 4. WATCHED pred, growth again on the other entity. */
    world_set(A, awake_b, true);
    world_set(B, awake_b, true);
    CHECK(agree(A, B, sy, "awake(b)=1") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(b,a)"))) == DL_PROVED);

    /* 5. WATCHED pred, RETRACTION: the match set must SHRINK. A missed
     *    re-ground here keeps a conclusion whose support is gone. */
    world_set(A, awake_a, false);
    world_set(B, awake_a, false);
    CHECK(agree(A, B, sy, "awake(a)=0") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) != DL_PROVED);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(b,a)"))) == DL_PROVED);

    /* 6. and a step still invalidates unconditionally (its commit path is not
     *    pred-scoped): waking b via the action leaves both worlds agreeing. */
    char err[96];
    uint32_t wake_a = intern_id(sy, "wake(a)");
    CHECK(world_step(A, &wake_a, 1, err, sizeof err) == 0);
    CHECK(world_step(B, &wake_a, 1, err, sizeof err) == 0);
    CHECK(agree(A, B, sy, "step wake(a)") == 0);
    CHECK(world_query(B, dl_pos(intern_id(sy, "threat(a,b)"))) == DL_PROVED);

    story_matcher_free(M);
    world_free(A);
    world_free(B);
    intern_free(sy);
    printf("test_matchwatch: all passed\n");
    return 0;
}
