/* Golden tests for the numeric-fluent boundary (DESIGN.md 5.8), pinned
 * for the parts expressible without a value store: guard atoms as strict
 * closed-world inputs, and compiler-generated entailment between ordered
 * thresholds.
 *
 * The value store does not exist yet, so the "provider" here is the test
 * itself: it buckets a notional hp value against the harvested thresholds
 * (hp <= 0, hp < 10) and asserts the guard atoms as facts, both
 * polarities, exactly as 5.8 specifies.
 *
 * NOT expressible until the M1 value store lands (listed so the gap is
 * visible, DESIGN.md 5.8 golden tests):
 *   - effect pipeline: two deltas on one tick sum; := competes as a value
 *     conclusion; winning := replaces the base with deltas still applied;
 *     declared-range clamp as the outermost stage; receipt-shaped trace;
 *   - dying trigger: primed numeric guard concluding dead' in the same
 *     step, cascading into a ramification (stratified);
 *   - heal/curse oscillator rejected at compile time naming the cycle;
 *   - chip damage crossing no threshold wakes no rules (I3). */

#include "logic/dl.h"
#include "core/intern.h"

#include <stdio.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* One theory per hp bucket. Thresholds harvested from the rules:
 * hp <= 0 (le0) and hp < 10 (lt10); generated strict entailment
 * le0 -> lt10; judgments: le0 => down, lt10 => bloodied. */
static dl_theory *make_theory(intern *sy, uint32_t le0, uint32_t lt10,
                              uint32_t down, uint32_t bloodied)
{
    dl_theory *t = dl_theory_new(sy);
    dl_lit b_le0 = dl_pos(le0), b_lt10 = dl_pos(lt10);
    dl_add_rule(t, "gen_le0_implies_lt10", DL_STRICT, dl_pos(lt10), &b_le0, 1);
    dl_add_rule(t, "dying", DL_DEFEASIBLE, dl_pos(down), &b_le0, 1);
    dl_add_rule(t, "bloodied", DL_DEFEASIBLE, dl_pos(bloodied), &b_lt10, 1);
    return t;
}

/* The generated entailment chain: asserting only the finest true
 * threshold (hp = 0 -> le0) strictly derives the coarser one, and both
 * judgments fire. "At 0 you are also bloodied" is free. */
static int test_threshold_entailment(void)
{
    intern *sy = intern_new();
    uint32_t le0 = intern_id(sy, "hp_le_0"), lt10 = intern_id(sy, "hp_lt_10"),
             down = intern_id(sy, "down"), bloodied = intern_id(sy, "bloodied");

    dl_theory *t = make_theory(sy, le0, lt10, down, bloodied);
    dl_add_fact(t, dl_pos(le0));

    dl_result *res = dl_solve(t);
    CHECK(dl_definite(res, dl_pos(lt10)) == DL_PROVED);   /* strict chain */
    CHECK(dl_defeasible(res, dl_pos(down)) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_pos(bloodied)) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Guard atoms are closed-world strict inputs: the provider asserts every
 * threshold's polarity each evaluation, so guards never sit UNDECIDED and
 * the judgments track the bucket exactly. */
static int test_guard_buckets(void)
{
    intern *sy = intern_new();
    uint32_t le0 = intern_id(sy, "hp_le_0"), lt10 = intern_id(sy, "hp_lt_10"),
             down = intern_id(sy, "down"), bloodied = intern_id(sy, "bloodied");

    /* hp = 7: not le0, lt10 -- bloodied but standing */
    dl_theory *t = make_theory(sy, le0, lt10, down, bloodied);
    dl_add_fact(t, dl_neg(le0));
    dl_add_fact(t, dl_pos(lt10));
    dl_result *res = dl_solve(t);
    CHECK(dl_definite(res, dl_pos(le0)) == DL_REFUTED);   /* never UNDECIDED */
    CHECK(dl_defeasible(res, dl_pos(down)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_pos(bloodied)) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);

    /* hp = 15: neither threshold -- healthy */
    t = make_theory(sy, le0, lt10, down, bloodied);
    dl_add_fact(t, dl_neg(le0));
    dl_add_fact(t, dl_neg(lt10));
    res = dl_solve(t);
    CHECK(dl_defeasible(res, dl_pos(down)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_pos(bloodied)) == DL_REFUTED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_threshold_entailment()) return 1;
    if (test_guard_buckets()) return 1;
    printf("test_landmark: all passed\n");
    return 0;
}
