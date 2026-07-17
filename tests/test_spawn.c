/* Golden tests for spawning (DESIGN.md 5.9): pools as declared capacity
 * plus an active fluent; spawn/despawn/recycle as ordinary actions with
 * complete effect lists; inactive members inert under inertia; and exact
 * replay of the whole lifecycle from the action log (I4).
 *
 * Not expressible until M1: pool-exhaustion reporting and the
 * entity-pool sugar (`entity goblin[8]`) -- those are compiler surface;
 * the runtime semantics they lower to is what this file pins. */

#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

enum { NFLUENTS = 4 };

typedef struct {
    uint32_t active1, angry1, active2, threat_seen;
    uint32_t a_spawn1, a_enrage1, a_despawn1, a_enrage2;
    uint32_t fluents[NFLUENTS];
} vocab;

/* Pool of two goblins. Spawn carries the complete effect list (the
 * recycle-reset rule: a slot's fluents are re-initialized by the spawn
 * action, never inherited from a previous life). threat is a derived
 * judgment over pool state -- never stored (I1). */
static world *build(intern *sy, vocab *v)
{
    v->active1 = intern_id(sy, "goblin1_active");
    v->angry1 = intern_id(sy, "goblin1_angry");
    v->active2 = intern_id(sy, "goblin2_active");
    v->threat_seen = intern_id(sy, "threat");
    v->a_spawn1 = intern_id(sy, "act_spawn_goblin1");
    v->a_enrage1 = intern_id(sy, "act_enrage_goblin1");
    v->a_despawn1 = intern_id(sy, "act_despawn_goblin1");
    v->a_enrage2 = intern_id(sy, "act_enrage_goblin2");
    v->fluents[0] = v->active1;
    v->fluents[1] = v->angry1;
    v->fluents[2] = v->active2;
    v->fluents[3] = 0;

    world *w = world_new(sy);
    world_declare_fluent(w, v->active1);
    world_declare_fluent(w, v->angry1);
    world_declare_fluent(w, v->active2);

    /* spawn = activate + reset: complete effect list */
    dl_lit spawn_eff[] = { dl_pos(v->active1), dl_neg(v->angry1) };
    world_add_step_rule(w, "spawn_g1", v->a_spawn1, NULL, 0, spawn_eff, 2);

    /* acting on a pool member requires it active (now) */
    step_cond g1_active = { { v->active1, false }, false };
    dl_lit enrage_eff[] = { dl_pos(v->angry1) };
    world_add_step_rule(w, "enrage_g1", v->a_enrage1, &g1_active, 1,
                        enrage_eff, 1);

    dl_lit despawn_eff[] = { dl_neg(v->active1) };
    world_add_step_rule(w, "despawn_g1", v->a_despawn1, NULL, 0,
                        despawn_eff, 1);

    /* goblin2 exists in the pool but is never spawned: its action's
     * condition can never hold */
    step_cond g2_active = { { v->active2, false }, false };
    dl_lit noop_eff[] = { dl_pos(v->angry1) };   /* would be visible if fired */
    world_add_step_rule(w, "enrage_g2", v->a_enrage2, &g2_active, 1,
                        noop_eff, 1);

    /* judgment: an active, angry goblin is a threat */
    dl_lit threat_body[] = { dl_pos(v->active1), dl_pos(v->angry1) };
    world_add_rule(w, "threat", DL_DEFEASIBLE, dl_pos(v->threat_seen),
                   threat_body, 2);
    return w;
}

static int run_log(world *w, const vocab *v, const uint32_t *log, int n,
                   char *err, size_t errsz)
{
    (void)v;
    for (int i = 0; i < n; i++)
        if (world_step(w, &log[i], 1, err, errsz) != 0) return -1;
    return 0;
}

/* Lifecycle: spawn -> enrage (threat derives) -> despawn (threat gone,
 * but the stale angry flag persists on the inactive slot -- inertia --
 * which is exactly why respawn must reset) -> respawn (reset observed).
 * The never-spawned pool member stays inert throughout. */
static int test_pool_lifecycle(void)
{
    intern *sy = intern_new();
    vocab v;
    world *w = build(sy, &v);
    char err[256];

    CHECK(world_step(w, &v.a_spawn1, 1, err, sizeof err) == 0);
    CHECK(world_get(w, v.active1) && !world_get(w, v.angry1));
    CHECK(world_query(w, dl_pos(v.threat_seen)) == DL_REFUTED);

    CHECK(world_step(w, &v.a_enrage1, 1, err, sizeof err) == 0);
    CHECK(world_get(w, v.angry1));
    CHECK(world_query(w, dl_pos(v.threat_seen)) == DL_PROVED);

    CHECK(world_step(w, &v.a_despawn1, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, v.active1));
    CHECK(world_get(w, v.angry1));   /* stale on the inactive slot: inertia */
    CHECK(world_query(w, dl_pos(v.threat_seen)) == DL_REFUTED);

    /* recycle: spawn resets the slot */
    CHECK(world_step(w, &v.a_spawn1, 1, err, sizeof err) == 0);
    CHECK(world_get(w, v.active1) && !world_get(w, v.angry1));
    CHECK(world_query(w, dl_pos(v.threat_seen)) == DL_REFUTED);

    /* inactive member: its gated action fires nothing, ever */
    CHECK(world_step(w, &v.a_enrage2, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, v.active2) && !world_get(w, v.angry1));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Replay: rebuild an identical world and re-run the action log; every
 * fluent must match the original run exactly (a save is base facts +
 * action log, I4). Spawn, despawn, and recycle are ordinary actions, so
 * they replay like everything else. */
static int test_pool_replay(void)
{
    intern *sy = intern_new();
    vocab v;
    world *w = build(sy, &v);
    char err[256];

    uint32_t log[] = { v.a_spawn1, v.a_enrage1, v.a_despawn1, v.a_spawn1,
                       v.a_enrage1, v.a_enrage2 };
    int n = (int)(sizeof log / sizeof log[0]);
    CHECK(run_log(w, &v, log, n, err, sizeof err) == 0);

    world *w2 = build(sy, &v);
    CHECK(run_log(w2, &v, log, n, err, sizeof err) == 0);

    for (int i = 0; i < NFLUENTS && v.fluents[i]; i++)
        CHECK(world_get(w, v.fluents[i]) == world_get(w2, v.fluents[i]));
    CHECK(world_query(w, dl_pos(v.threat_seen)) ==
          world_query(w2, dl_pos(v.threat_seen)));

    world_free(w);
    world_free(w2);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_pool_lifecycle()) return 1;
    if (test_pool_replay()) return 1;
    printf("test_spawn: all passed\n");
    return 0;
}
