#ifndef INF_LOGIC_DL_GRAPH_H
#define INF_LOGIC_DL_GRAPH_H

#include <stdint.h>

/* Graph machinery shared by the two solver backings (dl.c scalar, dl_col.c
 * columnar). Both condense literal graphs twice over: once for the §5.2 cycle
 * rule (the support graph, #109) and once for the evaluator's SCC schedule
 * (the dependency graph, §5.2 item 2). Same algorithm, four call sites — it
 * lives here so the two backings cannot drift, for the same reason the
 * why-trace format lives once in dl_trace.c. */

/* Iterative Tarjan over a CSR adjacency: n nodes, out-edges of v are
 * ato[aoff[v] .. aoff[v+1]). Writes each node's component id to comp[] (caller
 * allocates, n entries) and returns the component count.
 *
 * Numbering is REVERSE topological — a component closes only after every
 * component reachable from it — so an edge u -> v implies comp[u] >= comp[v].
 * Walking components in descending id order therefore visits each node after
 * the nodes it points at have settled. */
int dl_tarjan(int n, const int32_t *aoff, const int32_t *ato, int32_t *comp);

/* Group nodes by component id (counting sort): the members of component c are
 * (*lit)[(*off)[c] .. (*off)[c+1]). Allocates both arrays with malloc/calloc;
 * the caller owns them. `off` is sized ncomp+2 so the [c+1] read is always in
 * bounds. */
void dl_group_by_comp(int n, int ncomp, const int32_t *comp,
                      int32_t **off, int32_t **lit);

#endif
