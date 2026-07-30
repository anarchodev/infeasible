/* Golden test for the #109 cycle rule (DESIGN.md §5.2, decided in PR #108):
 * recursion is Datalog; defeat does not recurse.
 *
 * Supported literals always proved through cycles (the fixpoint is monotone —
 * cycles stall, never oscillate); what was missing is the two halves pinned
 * here:
 *
 *  - COMPLETION: in a support-SCC that is cyclic and attacked NOWHERE (no
 *    rule of any kind concludes a member's complement), every member left
 *    underived by the fixpoint completes to REFUTED instead of stalling
 *    UNDECIDED — rule-authored recursive relations (transitive closure,
 *    contagion, command chains) become first-class. The why-trace renders
 *    the loop ("no support: every derivation … re-enters the cycle").
 *  - THE GATE: only loop-starvation may complete; conflict must stay loud.
 *    An attacked cycle is a located COMPILE error naming the loop and the
 *    attacker; a tied attack outside any cycle keeps its contested verdict
 *    (REFUTED both polarities under ambiguity blocking) with no loop line;
 *    an SCC reading an UNDECIDED out-of-SCC input (a #116 open partial-value
 *    guard) stays UNDECIDED — completion never consumes an undecided input.
 *
 *  Bonus regression this work exposed: a multi-var JOIN lane family used to
 *  claim world_query routing for its head atoms while carrying only its own
 *  rule — wrong whenever the pred had other rules (or was recursive). A join
 *  family now requires sole, non-recursive ownership of its head pred.
 *
 * Conservativity on acyclic theories is pinned by the whole existing suite;
 * scalar/columnar agreement by test_col's differential fuzz. */

#include "lang/story.h"
#include "state/world.h"
#include "logic/dl.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

static dl_verdict q(world *w, intern *sy, const char *atom)
{
    return world_query(w, (dl_lit){ intern_id(sy, atom), false });
}

static char *why_str(world *w, intern *sy, const char *atom)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(w, (dl_lit){ intern_id(sy, atom), false }, m);
    fclose(m);
    return buf;
}

/* a -> b -> c -> a, and d off to the side */
static const char *TC_SRC =
    "sort node\n"
    "entity ( a : node  b : node  c : node  d : node )\n"
    "state edge(node, node)\n"
    "init ( edge(a,b)  edge(b,c)  edge(c,a) )\n"
    "rule base(X: node, Y: node): edge(X,Y) => conn(X,Y)\n"
    "rule step(X: node, Z: node, Y: node): edge(X,Z) & conn(Z,Y) => conn(X,Y)\n";

/* --- transitive closure: reachable PROVED, unreachable REFUTED (not
 *     UNDECIDED — the pothole this rule removes), loop trace rendered --- */
static int test_transitive_closure(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(TC_SRC, sy);
    CHECK(w != NULL);

    CHECK(q(w, sy, "conn(a,b)") == DL_PROVED);
    CHECK(q(w, sy, "conn(a,c)") == DL_PROVED);
    CHECK(q(w, sy, "conn(a,a)") == DL_PROVED);     /* around the loop */
    CHECK(q(w, sy, "conn(a,d)") == DL_REFUTED);    /* completed, not stalled */
    CHECK(q(w, sy, "conn(d,a)") == DL_REFUTED);

    char *tr = why_str(w, sy, "conn(a,d)");
    CHECK(tr != NULL);
    CHECK(strstr(tr, "defeasible: REFUTED") != NULL);
    CHECK(strstr(tr, "re-enters the cycle") != NULL);
    free(tr);
    /* a PROVED literal renders no loop line */
    tr = why_str(w, sy, "conn(a,c)");
    CHECK(strstr(tr, "re-enters the cycle") == NULL);
    free(tr);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- an attacked cycle is a located compile error --- */
static int test_attacked_cycle_rejected(void)
{
    char src[1024];
    snprintf(src, sizeof src, "%s%s", TC_SRC,
             "state blocked(node)\n"
             "rule veto(X: node, Y: node): blocked(X) => ~conn(X,Y)\n");
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    CHECK(w == NULL && d.nerrors > 0);
    int hit = 0;
    for (int i = 0; i < d.count && i < d.cap; i++)
        if (strstr(di[i].msg, "defeat cannot reach through a cycle")) hit = 1;
    if (!hit)
        fprintf(stderr, "  got: %s\n", d.count ? di[0].msg : "");
    CHECK(hit);
    intern_free(sy);
    return 0;
}

/* --- a tied attack OUTSIDE any cycle is untouched by the completion: under
 *     ambiguity blocking a contested literal reads REFUTED on BOTH polarities
 *     (each side's uncountered attacker refutes it) — the #98 silently-
 *     REFUTED null, which must neither change nor gain a loop line --- */
static int test_tied_attack_stays(void)
{
    char src[1024];
    snprintf(src, sizeof src, "%s%s", TC_SRC,
             "state ( hot  cold )\n"
             "init ( hot  cold )\n"
             "rule w1: hot  => nice\n"
             "rule w2: cold => ~nice\n");
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "nice") == DL_REFUTED);         /* contested, untouched */
    CHECK(world_query(w, (dl_lit){ intern_id(sy, "nice"), true })
          == DL_REFUTED);                          /* … on both polarities */
    char *tr = why_str(w, sy, "nice");
    CHECK(strstr(tr, "re-enters the cycle") == NULL);   /* no loop line: this
                                                          * REFUTED is defeat,
                                                          * not completion */
    free(tr);
    CHECK(q(w, sy, "conn(a,d)") == DL_REFUTED);    /* the SCC still completes */
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- #116 interaction: an SCC gated on an OPEN partial-value guard stays
 *     UNDECIDED — completion never consumes an undecided input --- */
static int test_open_input_blocks_completion(void)
{
    static const char *src =
        "sort node\n"
        "entity ( a : node  b : node  c : node  d : node )\n"
        "state ( edge(node, node)  special(node) )\n"
        "init ( edge(a,b)  edge(b,c)  edge(c,a) )\n"
        "value pv(node) : int\n"
        "rule pd(X: node): special(X) => pv(X) = 1\n"
        "rule base(X: node, Y: node): edge(X,Y) & pv(X) >= 0 => conn(X,Y)\n"
        "rule step(X: node, Z: node, Y: node):\n"
        "    edge(X,Z) & conn(Z,Y) & pv(X) >= 0 => conn(X,Y)\n";
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    /* pv is partial and defined nowhere (no `special` node): every guard is
     * UNDECIDED, so both the acyclic pairs AND the would-complete cyclic
     * SCC stay honestly UNDECIDED */
    CHECK(q(w, sy, "conn(a,b)") == DL_UNDECIDED);
    CHECK(q(w, sy, "conn(a,d)") == DL_UNDECIDED);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the scalar engine, strict layer: a defeat-free strict cycle completes
 *     -Delta (and so -d); with a fact beneath it, it proves — the Datalog
 *     least fixpoint both ways --- */
static int test_scalar_strict_cycle(void)
{
    intern *sy = intern_new();
    uint32_t p = intern_id(sy, "p"), r = intern_id(sy, "r");
    dl_theory *t = dl_theory_new(sy);
    dl_lit lp = dl_pos(p), lr = dl_pos(r);
    dl_add_rule(t, "pq", DL_STRICT, lp, &lr, 1);
    dl_add_rule(t, "qp", DL_STRICT, lr, &lp, 1);
    dl_result *res = dl_solve(t);
    CHECK(dl_definite(res, lp) == DL_REFUTED);
    CHECK(dl_defeasible(res, lp) == DL_REFUTED);
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    dl_why(t, res, lp, m);
    fclose(m);
    CHECK(strstr(buf, "re-enters the cycle") != NULL);
    free(buf);
    dl_result_free(res);
    dl_theory_free(t);

    /* with support beneath, the same cycle proves through */
    t = dl_theory_new(sy);
    dl_add_rule(t, "pq", DL_STRICT, lp, &lr, 1);
    dl_add_rule(t, "qp", DL_STRICT, lr, &lp, 1);
    dl_add_fact(t, lr);
    res = dl_solve(t);
    CHECK(dl_definite(res, lp) == DL_PROVED);
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    return 0;
}

/* --- the join-lane ownership regression (pre-existing, exposed by #109):
 *     two 2-var rules concluding one pred — the lane route must not shadow
 *     the second rule's contribution --- */
static int test_join_lane_ownership(void)
{
    static const char *src =
        "sort node\n"
        "entity ( a : node  b : node )\n"
        "state edge(node, node)\n"
        "init ( edge(a,b) )\n"
        "rule r1(X: node, Y: node): edge(X,Y) => conn(X,Y)\n"
        "rule r2(X: node, Y: node): edge(Y,X) => conn(X,Y)\n";
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    CHECK(q(w, sy, "conn(a,b)") == DL_PROVED);     /* via r1 */
    CHECK(q(w, sy, "conn(b,a)") == DL_PROVED);     /* via r2 — was REFUTED */
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_transitive_closure()) return 1;
    if (test_attacked_cycle_rejected()) return 1;
    if (test_tied_attack_stays()) return 1;
    if (test_open_input_blocks_completion()) return 1;
    if (test_scalar_strict_cycle()) return 1;
    if (test_join_lane_ownership()) return 1;
    printf("test_cycle: all passed\n");
    return 0;
}
