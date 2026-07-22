#ifndef INF_LOGIC_DL_TRACE_H
#define INF_LOGIC_DL_TRACE_H

#include <stdbool.h>
#include <stdio.h>

#include "logic/dl.h"

/* One why-trace renderer, shared by both backings (the scalar `dl_result` and
 * the columnar family). The proof/defeat structure — rules for/against a query,
 * body statuses, applicability, superiority — is identical across backings; only
 * the accessors differ (a family tags names with an entity, reads columns
 * instead of the permuted arrays). Each backing supplies this vtable over an
 * opaque ctx and `dl_why`/`dlcol_why` become thin wrappers. The format lives
 * here, once, so a trace change (e.g. provenance, §6.3) lands in one place — and
 * the two backings stay byte-for-byte identical by construction, not by a pin
 * that must be maintained (tests/test_col.c still checks it).
 *
 * `dl_lit` is opaque to the renderer: it is only ever passed back into the
 * vtable, never inspected, so the family's location-space literals and the
 * scalar atom-space literals both flow through unchanged. A "rule" is an integer
 * handle the backing understands (a permuted index for the scalar result, a rule
 * id for the family); the renderer only round-trips it through the accessors. */

typedef struct {
    /* print literal l as "~name"/"name" (a family appends an entity tag) */
    void (*put_lit)(void *ctx, dl_lit l, FILE *out);
    /* print rule r's label (a family appends an entity tag) */
    void (*put_rule)(void *ctx, int r, FILE *out);

    dl_verdict (*definite)(void *ctx, dl_lit l);
    dl_verdict (*defeasible)(void *ctx, dl_lit l);
    bool       (*is_fact)(void *ctx, dl_lit l);
    bool       (*solved)(void *ctx);          /* false -> "(… not solved)" */

    int  (*nhead)(void *ctx, dl_lit l);       /* # rules whose head is l   */
    int  (*head_at)(void *ctx, dl_lit l, int i); /* i-th such rule handle  */

    dl_rule_kind (*rule_kind)(void *ctx, int r);
    int    (*nbody)(void *ctx, int r);
    dl_lit (*body_at)(void *ctx, int r, int i);
    int    (*applicable)(void *ctx, int r);   /* +1 / -1 / 0 (tri-valued)  */
    bool   (*beats)(void *ctx, int winner, int loser);

    /* provenance suffix for rule r (source span + generation reason, §6.3), or
     * NULL for none. The whole pointer may be NULL if the backing carries no
     * provenance. */
    const char *(*rule_prov)(void *ctx, int r);
} dl_trace_vtbl;

void dl_trace_render(const dl_trace_vtbl *v, void *ctx, dl_lit q, FILE *out);

#endif
