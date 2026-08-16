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
    /* The POSITION state the library reads to answer it, NULL-padded. An
     * undeclared numeric fluent reads 0 (`world_get_num`), so a story that
     * declares `hex_adjacent` over `grid_x`/`grid_y` positions puts its whole
     * cast on one cell and every one of them is adjacent to every other —
     * #263's silent-always-false with the sign flipped, and two topologies is
     * what makes it reachable. So the compiler checks for these by name.
     *
     * The blocker flag (`grid_blocks`/`hex_blocks`) is deliberately NOT here:
     * an undeclared boolean reads false, which says "nothing blocks", and that
     * is a truthful reading of a story with no walls in it. */
    const char *reads[2];
} stock_decl;

static const stock_decl STOCK_PROVIDERS[] = {
    /* square grid (src/stock/grid.c) — positions are `grid_x`/`grid_y`
     * numeric fluents the library reads by entity, so movement is an
     * ordinary effect and nothing here is a ruling. */
    { "grid_adjacent",  2, NULL,  { "grid_x", "grid_y" } },  /* Chebyshev 1 */
    { "grid_los",       2, NULL,  { "grid_x", "grid_y" } },  /* the centre line */
    { "grid_chebyshev", 2, "int", { "grid_x", "grid_y" } },  /* the distance a
                                                              * story thresholds */
    { "grid_manhattan", 2, "int", { "grid_x", "grid_y" } },
    /* How much of the target a blocker hides, in percent — the measurement
     * under a cover ruling. There is no `has_cover` here on purpose: half at
     * 50 and three-quarters at 75 are 5e's numbers, not geometry's. */
    { "grid_occlusion", 2, "int", { "grid_x", "grid_y" } },

    /* hex grid — the same questions over axial coordinates `hex_q`/`hex_r`,
     * because a story should not lose a question by choosing a topology. */
    { "hex_adjacent",   2, NULL,  { "hex_q", "hex_r" } },
    { "hex_los",        2, NULL,  { "hex_q", "hex_r" } },
    { "hex_distance",   2, "int", { "hex_q", "hex_r" } },
    { "hex_occlusion",  2, "int", { "hex_q", "hex_r" } },
};
enum { STOCK_NPROVIDERS = (int)(sizeof STOCK_PROVIDERS / sizeof STOCK_PROVIDERS[0]) };

#endif
