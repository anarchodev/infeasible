/* Golden test for subscriptions (§11 M2) — the client's reactive channel.
 *
 * The design claim being pinned is that ONE primitive covers facts and
 * judgments. A base fact is the free leaf case (the step already computes its
 * changeset); a derived judgment is the cone-recompute case. They differ in
 * cost, never in call shape — which is what makes a fluent refactored into a
 * judgment invisible to a client, because a subscription names a CONCLUSION,
 * not a storage decision.
 *
 * The rest is the loop contract: a level to decide from and edges to react to,
 * measured per step (the `btnp` to level's `btn`); stable handles; edges in
 * subscription order and absent on a step that did not happen. */

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

/* `weakened` and `can_force_door` are DERIVED — nothing stores them — while
 * `poisoned` and `door_closed` are base facts. A client subscribes to all four
 * the same way. */
static const char *SRC =
    "sort actor\n"
    "entity ( hero, guard : actor )\n"
    "state (\n"
    "  poisoned(actor)\n"
    "  strong(actor)\n"
    "  door_closed\n"
    ")\n"
    "init ( strong(hero) strong(guard) door_closed )\n"
    "rule weak(X: actor):     poisoned(X)              => weakened(X)\n"
    "rule can_force(X: actor): strong(X) & door_closed => can_force_door(X)\n"
    "rule too_weak(X: actor):  weakened(X)             => ~can_force_door(X)\n"
    "too_weak > can_force\n"
    "action poison(X: actor): causes poisoned(X)\n"
    "action cure(X: actor):   causes ~poisoned(X)\n"
    "exclusive poison(X), cure(X)\n"
    "action force_door(X: actor):\n"
    "  requires can_force_door(X) & door_closed\n"
    "  causes   ~door_closed\n";

static world *compile(const char *src, intern *sy)
{
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "  compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

/* The edge for subscription `h` in the last step, or NULL if it did not flip. */
static const world_sub_edge *edge_for(world *w, int h)
{
    int n;
    const world_sub_edge *e = world_sub_edges(w, &n);
    for (int i = 0; i < n; i++)
        if (e[i].sub == h) return &e[i];
    return NULL;
}

int main(void)
{
    intern *sy = intern_new();
    world *w = compile(SRC, sy);
    CHECK(w != NULL);

    uint32_t poisoned_h = intern_id(sy, "poisoned(hero)"),
             weakened_h = intern_id(sy, "weakened(hero)"),
             force_h    = intern_id(sy, "can_force_door(hero)"),
             door       = intern_id(sy, "door_closed");
    char err[128];

    /* one call shape for a base fact and for two judgments */
    int s_fact  = world_subscribe(w, dl_pos(poisoned_h));
    int s_weak  = world_subscribe(w, dl_pos(weakened_h));
    int s_force = world_subscribe(w, dl_pos(force_h));
    int s_door  = world_subscribe(w, dl_pos(door));

    /* the level is valid before any step — a client may subscribe mid-run */
    CHECK(world_sub_verdict(w, s_fact)  == DL_REFUTED);
    CHECK(world_sub_verdict(w, s_force) == DL_PROVED);
    CHECK(world_sub_verdict(w, s_door)  == DL_PROVED);

    /* nothing has stepped, so there are no edges */
    int n;
    world_sub_edges(w, &n);
    CHECK(n == 0);

    /* --- one action, one fact, and the judgments in its cone -------------- */
    uint32_t poison = intern_id(sy, "poison(hero)");
    CHECK(world_step(w, &poison, 1, err, sizeof err) == 0);

    world_sub_edges(w, &n);
    CHECK(n == 3);                       /* the fact and BOTH judgments moved */
    const world_sub_edge *e = edge_for(w, s_fact);
    CHECK(e && e->from == DL_REFUTED && e->to == DL_PROVED);
    e = edge_for(w, s_weak);
    CHECK(e && e->to == DL_PROVED);      /* the leaf's cone, recomputed */
    e = edge_for(w, s_force);
    CHECK(e && e->from == DL_PROVED && e->to == DL_REFUTED);   /* the exception won */
    CHECK(edge_for(w, s_door) == NULL);  /* untouched: absent, not reported flat */

    /* edges come in subscription order — deterministic, so a replay produces
     * the same stream (I4) */
    const world_sub_edge *all = world_sub_edges(w, &n);
    CHECK(all[0].sub == s_fact && all[1].sub == s_weak && all[2].sub == s_force);

    /* the level agrees with the edge that produced it */
    CHECK(world_sub_verdict(w, s_force) == DL_REFUTED);

    /* --- a step that changes nothing relevant reports nothing ------------- */
    uint32_t poison_g = intern_id(sy, "poison(guard)");
    CHECK(world_step(w, &poison_g, 1, err, sizeof err) == 0);
    CHECK(edge_for(w, s_fact) == NULL);      /* hero's fact did not move */
    CHECK(edge_for(w, s_weak) == NULL);
    world_sub_edges(w, &n);
    CHECK(n == 0);

    /* --- the reverse flip, and the judgment following it back ------------- */
    uint32_t cure = intern_id(sy, "cure(hero)");
    CHECK(world_step(w, &cure, 1, err, sizeof err) == 0);
    e = edge_for(w, s_fact);
    CHECK(e && e->from == DL_PROVED && e->to == DL_REFUTED);
    e = edge_for(w, s_force);
    CHECK(e && e->to == DL_PROVED);          /* the exception lapsed */

    /* --- a REJECTED step produces no edges (no tick, nothing moved) ------- */
    uint32_t bogus = intern_id(sy, "no_such_action");
    CHECK(world_step(w, &bogus, 1, err, sizeof err) == -1);
    world_sub_edges(w, &n);
    CHECK(n == 0);

    /* --- unsubscribing is silent, and handles stay stable ----------------- */
    world_unsubscribe(w, s_weak);
    int s_late = world_subscribe(w, dl_pos(intern_id(sy, "weakened(guard)")));
    CHECK(s_late != s_weak);                 /* the slot is not recycled */
    CHECK(world_sub_verdict(w, s_late) == DL_PROVED);   /* guard was poisoned */

    CHECK(world_step(w, &poison, 1, err, sizeof err) == 0);
    CHECK(edge_for(w, s_weak) == NULL);      /* unsubscribed: no longer reported */
    CHECK(edge_for(w, s_fact) != NULL);      /* its neighbour still is */
    CHECK(world_sub_verdict(w, s_weak) == DL_UNDECIDED);   /* and reads inert */

    /* --- a NEGATIVE literal is an ordinary subscription ------------------- */
    int s_neg = world_subscribe(w, dl_neg(force_h));
    CHECK(world_sub_verdict(w, s_neg) == DL_PROVED);       /* ~can_force_door */
    CHECK(world_step(w, &cure, 1, err, sizeof err) == 0);
    e = edge_for(w, s_neg);
    CHECK(e && e->from == DL_PROVED && e->to == DL_REFUTED);
    CHECK(e->lit.neg);

    /* --- the leaf case agrees with the changeset -------------------------- */
    {
        int nb;
        CHECK(world_step(w, &poison, 1, err, sizeof err) == 0);
        const world_bool_delta *bd = world_bool_deltas(w, &nb);
        const world_sub_edge *fe = edge_for(w, s_fact);
        CHECK(fe != NULL);
        bool in_changeset = false;
        for (int i = 0; i < nb; i++)
            if (bd[i].atom == poisoned_h)
                in_changeset = (bd[i].value == (fe->to == DL_PROVED));
        CHECK(in_changeset);          /* one truth, two readouts */
    }

    world_free(w);
    intern_free(sy);
    printf("test_subscribe: all passed\n");
    return 0;
}
