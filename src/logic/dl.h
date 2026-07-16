#ifndef INF_LOGIC_DL_H
#define INF_LOGIC_DL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "core/intern.h"

/* Propositional defeasible logic engine.
 *
 * Standard (Billington / Antoniou-Billington-Governatori-Maher) semantics:
 * strict rules, defeasible rules, defeaters, a superiority relation,
 * ambiguity blocking, team defeat. Computes +/-Delta (definite) and
 * +/-Partial (defeasible) statuses for every literal.
 *
 * Scaffold implementation: tri-valued fixpoint over ground rules. Correct,
 * not yet Maher-linear (see DESIGN.md 5.2). Cyclic rule graphs may leave
 * literals UNDECIDED; the language compiler will reject cycles. */

typedef struct {
    uint32_t atom;   /* interned id */
    bool neg;
} dl_lit;

static inline dl_lit dl_pos(uint32_t atom) { dl_lit l = { atom, false }; return l; }
static inline dl_lit dl_neg(uint32_t atom) { dl_lit l = { atom, true  }; return l; }
static inline dl_lit dl_complement(dl_lit l) { l.neg = !l.neg; return l; }

typedef enum {
    DL_STRICT,      /* body -> head */
    DL_DEFEASIBLE,  /* body => head */
    DL_DEFEATER     /* body ~> head : can block ~head, never proves head */
} dl_rule_kind;

typedef enum {
    DL_UNDECIDED = 0,
    DL_PROVED,
    DL_REFUTED
} dl_verdict;

typedef struct dl_theory dl_theory;
typedef struct dl_result dl_result;

dl_theory *dl_theory_new(intern *syms);
void       dl_theory_free(dl_theory *t);

/* Returns a rule id usable in dl_add_sup. Name and body are copied. */
int  dl_add_rule(dl_theory *t, const char *name, dl_rule_kind kind,
                 dl_lit head, const dl_lit *body, int nbody);
void dl_add_sup(dl_theory *t, int winner, int loser);
void dl_add_fact(dl_theory *t, dl_lit fact);

/* Atoms must all be interned before solving (the result is sized to the
 * intern table at call time). */
dl_result *dl_solve(dl_theory *t);
void       dl_result_free(dl_result *r);

dl_verdict dl_definite(const dl_result *r, dl_lit q);    /* +Delta / -Delta */
dl_verdict dl_defeasible(const dl_result *r, dl_lit q);  /* +d / -d */

/* Human-readable proof/defeat trace for q. */
void dl_why(const dl_theory *t, const dl_result *r, dl_lit q, FILE *out);

#endif
