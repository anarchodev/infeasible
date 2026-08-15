#ifndef INF_STOCK_GRID_H
#define INF_STOCK_GRID_H

#include <stdbool.h>
#include <stdint.h>

#include "core/intern.h"
#include "state/world.h"

/* The stock square-grid provider (DESIGN.md §5.6, #255) — the first member of
 * `src/stock/`, the platform's provider library.
 *
 * A `.story` with no host code cannot express "next to" anything: a provider
 * needs someone to answer it, so `cellar_pure.story` and `duel_pure.story` use
 * none at all and have no geometry available to them. Their alternatives are a
 * cross product the grounder refuses past 2^20 instances, or writing host code
 * — which is exactly what a pure cart exists to avoid. A STOCK provider is the
 * answer: shipped with the engine, identical for every cart, so a hostless
 * story declares it and uses it.
 *
 * It is not part of the inference. The engine learns no geometry; this reads
 * ordinary state and answers a relation, so §5.2 and §5.6 are untouched. And
 * because it derives from facts the engine owns, it costs nothing to replicate
 * — every peer in lockstep re-derives it rather than shipping it.
 *
 * VOCABULARY, by convention. The story declares:
 *
 *     state ( grid_x(actor) : int  grid_y(actor) : int  grid_blocks(actor) )
 *     provider ( grid_adjacent(actor, actor)  grid_los(actor, actor) )
 *
 * `grid_adjacent` is Chebyshev distance <= 1 between two DIFFERENT entities,
 * so a diagonal counts and two on the same cell count — the melee-reach
 * reading, not the strictly-one-away one. An entity is never adjacent to
 * itself.
 *
 * `grid_x`/`grid_y` are ordinary numeric fluents, so movement is
 * `grid_x(A) += 1` — an ordinary effect. No geometry function is needed for
 * it, which is why the `neighbor(cell, dir)` shape of `patrol.story`'s opaque
 * cell domain has no counterpart here.
 *
 * MEASUREMENTS (#258). §5.6's rule is that a stock provider returns the
 * smallest measurement that still admits a ruling — `chebyshev`, not
 * `in_range` — so the thresholds and their exceptions stay in the story where
 * `why?` can reach them. A story writes:
 *
 *     function ( grid_chebyshev(actor, actor) : int
 *                grid_manhattan(actor, actor) : int )
 *
 *     rule near(A: actor, B: actor):
 *         grid_chebyshev(A, B) <= 3  => in_shout(A, B)
 *
 * and owns the number. An entity the grid has never heard of measures as
 * INFINITELY far rather than -1: a guard reads `<= n`, so a negative
 * "undefined" would make an unknown entity close to everything. */

typedef struct stock_grid stock_grid;

/* Install on `w` over the given entities, reading `grid_x`/`grid_y` for each.
 * The caller supplies the entity set because the PLATFORM knows it (the §6.3
 * interface artifact lists every sort's entities) and the provider should not
 * guess at discovery. Returns NULL if no entity carries a position.
 *
 * The handle owns the index and must outlive the world. Positions are re-read
 * when the world's tick counter moves, so a step's providers see that step's
 * pre-commit state and a host that edits positions mid-tick calls
 * stock_grid_refresh itself. */
stock_grid *stock_grid_install(world *w, intern *syms,
                               const uint32_t *ents, int nent);
void        stock_grid_free(stock_grid *g);
void        stock_grid_refresh(stock_grid *g);

/* The measurements, available to C hosts now and to stories when a value
 * provider can be called with entity arguments. Chebyshev is the square grid's
 * distance (a diagonal step costs one); Manhattan is the four-way one. */
int stock_grid_chebyshev(stock_grid *g, uint32_t a, uint32_t b);
int stock_grid_manhattan(stock_grid *g, uint32_t a, uint32_t b);

/* Cells examined by the last generator run — the separability metric (§8.3).
 * With the bucket index this tracks the answer size, not the population. */
long stock_grid_probes(const stock_grid *g);

#endif
