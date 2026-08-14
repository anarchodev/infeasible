#ifndef INF_STATE_NUMAXIS_H
#define INF_STATE_NUMAXIS_H

#include <stdint.h>

#include "state/world.h"

/* Ordered-axis index over the numeric value store (DESIGN.md §8.3, #247).
 *
 * A difference bound — `x(A) - x(B) <= r` — is an ACCESS PATH, not just a test.
 * Carried as a filter it prunes only after every pair has been formed, so the
 * rule grounds the sort cross product; used as a generator it forms only the
 * pairs that can satisfy it. This is the structure that generates them.
 *
 * INDEX EVERY AXIS THE BOUND MENTIONS, not one. That is the measured decision
 * (§8.3, `bench_dbound`): with a single sorted axis the answer gets sparser
 * with each added dimension while the work does not move, because the band is
 * chosen before the other axes are consulted. Bucketing on all of them makes
 * the pairs examined equal the pairs that match.
 *
 * CONSERVATIVE, NEVER LOSSY. A superset is correct — the bound remains an
 * ordinary body conjunct that the rule re-checks, so an extra pair costs a
 * comparison. A DROPPED pair is a silently missing rule instance, which no
 * later stage can recover. The asymmetry is the whole safety argument, and
 * test_numaxis asserts set EQUALITY against an exhaustive filter rather than
 * mere containment.
 *
 * CANONICAL ORDER (I4). Pairs are emitted ascending by (low lane, high lane),
 * a function of the values and the lane numbering alone — never of bucket
 * iteration or allocation order. Grounding order sets roll-site indices and
 * lane assignment, so this is semantics rather than tidiness.
 *
 * REBUILD, DON'T MAINTAIN (this slice). Entities move every tick and a tick
 * runs one bulk enumeration, so the build is a sort — the shape bench_slice's
 * host grid already uses — not a tree that rebalances badly under motion.
 * Incremental maintenance is a later #246 slice. */

typedef struct numaxis numaxis;

enum { NUMAXIS_MAXAXES = 4 };   /* a rule bounds 2-4 axes; past that §8.3 says
                                 * one selective axis plus a filter wins */

/* Build over the CURRENT values of `nent` lanes on `naxis` axes.
 * `atom_cell[a * nent + e]` is the ground numeric atom lane e compares on axis
 * a (0 = this lane has no value on this axis, reading 0 — the closed-world
 * analogue of world_get_num on an undeclared atom). `radius[a]` is that axis's
 * band half-width; 0 means "equal on this axis". Returns NULL if naxis is out
 * of range. Values are read once, here: the index is a snapshot, invalid the
 * moment the store moves. */
numaxis *numaxis_build(const world *w, int naxis, int nent,
                       const uint32_t *atom_cell, const long *radius);
void     numaxis_free(numaxis *ix);

/* Enumerate the unordered lane pairs within the band on EVERY axis. Writes
 * 2*n lane indices (low, high) into `out` and returns the total number of
 * pairs — which may exceed `cap`, so a caller can size a buffer by calling
 * with cap 0 first. */
long numaxis_pairs(const numaxis *ix, uint32_t *out, long cap);

/* Pairs examined during the last numaxis_pairs call — the separability metric
 * (§8.3). With every bound axis indexed this equals the pair count returned;
 * a gap is the index doing work in vain. */
long numaxis_probes(const numaxis *ix);

#endif
