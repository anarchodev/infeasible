#ifndef INF_STOCK_STOCK_H
#define INF_STOCK_STOCK_H

/* The stock provider ROSTER — what the platform can answer (#253, #263).
 *
 * A provider nobody answers reads closed-world false, forever and silently:
 * the rule never fires, the author sees no diagnostic, and the failure looks
 * exactly like a world where the relation happens not to hold. That was
 * defensible while a game could ship its own host to answer it. It cannot
 * (#253), so an unrecognised provider name now means the author named
 * something that does not exist, and the compiler says so.
 *
 * This is DATA, not behaviour: `lang` reads the names and arities to check a
 * declaration, and learns no geometry from them. Adding a provider to the
 * library is a line here beside its implementation.
 *
 * A story that genuinely wants a host-answered relation — an embedder with
 * its own topology, which §5.6's `patrol.story` demonstrates — says so with
 * `host provider`, and the §6.3 artifact publishes the fact so a runtime with
 * no host can refuse the story rather than run it with everything false. */

typedef struct {
    const char *name;
    int         arity;
    /* NULL for a boolean relation; the return sort for a value FUNCTION —
     * a measurement, per §5.6's rule that a stock provider returns the
     * smallest measurement that still admits a ruling. */
    const char *ret;
} stock_decl;

static const stock_decl STOCK_PROVIDERS[] = {
    /* square grid (src/stock/grid.c) — positions are `grid_x`/`grid_y`
     * numeric fluents the library reads by entity, so movement is an
     * ordinary effect and nothing here is a ruling. */
    { "grid_adjacent",  2, NULL  },   /* Chebyshev 1: a diagonal counts */
    { "grid_los",       2, NULL  },   /* no `grid_blocks` entity between them */
    { "grid_chebyshev", 2, "int" },   /* the distance a story thresholds */
    { "grid_manhattan", 2, "int" },
};
enum { STOCK_NPROVIDERS = (int)(sizeof STOCK_PROVIDERS / sizeof STOCK_PROVIDERS[0]) };

#endif
