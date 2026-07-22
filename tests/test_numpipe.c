/* Golden tests for the numeric-fluent WRITE side (DESIGN.md 5.8): the effect
 * operators (`:=`/`+=`/`-=`), the expression VM, and the commit pipeline —
 * base (winning `:=`, else inertia) -> Σ undefeated deltas -> clamp to the
 * declared range. These are the tests test_landmark.c's header lists as
 * "await the M1 value store"; the store's write side now exists, so they move
 * here. Built against the engine C API, the way the M1 compiler will lower.
 *
 * Pinned here (the default §5.8 semantics):
 *   - two deltas on one tick SUM, order-free;
 *   - a winning `:=` replaces the base, deltas still apply on top;
 *   - the declared range clamps as the OUTERMOST stage;
 *   - heal-plus-fire runs base/deltas/clamp in one step;
 *   - two unresolved `:=` on one fluent is a contested step (world_step -> -1,
 *     state untouched), the numeric twin of the boolean flip-flop rejection;
 *   - the commit receipt is itemized structured data (assign first, then each
 *     delta with its source rule) — the thing BG3 floating combat text is a
 *     view of, never a parsed why? string;
 *   - effect RHSs are full expressions through the VM (`max(1, hp - dmg)`);
 *   - the write side feeds the read side: a delta that crosses a threshold
 *     flips a guard-fed judgment, one that does not leaves it untouched.
 *
 * DEFERRED to later slices (so the gap stays visible, per 5.8's golden list):
 *   - suppression across rules by superiority over numeric effects, and the
 *     per-fluent `combine min/max` collision resolver (needs effect reification);
 *   - the dying trigger (a PRIMED numeric guard concluding dead' in-step) and
 *     heal/curse oscillator rejection — both need the §5.8 stratified schedule. */

#include "state/world.h"
#include "logic/dl.h"
#include "core/intern.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* --- two damage sources on one tick sum, order-free (5.8) --- */
static int test_two_deltas_sum(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), hit = intern_id(sy, "hit");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 0, false);
    world_set_num(w, hp, 12);

    expr_ins three[] = {{EXPR_CONST, 3}}, four[] = {{EXPR_CONST, 4}};
    int r1 = world_add_step_rule(w, "goblin_stab", hit, NULL, 0, NULL, 0);
    world_add_num_effect(w, r1, hp, WORLD_OP_SUB, three, 1);
    int r2 = world_add_step_rule(w, "fire_aura", hit, NULL, 0, NULL, 0);
    world_add_num_effect(w, r2, hp, WORLD_OP_SUB, four, 1);

    char err[128];
    uint32_t acts[] = { hit };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 5);          /* 12 - 3 - 4 */

    /* no action → no effect fires → inertia holds the value unchanged */
    CHECK(world_step(w, NULL, 0, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 5);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- a winning `:=` sets the base; deltas still apply on top --- */
static int test_assign_then_delta(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), turn = intern_id(sy, "turn");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 0, false);
    world_set_num(w, hp, 5);

    expr_ins twenty[] = {{EXPR_CONST, 20}}, four[] = {{EXPR_CONST, 4}};
    int h = world_add_step_rule(w, "full_heal", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, h, hp, WORLD_OP_ASSIGN, twenty, 1);
    int fzr = world_add_step_rule(w, "fire_aura", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, fzr, hp, WORLD_OP_SUB, four, 1);

    char err[128];
    uint32_t acts[] = { turn };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 16);         /* base 20 (:=), then -4 */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the declared range clamps as the outermost stage --- */
static int test_range_clamp(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), hit = intern_id(sy, "hit"),
             heal = intern_id(sy, "heal");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 20, true);     /* hp : int in 0..20 */
    world_set_num(w, hp, 3);

    expr_ins ten[] = {{EXPR_CONST, 10}}, twentyfive[] = {{EXPR_CONST, 25}};
    int d = world_add_step_rule(w, "big_hit", hit, NULL, 0, NULL, 0);
    world_add_num_effect(w, d, hp, WORLD_OP_SUB, ten, 1);
    int u = world_add_step_rule(w, "over_heal", heal, NULL, 0, NULL, 0);
    world_add_num_effect(w, u, hp, WORLD_OP_ASSIGN, twentyfive, 1);

    char err[128];
    uint32_t hitact[] = { hit };
    CHECK(world_step(w, hitact, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 0);          /* 3 - 10 = -7, clamped to 0 */

    uint32_t healact[] = { heal };
    CHECK(world_step(w, healact, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 20);         /* := 25, clamped to 20 */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- heal-plus-fire: base, deltas, clamp all exercised in one step --- */
static int test_heal_plus_fire(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), turn = intern_id(sy, "turn");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 20, true);
    world_set_num(w, hp, 2);

    /* full heal to max, minus this tick's fire, clamped by the range */
    expr_ins tomax[] = {{EXPR_CONST, 100}};    /* heal to 100, range caps it */
    expr_ins four[] = {{EXPR_CONST, 4}};
    int h = world_add_step_rule(w, "full_heal", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, h, hp, WORLD_OP_ASSIGN, tomax, 1);
    int fzr = world_add_step_rule(w, "fire_aura", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, fzr, hp, WORLD_OP_SUB, four, 1);

    char err[128];
    uint32_t acts[] = { turn };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    /* base := 100, delta -4 -> 96, clamp to 20. "full minus 4" only if the
     * range didn't already bite; here the range is the outermost stage. */
    CHECK(world_get_num(w, hp) == 20);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- two unresolved `:=` on one fluent contest the step (5.8) --- */
static int test_assign_conflict_rejected(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), turn = intern_id(sy, "turn");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 0, false);
    world_set_num(w, hp, 8);

    expr_ins twenty[] = {{EXPR_CONST, 20}}, one[] = {{EXPR_CONST, 1}};
    int a = world_add_step_rule(w, "heal", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, a, hp, WORLD_OP_ASSIGN, twenty, 1);
    int b = world_add_step_rule(w, "curse", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, b, hp, WORLD_OP_ASSIGN, one, 1);

    char err[128];
    err[0] = '\0';
    uint32_t acts[] = { turn };
    CHECK(world_step(w, acts, 1, err, sizeof err) == -1);
    CHECK(world_get_num(w, hp) == 8);          /* state untouched on reject */
    /* the error names the contested fluent */
    CHECK(strstr(err, "hp") != NULL);

    /* two `:=` of the SAME value agree — not a conflict */
    world *w2 = world_new(sy);
    world_declare_num(w2, hp, 0, 0, false);
    world_set_num(w2, hp, 8);
    int a2 = world_add_step_rule(w2, "heal_a", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w2, a2, hp, WORLD_OP_ASSIGN, twenty, 1);
    int b2 = world_add_step_rule(w2, "heal_b", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w2, b2, hp, WORLD_OP_ASSIGN, twenty, 1);
    CHECK(world_step(w2, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w2, hp) == 20);
    world_free(w2);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the commit receipt: itemized, assign first then deltas, sources named --- */
static int test_commit_receipt(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), turn = intern_id(sy, "turn");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 100, true);
    world_set_num(w, hp, 5);

    expr_ins fifty[] = {{EXPR_CONST, 50}}, three[] = {{EXPR_CONST, 3}},
             four[] = {{EXPR_CONST, 4}};
    int h = world_add_step_rule(w, "chug_potion", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, h, hp, WORLD_OP_ASSIGN, fifty, 1);
    int g = world_add_step_rule(w, "goblin_stab", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, g, hp, WORLD_OP_SUB, three, 1);
    int f = world_add_step_rule(w, "fire_aura", turn, NULL, 0, NULL, 0);
    world_add_num_effect(w, f, hp, WORLD_OP_SUB, four, 1);

    char err[128];
    uint32_t acts[] = { turn };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 43);         /* 50 - 3 - 4 */

    long base;
    world_contrib items[8];
    int n = world_num_receipt(w, hp, &base, items, 8);
    CHECK(base == 50);
    CHECK(n == 3);
    /* assign first */
    CHECK(items[0].op == WORLD_OP_ASSIGN);
    CHECK(items[0].amount == 50);
    CHECK(strcmp(items[0].rule, "chug_potion") == 0);
    /* then the deltas, signed, in scan order, each with its source rule */
    CHECK(items[1].op == WORLD_OP_SUB && items[1].amount == -3);
    CHECK(strcmp(items[1].rule, "goblin_stab") == 0);
    CHECK(items[2].op == WORLD_OP_SUB && items[2].amount == -4);
    CHECK(strcmp(items[2].rule, "fire_aura") == 0);

    /* a fluent with no effects this step reports an empty receipt */
    uint32_t mp = intern_id(sy, "mp");
    world_declare_num(w, mp, 0, 0, false);
    world_set_num(w, mp, 7);
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    n = world_num_receipt(w, mp, &base, items, 8);
    CHECK(n == 0 && base == 7);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- effect RHSs are full expressions through the VM --- */
static int test_expr_vm(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), hit = intern_id(sy, "hit");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 0, false);
    world_set_num(w, hp, 6);

    /* hp := max(1, hp - 8): reads the fluent, subtracts, floors at 1 */
    expr_ins code[] = {
        {EXPR_CONST, 1},
        {EXPR_LOAD, (long)hp},
        {EXPR_CONST, 8},
        {EXPR_SUB, 0},
        {EXPR_MAX, 0},
    };
    int r = world_add_step_rule(w, "brutal", hit, NULL, 0, NULL, 0);
    world_add_num_effect(w, r, hp, WORLD_OP_ASSIGN, code, 5);

    char err[128];
    uint32_t acts[] = { hit };
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 1);          /* max(1, 6-8) = max(1,-2) = 1 */

    /* again from a healthier value: max(1, 30-8) = 22 */
    world_set_num(w, hp, 30);
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 22);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the write side feeds the read side across a threshold --- */
static int test_write_crosses_threshold(void)
{
    intern *sy = intern_new();
    uint32_t hp = intern_id(sy, "hp"), hit = intern_id(sy, "hit"),
             hp_le0 = intern_id(sy, "hp<=0"), dead = intern_id(sy, "dead");
    world *w = world_new(sy);
    world_declare_num(w, hp, 0, 0, false);
    world_set_num(w, hp, 5);
    world_add_guard(w, hp_le0, hp, WORLD_CMP_LE, 0);
    /* judgment: hp<=0 => dead (read side over the value store) */
    dl_lit body = dl_pos(hp_le0);
    world_add_rule(w, "die", DL_DEFEASIBLE, dl_pos(dead), &body, 1);

    expr_ins three[] = {{EXPR_CONST, 3}};
    int r = world_add_step_rule(w, "stab", hit, NULL, 0, NULL, 0);
    world_add_num_effect(w, r, hp, WORLD_OP_SUB, three, 1);

    char err[128];
    uint32_t acts[] = { hit };
    /* chip that crosses no threshold: 5 -> 2, dead stays unproved */
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == 2);
    CHECK(world_query(w, dl_pos(dead)) != DL_PROVED);

    /* the blow that crosses it: 2 -> -1, dead now proved */
    CHECK(world_step(w, acts, 1, err, sizeof err) == 0);
    CHECK(world_get_num(w, hp) == -1);
    CHECK(world_query(w, dl_pos(dead)) == DL_PROVED);
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_two_deltas_sum())          return 1;
    if (test_assign_then_delta())       return 1;
    if (test_range_clamp())             return 1;
    if (test_heal_plus_fire())          return 1;
    if (test_assign_conflict_rejected()) return 1;
    if (test_commit_receipt())          return 1;
    if (test_expr_vm())                 return 1;
    if (test_write_crosses_threshold()) return 1;
    printf("test_numpipe: all passed\n");
    return 0;
}
