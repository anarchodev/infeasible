#ifndef INF_STOCK_GRID_H
#define INF_STOCK_GRID_H

#include <stdbool.h>
#include <stdint.h>

#include "core/intern.h"
#include "state/world.h"

/* The stock grid provider (DESIGN.md §5.6, #255) — the first member of
 * `src/stock/`, the platform's provider library, in two topologies.
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
 * VOCABULARY, by convention. A square story declares:
 *
 *     state ( grid_x(actor) : int  grid_y(actor) : int  grid_blocks(actor) )
 *     provider ( grid_adjacent(actor, actor)  grid_los(actor, actor) )
 *
 * a hex one the same shape over axial coordinates:
 *
 *     state ( hex_q(actor) : int  hex_r(actor) : int  hex_blocks(actor) )
 *     provider ( hex_adjacent(actor, actor)  hex_los(actor, actor) )
 *
 * §5.6 says hex vs. square is just the neighbour function inside the provider,
 * and that is literally what it is here: ONE index, one ray walk, one
 * generator, and a topology that answers two questions — which cells are one
 * step away, and how far apart two cells are. What the two vocabularies buy is
 * that a story cannot mix them by accident, since `hex_adjacent` over
 * `grid_x`/`grid_y` positions would read every entity at the origin. The
 * compiler refuses that (#263, the `reads` column of the roster), and so does
 * `stock_grid_install`, which returns NULL rather than answer from nothing.
 *
 * `grid_adjacent` is Chebyshev distance <= 1 between two DIFFERENT entities,
 * so a diagonal counts and two on the same cell count — the melee-reach
 * reading, not the strictly-one-away one. `hex_adjacent` is the same reading
 * at hex distance <= 1. An entity is never adjacent to itself.
 *
 * Positions are ordinary numeric fluents, so movement is `grid_x(A) += 1` — an
 * ordinary effect. No geometry function is needed for it, which is why the
 * `neighbor(cell, dir)` shape of `patrol.story`'s opaque cell domain has no
 * counterpart here.
 *
 * MEASUREMENTS (#258). §5.6's rule is that a stock provider returns the
 * smallest measurement that still admits a ruling — `chebyshev`, not
 * `in_range` — so the thresholds and their exceptions stay in the story where
 * `why?` can reach them. A story writes:
 *
 *     function ( grid_chebyshev(actor, actor) : int
 *                grid_manhattan(actor, actor) : int
 *                grid_occlusion(actor, actor) : int )
 *
 *     rule near(A: actor, B: actor):
 *         grid_chebyshev(A, B) <= 3  => in_shout(A, B)
 *
 *     rule half(A: actor, T: actor):
 *         grid_occlusion(A, T) >= 50 => cover_bonus(T)
 *
 * and owns the number. `grid_occlusion` is the cover measurement the same rule
 * applies to: what PERCENT of the target's outline a blocker hides, counted by
 * casting one ray at each corner of the target's cell (four on a square, six
 * on a hex) and asking how many of them a `grid_blocks` entity intercepts.
 * "Half cover at 50, three-quarters at 75, unless Sharpshooter" is then a line
 * of content, which is the whole reason the library does not ship `has_cover`.
 *
 * An entity the grid has never heard of measures as INFINITELY far rather than
 * -1: a guard reads `<= n`, so a negative "undefined" would make an unknown
 * entity close to everything. It is likewise fully occluded rather than 0%,
 * since a guard there reads `>= n` and an unknown entity should not be the one
 * everybody can see. */

typedef struct stock_grid stock_grid;

/* Install on `w` over the given entities, reading the topology's position
 * fluents for each — `grid_x`/`grid_y` for the square, `hex_q`/`hex_r` for the
 * hex. The caller supplies the entity set because the PLATFORM knows it (the
 * §6.3 interface artifact lists every sort's entities) and the provider should
 * not guess at discovery. Returns NULL if the world declares no position for
 * any of them, which is the only honest answer: reading an undeclared numeric
 * fluent yields 0, and a grid that puts the whole cast on one cell reports
 * everyone adjacent to everyone.
 *
 * One topology per world: both installs claim the world's provider callback,
 * so the second would displace the first.
 *
 * The handle owns the index and must outlive the world. Positions are re-read
 * when the world's tick counter moves, so a step's providers see that step's
 * pre-commit state and a host that edits positions mid-tick calls
 * stock_grid_refresh itself. */
stock_grid *stock_grid_install(world *w, intern *syms,
                               const uint32_t *ents, int nent);
stock_grid *stock_hex_install(world *w, intern *syms,
                              const uint32_t *ents, int nent);
void        stock_grid_free(stock_grid *g);
void        stock_grid_refresh(stock_grid *g);

/* The measurements, available to C hosts now and to stories when a value
 * provider can be called with entity arguments. Chebyshev is the square grid's
 * distance (a diagonal step costs one); Manhattan is the four-way one; both
 * are square-only. `stock_grid_distance` is whichever one the installed
 * topology calls its own — Chebyshev on a square, hex distance on a hex — and
 * is what `grid_adjacent`/`hex_adjacent` threshold at 1. */
int stock_grid_chebyshev(stock_grid *g, uint32_t a, uint32_t b);
int stock_grid_manhattan(stock_grid *g, uint32_t a, uint32_t b);
int stock_grid_distance(stock_grid *g, uint32_t a, uint32_t b);

/* Percent of `b`'s outline that blockers hide from `a` (0..100), and whether
 * the centre-to-centre line is clear. `stock_grid_los` is the strictly cheaper
 * question and is not the same as `occlusion < 100`: a wall can cover every
 * corner ray while leaving the centre line open, and a narrow pillar can do
 * the reverse. Both exclude the two endpoints' own cells. */
int  stock_grid_occlusion(stock_grid *g, uint32_t a, uint32_t b);
bool stock_grid_los(stock_grid *g, uint32_t a, uint32_t b);

/* Cells examined by the last generator run — the separability metric (§8.3).
 * With the bucket index this tracks the answer size, not the population. */
long stock_grid_probes(const stock_grid *g);

#endif
