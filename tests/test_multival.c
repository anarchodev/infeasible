/* Golden tests for multi-valued fluents and defeat across values
 * (DESIGN.md 5.7), pinned via the erasure encoding the M1 compiler will
 * emit, hand-built against the M0 engine:
 *
 *   - a fluent f : {a,b,c} becomes one atom per value (f_a, f_b, f_c);
 *   - a rule concluding f=a becomes a FAMILY: the primary (body => f_a)
 *     plus one shadow per sibling value (body => ~f_b, body => ~f_c),
 *     all sharing the rule's body;
 *   - superiority r > s (r for value v, s for value u, v != u) mirrors
 *     onto the conflicting family members: sup(r, s_shadow_v) and
 *     sup(r_shadow_u, s);
 *   - facts assert exactly-one-value closed-world (f_a, ~f_b, ~f_c).
 *
 * No strict exclusion axioms (f_a -> ~f_b): they create negative cycles
 * the tri-valued fixpoint leaves UNDECIDED. Exclusion is by construction.
 *
 * Note that this encoding yields STRICT-TEAM defeat inherently: attackers
 * of f_a can only be beaten by supporters of f_a, because team defeat is
 * per-literal. The doc's strict-vs-coalition decision is also the natural
 * one under erasure; test_strict_team_contested pins it.
 *
 * DISCOVERY pinned by the sealed-door pair below: DESIGN.md 5.7's claim
 * that a value-specific defeater ("sealed ~> ~(door=open)") composes with
 * inertia into "unchanged" is NOT what the erasure gives. The blocked
 * rule's shadow (~locked') is still applicable and still beats inertia,
 * so no value is provable and the step is rejected. Getting the
 * documented behavior needs the whole effect family to withdraw when its
 * value conclusion is defeated -- an M1 semantic decision. The authoring
 * pattern that works today is a requires-condition (rule inapplicable =>
 * shadows inapplicable => inertia holds); both truths are pinned. */

#include "logic/dl.h"
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

/* Build the three-value judgment theory stance : {a,b,c} with one rule
 * family per value, all applicable. Returns rule ids via out params:
 * ids[v][0] = primary for value v; ids[v][1], ids[v][2] = shadows against
 * the other two values, in (a,b,c) order skipping v itself. */
static void add_family(dl_theory *t, const char *tag, int v,
                       uint32_t val_atoms[3], uint32_t trigger, int ids[3])
{
    char buf[32];
    dl_lit body = dl_pos(trigger);
    snprintf(buf, sizeof buf, "%s", tag);
    ids[0] = dl_add_rule(t, buf, DL_DEFEASIBLE, dl_pos(val_atoms[v]), &body, 1);
    int k = 1;
    for (int u = 0; u < 3; u++) {
        if (u == v) continue;
        snprintf(buf, sizeof buf, "%s_not%d", tag, u);
        ids[k++] = dl_add_rule(t, buf, DL_DEFEASIBLE,
                               dl_neg(val_atoms[u]), &body, 1);
    }
}

/* shadow id within a family that attacks value u (u != v of the family) */
static int shadow_against(int v, int u, const int ids[3])
{
    int k = 1;
    for (int w = 0; w < 3; w++) {
        if (w == v) continue;
        if (w == u) return ids[k];
        k++;
    }
    return -1;
}

/* mirror superiority ri > rj across families (value vi beats value vj) */
static void mirror_sup(dl_theory *t, int vi, const int ri[3],
                       int vj, const int rj[3])
{
    /* ri primary (f_vi) beats rj's shadow against vi (~f_vi) */
    dl_add_sup(t, ri[0], shadow_against(vj, vi, rj));
    /* ri's shadow against vj (~f_vj) beats rj primary (f_vj) */
    dl_add_sup(t, shadow_against(vi, vj, ri), rj[0]);
}

/* Intransitive hand-written chain r1 > r3 > r2 with the r1 > r2 edge
 * missing: strict teams leave NO value provable (the step-level meaning
 * of contested). Pins the strict-vs-coalition decision: coalition would
 * prove stance=a; strict teams deadlock on it, because a's attacker r2
 * is beaten by nobody on a's own team. The chain-defeated values b and c
 * do have their negations established (each ~ has a supporter beating
 * its lone attacker via the mirrored edges) -- the deadlock is exactly
 * and only on the would-be winner, which is why "add r1 > r2" is the
 * right compile-time suggestion. */
static int test_strict_team_contested(void)
{
    intern *sy = intern_new();
    uint32_t val[3] = { intern_id(sy, "stance_a"), intern_id(sy, "stance_b"),
                        intern_id(sy, "stance_c") };
    uint32_t t1 = intern_id(sy, "t1"), t2 = intern_id(sy, "t2"),
             t3 = intern_id(sy, "t3");

    dl_theory *t = dl_theory_new(sy);
    int r1[3], r2[3], r3[3];
    add_family(t, "r1", 0, val, t1, r1);   /* => stance=a */
    add_family(t, "r2", 1, val, t2, r2);   /* => stance=b */
    add_family(t, "r3", 2, val, t3, r3);   /* => stance=c */
    mirror_sup(t, 0, r1, 2, r3);           /* r1 > r3 */
    mirror_sup(t, 2, r3, 1, r2);           /* r3 > r2 */
    dl_add_fact(t, dl_pos(t1));
    dl_add_fact(t, dl_pos(t2));
    dl_add_fact(t, dl_pos(t3));

    dl_result *res = dl_solve(t);
    /* no value is provable -- a step on this fluent would be rejected */
    for (int v = 0; v < 3; v++)
        CHECK(dl_defeasible(res, dl_pos(val[v])) == DL_REFUTED);
    /* the deadlock is on the would-be winner alone */
    CHECK(dl_defeasible(res, dl_neg(val[0])) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(val[1])) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_neg(val[2])) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* The same three rules in bands 3/2/1 (a over c over b): band-induced
 * superiority is total, so a beats every attacker directly and wins.
 * Also pins the at-most-one-value invariant. */
static int test_bands_resolve(void)
{
    intern *sy = intern_new();
    uint32_t val[3] = { intern_id(sy, "stance_a"), intern_id(sy, "stance_b"),
                        intern_id(sy, "stance_c") };
    uint32_t t1 = intern_id(sy, "t1"), t2 = intern_id(sy, "t2"),
             t3 = intern_id(sy, "t3");

    dl_theory *t = dl_theory_new(sy);
    int r1[3], r2[3], r3[3];
    add_family(t, "r1", 0, val, t1, r1);
    add_family(t, "r2", 1, val, t2, r2);
    add_family(t, "r3", 2, val, t3, r3);
    /* bands compile to the transitive closure of pairwise edges */
    mirror_sup(t, 0, r1, 2, r3);           /* band 3 > band 2 */
    mirror_sup(t, 0, r1, 1, r2);           /* band 3 > band 1 */
    mirror_sup(t, 2, r3, 1, r2);           /* band 2 > band 1 */
    dl_add_fact(t, dl_pos(t1));
    dl_add_fact(t, dl_pos(t2));
    dl_add_fact(t, dl_pos(t3));

    dl_result *res = dl_solve(t);
    CHECK(dl_defeasible(res, dl_pos(val[0])) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_pos(val[1])) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_pos(val[2])) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(val[1])) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_neg(val[2])) == DL_PROVED);
    /* at most one value +d */
    int winners = 0;
    for (int v = 0; v < 3; v++)
        if (dl_defeasible(res, dl_pos(val[v])) == DL_PROVED) winners++;
    CHECK(winners == 1);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Multi-valued flip-flop at the step level: door : {locked, closed, open},
 * two causal rules on one action forcing different values. A value-write
 * erases to the full effect family (own value + sibling negations), so
 * the rules contest a sibling atom and the step must be rejected with the
 * state untouched. A single writer commits exactly-one-value. */
static int test_flipflop_step(void)
{
    intern *sy = intern_new();
    uint32_t locked = intern_id(sy, "door_locked"),
             closed = intern_id(sy, "door_closed"),
             open = intern_id(sy, "door_open");
    uint32_t a_flip = intern_id(sy, "act_flip"),
             a_open = intern_id(sy, "act_open");

    world *w = world_new(sy);
    world_declare_fluent(w, locked);
    world_declare_fluent(w, closed);
    world_declare_fluent(w, open);
    world_set(w, locked, true);   /* door = locked */

    dl_lit to_open[]  = { dl_pos(open), dl_neg(locked), dl_neg(closed) };
    dl_lit to_close[] = { dl_pos(closed), dl_neg(locked), dl_neg(open) };
    world_add_step_rule(w, "flip_open", a_flip, NULL, 0, to_open, 3);
    world_add_step_rule(w, "flip_close", a_flip, NULL, 0, to_close, 3);
    world_add_step_rule(w, "just_open", a_open, NULL, 0, to_open, 3);

    char err[256] = "";
    CHECK(world_step(w, &a_flip, 1, err, sizeof err) == -1);
    CHECK(err[0] != '\0');
    CHECK(world_get(w, locked) && !world_get(w, closed) && !world_get(w, open));

    CHECK(world_step(w, &a_open, 1, err, sizeof err) == 0);
    CHECK(world_get(w, open) && !world_get(w, locked) && !world_get(w, closed));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Binary degeneration: a two-value domain encoded the same way (own value
 * + the single sibling negation) collapses to the plain boolean fluent
 * the existing suite pins -- same conflict, same commit. */
static int test_boolean_degeneration(void)
{
    intern *sy = intern_new();
    uint32_t on = intern_id(sy, "lamp_on"), off = intern_id(sy, "lamp_off");
    uint32_t a_flip = intern_id(sy, "act_flip"), a_on = intern_id(sy, "act_on");

    world *w = world_new(sy);
    world_declare_fluent(w, on);
    world_declare_fluent(w, off);
    world_set(w, off, true);

    dl_lit to_on[]  = { dl_pos(on), dl_neg(off) };
    dl_lit to_off[] = { dl_pos(off), dl_neg(on) };
    world_add_step_rule(w, "turns_on", a_flip, NULL, 0, to_on, 2);
    world_add_step_rule(w, "turns_off", a_flip, NULL, 0, to_off, 2);
    world_add_step_rule(w, "just_on", a_on, NULL, 0, to_on, 2);

    char err[256] = "";
    CHECK(world_step(w, &a_flip, 1, err, sizeof err) == -1);
    CHECK(world_get(w, off) && !world_get(w, on));
    CHECK(world_step(w, &a_on, 1, err, sizeof err) == 0);
    CHECK(world_get(w, on) && !world_get(w, off));

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Sealed door, defeater version -- pins the DISCOVERY (header comment):
 * one hand-built step theory over primed atoms, door : {locked, open}
 * currently locked, an open-action whose family is (open', ~locked'),
 * and a defeater sealed ~> ~open'. The defeater blocks open', but the
 * family's shadow ~locked' is still applicable and beats locked-inertia,
 * so NEITHER value is provable: the step would be rejected, the door is
 * not "kept locked by inertia" as 5.7's prose claims. */
static int test_sealed_defeater_contested(void)
{
    intern *sy = intern_new();
    uint32_t locked = intern_id(sy, "locked"), open = intern_id(sy, "open"),
             locked_p = intern_id(sy, "locked_p"), open_p = intern_id(sy, "open_p"),
             act = intern_id(sy, "act_open"), sealed = intern_id(sy, "sealed");

    dl_theory *t = dl_theory_new(sy);
    dl_lit l_now = dl_pos(locked), no_now = dl_neg(open),
           b_act = dl_pos(act), b_sealed = dl_pos(sealed);

    /* inertia (as world.c generates it) */
    int i_lp = dl_add_rule(t, "inertia+locked", DL_DEFEASIBLE,
                           dl_pos(locked_p), &l_now, 1);
    int i_no = dl_add_rule(t, "inertia-open", DL_DEFEASIBLE,
                           dl_neg(open_p), &no_now, 1);
    /* causal family for door=open, each effect superior to its inertia */
    int c_open = dl_add_rule(t, "open_door", DL_DEFEASIBLE,
                             dl_pos(open_p), &b_act, 1);
    int c_notlocked = dl_add_rule(t, "open_door_notlocked", DL_DEFEASIBLE,
                                  dl_neg(locked_p), &b_act, 1);
    dl_add_sup(t, c_open, i_no);
    dl_add_sup(t, c_notlocked, i_lp);
    /* the seal: value-specific defeater on open' */
    dl_add_rule(t, "sealed_blocks", DL_DEFEATER, dl_neg(open_p), &b_sealed, 1);

    dl_add_fact(t, dl_pos(locked));
    dl_add_fact(t, dl_neg(open));
    dl_add_fact(t, dl_pos(act));
    dl_add_fact(t, dl_pos(sealed));

    dl_result *res = dl_solve(t);
    /* open' blocked by the defeater... */
    CHECK(dl_defeasible(res, dl_pos(open_p)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(open_p)) == DL_REFUTED);
    /* ...but the shadow still kills locked': no value survives */
    CHECK(dl_defeasible(res, dl_pos(locked_p)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(locked_p)) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Sealed door, requires version -- the authoring pattern that works: the
 * seal is a body condition, the rule is inapplicable, the whole family
 * (shadows included) is inapplicable, and inertia keeps the door locked.
 * Unsealing restores the normal override. */
static int test_sealed_requires_holds(void)
{
    intern *sy = intern_new();
    uint32_t locked = intern_id(sy, "door_locked"),
             open = intern_id(sy, "door_open"),
             sealed = intern_id(sy, "door_sealed");
    uint32_t a_open = intern_id(sy, "act_open");

    world *w = world_new(sy);
    world_declare_fluent(w, locked);
    world_declare_fluent(w, open);
    world_declare_fluent(w, sealed);
    world_set(w, locked, true);
    world_set(w, sealed, true);

    step_cond not_sealed = { { sealed, true }, false };   /* ~sealed (now) */
    dl_lit to_open[] = { dl_pos(open), dl_neg(locked) };
    world_add_step_rule(w, "open_door", a_open, &not_sealed, 1, to_open, 2);

    char err[256];
    CHECK(world_step(w, &a_open, 1, err, sizeof err) == 0);
    CHECK(world_get(w, locked) && !world_get(w, open));   /* inertia held */

    world_set(w, sealed, false);
    CHECK(world_step(w, &a_open, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, locked) && world_get(w, open));

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_strict_team_contested()) return 1;
    if (test_bands_resolve()) return 1;
    if (test_flipflop_step()) return 1;
    if (test_boolean_degeneration()) return 1;
    if (test_sealed_defeater_contested()) return 1;
    if (test_sealed_requires_holds()) return 1;
    printf("test_multival: all passed\n");
    return 0;
}
