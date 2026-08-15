#ifndef INF_LANG_DBOUND_H
#define INF_LANG_DBOUND_H

#include <stdbool.h>
#include <stdint.h>

#include "state/world.h"

/* Difference bounds, normalised (DESIGN.md §8.3, #249).
 *
 * A conjunct like `x(A) - x(B) <= 1` is an access path (§8.3): used as a
 * generator it forms only the pairs that can satisfy it, instead of filtering
 * a cross product after the fact. Before it can drive an index it has to be
 * reduced to one canonical shape, and that reduction is the part that bites —
 * five comparison operators, a constant that may sit on either side, and sign
 * flips that swap which variable is which.
 *
 * Canonical form is `val(hi) - val(lo) <= k`. Everything else rewrites into it:
 * a strict bound loses one (`d < c` is `d <= c-1`), a lower bound NEGATES (`d
 * >= c` is `-d <= -c`, so hi and lo trade places), and an equality is both
 * directions at once. A constant on the left mirrors the comparator.
 *
 * A single bound is a HALF-SPACE, not a band: `d <= 5` admits pairs arbitrarily
 * far apart in one direction, so no finite radius covers it and it cannot drive
 * a bucketed index. Only a pair covering both directions can, which is what
 * dbound_bands looks for. */

/* val(hi) - val(lo) <= k, for the numeric fluent `pred` read at arity 1. */
typedef struct { uint32_t pred, hi, lo; long k; } dbound;

/* |val(u) - val(v)| <= r, with r >= 0 — what numaxis can bucket on. The band a
 * pair of bounds describes may be OFFSET rather than centred (`5 <= d <= 10`),
 * and `r` is then the conservative radius covering it. That is a superset of
 * the true band, which is correct: the conjuncts stay in the rule body and are
 * re-checked, so a wider radius costs a comparison and never an answer. */
typedef struct { uint32_t pred, u, v; long r; } dbound_band;

/* Normalise one conjunct `(val(u) - val(v)) cmp c` — or `c cmp (val(u) -
 * val(v))` when `flipped` — into canonical bounds. Writes up to 2 (an equality
 * is both directions) and returns how many. */
int dbound_normalise(uint32_t pred, uint32_t u, uint32_t v, world_cmp cmp,
                     long c, bool flipped, dbound *out);

/* Pair opposing bounds into bands. Bounds on the same (pred, variable pair) in
 * the same direction keep the TIGHTEST k; a direction with no opposite yields
 * no band; a pair whose bounds cannot both hold (k1 + k2 < 0, an unsatisfiable
 * conjunction) yields none either — a caller wanting to reject the rule should
 * ask dbound_unsat. Returns the number of bands written, capped at `cap`. */
int dbound_bands(const dbound *in, int n, dbound_band *out, int cap);

/* True if `in` contains an unsatisfiable opposing pair — `d <= k1` with
 * `d >= -k2` where k1 + k2 < 0. Such a rule can never fire; separated from
 * dbound_bands so the caller decides between a diagnostic and silence. */
bool dbound_unsat(const dbound *in, int n);

#endif
