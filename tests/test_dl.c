/* Golden tests for the defeasible logic engine (DESIGN.md 5.1). */

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

/* Tweety: birds normally fly; penguins are birds; penguins don't fly,
 * and the penguin rule beats the bird rule. */
static int test_tweety(void)
{
    intern *sy = intern_new();
    uint32_t bird = intern_id(sy, "bird"), penguin = intern_id(sy, "penguin"),
             flies = intern_id(sy, "flies");

    dl_theory *t = dl_theory_new(sy);
    dl_lit b = dl_pos(bird), p = dl_pos(penguin);
    int r_bird = dl_add_rule(t, "birds_fly", DL_DEFEASIBLE, dl_pos(flies), &b, 1);
    int r_peng = dl_add_rule(t, "penguins_dont", DL_DEFEASIBLE, dl_neg(flies), &p, 1);
    dl_add_rule(t, "penguins_are_birds", DL_STRICT, dl_pos(bird), &p, 1);
    dl_add_sup(t, r_peng, r_bird);
    dl_add_fact(t, dl_pos(penguin));

    dl_result *res = dl_solve(t);
    CHECK(dl_definite(res, dl_pos(bird)) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_pos(flies)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(flies)) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);

    /* a plain bird flies */
    t = dl_theory_new(sy);
    r_bird = dl_add_rule(t, "birds_fly", DL_DEFEASIBLE, dl_pos(flies), &b, 1);
    r_peng = dl_add_rule(t, "penguins_dont", DL_DEFEASIBLE, dl_neg(flies), &p, 1);
    dl_add_sup(t, r_peng, r_bird);
    dl_add_fact(t, dl_pos(bird));

    res = dl_solve(t);
    CHECK(dl_defeasible(res, dl_pos(flies)) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Defeater: antidote blocks 'weakened' without proving ~weakened. */
static int test_defeater(void)
{
    intern *sy = intern_new();
    uint32_t poisoned = intern_id(sy, "poisoned"),
             antidote = intern_id(sy, "antidote"),
             weakened = intern_id(sy, "weakened");

    dl_theory *t = dl_theory_new(sy);
    dl_lit po = dl_pos(poisoned), an = dl_pos(antidote);
    dl_add_rule(t, "poison_weakens", DL_DEFEASIBLE, dl_pos(weakened), &po, 1);
    dl_add_rule(t, "antidote_blocks", DL_DEFEATER, dl_neg(weakened), &an, 1);
    dl_add_fact(t, dl_pos(poisoned));
    dl_add_fact(t, dl_pos(antidote));

    dl_result *res = dl_solve(t);
    CHECK(dl_defeasible(res, dl_pos(weakened)) == DL_REFUTED);
    /* a defeater blocks; it must NOT prove the opposite */
    CHECK(dl_defeasible(res, dl_neg(weakened)) == DL_REFUTED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Conflict without superiority: neither side provable (ambiguity blocked). */
static int test_unresolved_conflict(void)
{
    intern *sy = intern_new();
    uint32_t a = intern_id(sy, "a"), b = intern_id(sy, "b"),
             q = intern_id(sy, "q");

    dl_theory *t = dl_theory_new(sy);
    dl_lit la = dl_pos(a), lb = dl_pos(b);
    dl_add_rule(t, "for", DL_DEFEASIBLE, dl_pos(q), &la, 1);
    dl_add_rule(t, "against", DL_DEFEASIBLE, dl_neg(q), &lb, 1);
    dl_add_fact(t, dl_pos(a));
    dl_add_fact(t, dl_pos(b));

    dl_result *res = dl_solve(t);
    CHECK(dl_defeasible(res, dl_pos(q)) == DL_REFUTED);
    CHECK(dl_defeasible(res, dl_neg(q)) == DL_REFUTED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* Strict conclusions are immune to defeasible attack. */
static int test_strict_wins(void)
{
    intern *sy = intern_new();
    uint32_t f = intern_id(sy, "f"), g = intern_id(sy, "g"),
             q = intern_id(sy, "q");

    dl_theory *t = dl_theory_new(sy);
    dl_lit lf = dl_pos(f), lg = dl_pos(g);
    dl_add_rule(t, "strict_q", DL_STRICT, dl_pos(q), &lf, 1);
    dl_add_rule(t, "def_not_q", DL_DEFEASIBLE, dl_neg(q), &lg, 1);
    dl_add_fact(t, dl_pos(f));
    dl_add_fact(t, dl_pos(g));

    dl_result *res = dl_solve(t);
    CHECK(dl_definite(res, dl_pos(q)) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_pos(q)) == DL_PROVED);
    CHECK(dl_defeasible(res, dl_neg(q)) == DL_REFUTED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* The two solve drivers must agree on every literal, on a theory that
 * exercises facts, chains, a defeater, and superiority (team defeat). */
static int test_drivers_agree(void)
{
    intern *sy = intern_new();
    enum { N = 40 };
    uint32_t a[N];
    char buf[16];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "a%d", i);
        a[i] = intern_id(sy, buf);
    }

    dl_theory *t = dl_theory_new(sy);
    dl_add_fact(t, dl_pos(a[0]));
    dl_add_fact(t, dl_pos(a[1]));
    for (int i = 2; i < N; i++) {
        /* a[i] normally follows from a[i-2]; a competing rule from a[i-1]
         * argues the negation and is beaten by the supporting rule. */
        dl_lit sup_body = dl_pos(a[i - 2]);
        snprintf(buf, sizeof buf, "for%d", i);
        int rf = dl_add_rule(t, buf, DL_DEFEASIBLE, dl_pos(a[i]), &sup_body, 1);
        dl_lit att_body = dl_pos(a[i - 1]);
        snprintf(buf, sizeof buf, "against%d", i);
        int ra = dl_add_rule(t, buf, DL_DEFEASIBLE, dl_neg(a[i]), &att_body, 1);
        if (i % 2 == 0)
            dl_add_sup(t, rf, ra);   /* superiority only on some, to vary status */
    }
    /* a lone defeater with no supporting rule, to cover that path */
    dl_lit d = dl_pos(a[3]);
    dl_add_rule(t, "block", DL_DEFEATER, dl_neg(a[0]), &d, 1);

    dl_result *s = dl_solve(t);
    dl_result *w = dl_solve_wl(t);
    for (uint32_t id = 0; id < intern_count(sy); id++) {
        for (int neg = 0; neg < 2; neg++) {
            dl_lit q = neg ? dl_neg(id) : dl_pos(id);
            CHECK(dl_definite(s, q) == dl_definite(w, q));
            CHECK(dl_defeasible(s, q) == dl_defeasible(w, q));
        }
    }
    dl_result_free(s);
    dl_result_free(w);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_tweety()) return 1;
    if (test_defeater()) return 1;
    if (test_unresolved_conflict()) return 1;
    if (test_strict_wins()) return 1;
    if (test_drivers_agree()) return 1;
    printf("test_dl: all passed\n");
    return 0;
}
