/* Golden tests for the step function: defeasible inertia, ramifications,
 * conflict detection, judgment-gated actions (DESIGN.md 5.3). */

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

/* Yale shooting: inertia holds through 'wait'; 'shoot' defeats it. */
static int test_yale_shooting(void)
{
    intern *sy = intern_new();
    uint32_t loaded = intern_id(sy, "loaded"), alive = intern_id(sy, "alive");
    uint32_t a_load = intern_id(sy, "act_load"),
             a_wait = intern_id(sy, "act_wait"),
             a_shoot = intern_id(sy, "act_shoot");

    world *w = world_new(sy);
    world_declare_fluent(w, loaded);
    world_declare_fluent(w, alive);
    world_set(w, alive, true);

    dl_lit e_loaded = dl_pos(loaded);
    world_add_step_rule(w, "load", a_load, NULL, 0, &e_loaded, 1);
    step_cond gun_loaded = { { loaded, false }, false };
    dl_lit e_dead[] = { dl_neg(alive), dl_neg(loaded) };
    world_add_step_rule(w, "shoot", a_shoot, &gun_loaded, 1, e_dead, 2);

    char err[256];
    CHECK(world_step(w, &a_load, 1, err, sizeof err) == 0);
    CHECK(world_get(w, loaded) && world_get(w, alive));

    CHECK(world_step(w, &a_wait, 1, err, sizeof err) == 0);
    CHECK(world_get(w, loaded) && world_get(w, alive));  /* inertia */

    CHECK(world_step(w, &a_shoot, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, loaded) && !world_get(w, alive));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Ramification: the dead guard drops the torch, in the same step. */
static int test_ramification(void)
{
    intern *sy = intern_new();
    uint32_t alive = intern_id(sy, "guard_alive"),
             holding = intern_id(sy, "guard_holds_torch"),
             floor = intern_id(sy, "torch_on_floor");
    uint32_t a_shoot = intern_id(sy, "act_shoot_guard");

    world *w = world_new(sy);
    world_declare_fluent(w, alive);
    world_declare_fluent(w, holding);
    world_declare_fluent(w, floor);
    world_set(w, alive, true);
    world_set(w, holding, true);

    dl_lit e_kill = dl_neg(alive);
    world_add_step_rule(w, "shoot_guard", a_shoot, NULL, 0, &e_kill, 1);
    /* body mixes next-state (primed) and current-state atoms */
    step_cond drop_body[] = {
        { { alive, true }, true },     /* ~guard_alive' */
        { { holding, false }, false }, /* guard_holds_torch (now) */
    };
    dl_lit drop_eff[] = { dl_neg(holding), dl_pos(floor) };
    world_add_step_rule(w, "drop_on_death", INTERN_NONE,
                        drop_body, 2, drop_eff, 2);

    char err[256];
    CHECK(world_step(w, &a_shoot, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, alive));
    CHECK(!world_get(w, holding));
    CHECK(world_get(w, floor));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Two causal rules with contradictory effects and no superiority:
 * the step must be rejected, state untouched. */
static int test_conflict_detection(void)
{
    intern *sy = intern_new();
    uint32_t lamp = intern_id(sy, "lamp_on");
    uint32_t a_flip = intern_id(sy, "act_flip");

    world *w = world_new(sy);
    world_declare_fluent(w, lamp);
    world_set(w, lamp, false);

    dl_lit on = dl_pos(lamp), off = dl_neg(lamp);
    world_add_step_rule(w, "turns_on", a_flip, NULL, 0, &on, 1);
    world_add_step_rule(w, "turns_off", a_flip, NULL, 0, &off, 1);

    char err[256] = "";
    CHECK(world_step(w, &a_flip, 1, err, sizeof err) == -1);
    CHECK(err[0] != '\0');
    CHECK(!world_get(w, lamp));   /* untouched */

    world_free(w);
    intern_free(sy);
    return 0;
}

/* The cellar: judgments (weakened / can_force) with defeater + superiority,
 * gating an action whose condition is itself a derived judgment. */
static int test_cellar(void)
{
    intern *sy = intern_new();
    uint32_t poisoned = intern_id(sy, "poisoned"),
             antidote = intern_id(sy, "has_antidote"),
             strong = intern_id(sy, "strong"),
             closed = intern_id(sy, "door_closed"),
             open = intern_id(sy, "door_open"),
             weakened = intern_id(sy, "weakened"),
             can_force = intern_id(sy, "can_force_door");
    uint32_t a_force = intern_id(sy, "act_force_door");

    world *w = world_new(sy);
    world_declare_fluent(w, poisoned);
    world_declare_fluent(w, antidote);
    world_declare_fluent(w, strong);
    world_declare_fluent(w, closed);
    world_declare_fluent(w, open);
    world_set(w, poisoned, true);
    world_set(w, strong, true);
    world_set(w, closed, true);

    dl_lit po = dl_pos(poisoned), an = dl_pos(antidote);
    world_add_rule(w, "poison_weakens", DL_DEFEASIBLE, dl_pos(weakened), &po, 1);
    world_add_rule(w, "antidote_blocks", DL_DEFEATER, dl_neg(weakened), &an, 1);
    dl_lit force_body[] = { dl_pos(strong), dl_pos(closed) };
    int r_force = world_add_rule(w, "can_force", DL_DEFEASIBLE,
                                 dl_pos(can_force), force_body, 2);
    dl_lit wk = dl_pos(weakened);
    int r_tooweak = world_add_rule(w, "too_weak", DL_DEFEASIBLE,
                                   dl_neg(can_force), &wk, 1);
    world_add_sup(w, r_tooweak, r_force);

    /* judgment gates the action: condition is the derived can_force_door */
    step_cond force_cond[] = {
        { { can_force, false }, false },
        { { closed, false }, false },
    };
    dl_lit force_eff[] = { dl_pos(open), dl_neg(closed) };
    world_add_step_rule(w, "force_door", a_force, force_cond, 2, force_eff, 2);

    /* poisoned, no antidote: the exception beats the norm */
    CHECK(world_query(w, dl_pos(weakened)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(can_force)) == DL_REFUTED);

    /* forcing the door does nothing (condition unprovable, inertia holds) */
    char err[256];
    CHECK(world_step(w, &a_force, 1, err, sizeof err) == 0);
    CHECK(world_get(w, closed) && !world_get(w, open));

    /* antidote picked up: defeater blocks 'weakened', norm reinstated */
    world_set(w, antidote, true);
    CHECK(world_query(w, dl_pos(weakened)) == DL_REFUTED);
    CHECK(world_query(w, dl_pos(can_force)) == DL_PROVED);

    CHECK(world_step(w, &a_force, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, closed) && world_get(w, open));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* The cached step schema must rebuild when the world grows mid-game: step,
 * then add a new fluent and step rule, step again — the new rule fires, the
 * old fluent's state and inertia survive the rebuild, and an action atom
 * interned after the schema was built is safely inert. */
static int test_grow_mid_game(void)
{
    intern *sy = intern_new();
    uint32_t lamp = intern_id(sy, "lamp"),
             a_on = intern_id(sy, "act_on");

    world *w = world_new(sy);
    world_declare_fluent(w, lamp);
    dl_lit effs[1];
    effs[0] = dl_pos(lamp);
    world_add_step_rule(w, "switch_on", a_on, NULL, 0, effs, 1);

    uint32_t acts[2];
    acts[0] = a_on;
    CHECK(world_step(w, acts, 1, NULL, 0) == 0);
    CHECK(world_get(w, lamp));

    /* grow: a second fluent + rule, plus an action no rule mentions */
    uint32_t door = intern_id(sy, "door_open"),
             a_open = intern_id(sy, "act_open"),
             a_noop = intern_id(sy, "act_dance");
    world_declare_fluent(w, door);
    effs[0] = dl_pos(door);
    world_add_step_rule(w, "open_door", a_open, NULL, 0, effs, 1);

    acts[0] = a_open;
    acts[1] = a_noop;
    CHECK(world_step(w, acts, 2, NULL, 0) == 0);
    CHECK(world_get(w, door));
    CHECK(world_get(w, lamp));      /* inertia across the rebuild */

    CHECK(world_step(w, NULL, 0, NULL, 0) == 0);   /* pure inertia step */
    CHECK(world_get(w, door) && world_get(w, lamp));

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_yale_shooting()) return 1;
    if (test_ramification()) return 1;
    if (test_conflict_detection()) return 1;
    if (test_cellar()) return 1;
    if (test_grow_mid_game()) return 1;
    printf("test_world: all passed\n");
    return 0;
}
