#ifndef INF_STATE_FACTINDEX_H
#define INF_STATE_FACTINDEX_H

#include <stdint.h>
#include <stdbool.h>

/* Fact-store extension index (§5.2 item 4; EPIC #26 / the semi-naïve matcher,
 * #28). The INVERSE of the world's atom -> "is it true?" map: a predicate maps
 * to the set of its currently-true ground argument tuples, ENUMERABLE and
 * probeable by a bound-argument pattern.
 *
 * This is the structure the join matcher iterates instead of the sort cross
 * product: a rule `adj(X,Y) & cond(X) => d(X,Y)` scans `adj`'s extension (the
 * handful of true facts) rather than the N² pairs the sorts allow — turning the
 * eager Nᵏ grounding bench_ground measured into cost ∝ actual matches. The
 * planner (#28) orders body atoms by factindex_count (selectivity) and drives
 * each with a cursor whose filtered positions come from already-bound vars.
 *
 * Enumeration is in INSERTION ORDER — deterministic, so replay is exact (I4).
 * The caller supplies each true tuple once per predicate (the closed-world fact
 * store already holds each fluent at a single value), so add is an O(1) append
 * and does not dedup. */

#define FACTINDEX_MAXARGS 6      /* mirrors story.c MAX_ARGS */

typedef struct factindex factindex;

factindex *factindex_new(void);
void       factindex_free(factindex *ix);

/* Record a true ground tuple pred(args[0..nargs)). nargs in [0, FACTINDEX_MAXARGS];
 * a 0-arity predicate (a plain proposition) records a single empty tuple. */
void       factindex_add(factindex *ix, uint32_t pred,
                         const uint32_t *args, int nargs);

/* Remove one occurrence of pred(args) — the incremental counterpart of add, so
 * the index is maintained per fact change instead of rebuilt (#73). Swap-remove:
 * O(1), deterministic by add/remove sequence (I4). Returns whether it was present. */
bool       factindex_remove(factindex *ix, uint32_t pred,
                            const uint32_t *args, int nargs);

/* Drop every tuple but keep the allocation, so the index can be refilled each
 * tick without realloc churn. */
void       factindex_clear(factindex *ix);

/* Number of true tuples for `pred` (0 if absent) — the selectivity the join
 * planner orders body atoms by. */
int        factindex_count(const factindex *ix, uint32_t pred);

/* Cursor over one predicate's extension, optionally filtered to a bound pattern.
 * Treat as opaque; fields are exposed only so callers can stack-allocate it. */
typedef struct {
    const factindex *ix;
    int      bucket;                          /* -1 once exhausted / absent */
    int      pos;                             /* next tuple slot to consider */
    int      nargs;
    bool     filtered[FACTINDEX_MAXARGS];
    uint32_t want[FACTINDEX_MAXARGS];
} factindex_cursor;

/* Begin a scan of `pred`'s extension. When `filter` is non-NULL, only tuples
 * whose arg i equals want[i] at every position with filter[i] set are yielded
 * (the matcher's "these vars are already bound" probe); NULL scans the whole
 * extension. `want` may be NULL iff `filter` is. */
void factindex_scan(const factindex *ix, uint32_t pred,
                    const bool *filter, const uint32_t *want,
                    factindex_cursor *cur);

/* Advance the cursor. On true, writes the matching tuple's args into
 * out[0..nargs) (out must hold FACTINDEX_MAXARGS); on false the scan is done. */
bool factindex_next(factindex_cursor *cur, uint32_t *out);

#endif
