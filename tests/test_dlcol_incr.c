/* Incremental dlcol maintenance (#66, EPIC #63): a family can be truncated to a
 * watermark and re-grown with a different rule suffix, and its atom columns can
 * grow on demand, WITHOUT rebuilding — and the result matches a from-scratch
 * build bit for bit. This is the primitive the tick-time matcher reuses to avoid
 * rebuilding the whole columnar family every re-ground. */

#include "logic/dl_col.h"
#include "logic/dl.h"

#include <stdio.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* a=0 b=1 c=2 d=3 e=4 ; one entity. */
static dl_lit P(uint32_t a) { return dl_pos(a); }
static int proved(dlcol *f, uint32_t a) { return dlcol_defeasible(f, P(a), 0) == DL_PROVED; }

int main(void)
{
    dlcol *f = dlcol_new(4, 1);
    dlcol_add_fact(f, P(0), 0);                          /* fact: a */
    dl_lit ba[] = { P(0) };
    dlcol_add_rule(f, "b<=a", DL_DEFEASIBLE, P(1), ba, 1);  /* a => b (static) */

    /* watermark after the static rules */
    int wr = dlcol_rule_count(f), wb = dlcol_body_count(f), ws = dlcol_sup_count(f);

    /* matched suffix v1: a => c */
    dl_lit ca[] = { P(0) };
    dlcol_add_rule(f, "c<=a", DL_DEFEASIBLE, P(2), ca, 1);
    dlcol_solve(f);
    CHECK(proved(f, 1) && proved(f, 2));                 /* b, c */
    CHECK(!proved(f, 3));                                /* d: no rule */

    /* truncate + matched suffix v2: a => d (c's rule gone) */
    dlcol_truncate_rules(f, wr, wb, ws);
    dl_lit da[] = { P(0) };
    dlcol_add_rule(f, "d<=a", DL_DEFEASIBLE, P(3), da, 1);
    dlcol_solve(f);
    CHECK(proved(f, 1) && proved(f, 3));                 /* b, d */
    CHECK(!proved(f, 2));                                /* c: rule was truncated away */

    /* equals a from-scratch build of {b<=a, d<=a} with fact a */
    {
        dlcol *g = dlcol_new(4, 1);
        dlcol_add_fact(g, P(0), 0);
        dlcol_add_rule(g, "b<=a", DL_DEFEASIBLE, P(1), ba, 1);
        dlcol_add_rule(g, "d<=a", DL_DEFEASIBLE, P(3), da, 1);
        dlcol_solve(g);
        for (uint32_t a = 0; a < 4; a++)
            CHECK(proved(f, a) == proved(g, a));
        dlcol_free(g);
    }

    /* grow the atom columns and add a rule over a NEW atom; existing verdicts
     * (b, d) must be preserved and the new one (e) derived */
    dlcol_ensure_atoms(f, 5);
    dl_lit ea[] = { P(0) };
    dlcol_add_rule(f, "e<=a", DL_DEFEASIBLE, P(4), ea, 1);
    dlcol_solve(f);
    CHECK(proved(f, 1) && proved(f, 3) && proved(f, 4)); /* b, d, e */
    CHECK(!proved(f, 2));                                /* c still absent */

    dlcol_free(f);
    printf("test_dlcol_incr: all passed\n");
    return 0;
}
