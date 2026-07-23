#ifndef INF_LOGIC_DL_COL_H
#define INF_LOGIC_DL_COL_H

#include "logic/dl.h"

/* Columnar evaluation of a homogeneous rule family (DESIGN.md 5.8, third
 * backing — experimental prototype).
 *
 * A family is one rule-graph *schema* shared by N entities: the same rules,
 * same attackers, same superiority edges, only the per-entity fact bits
 * differing. Instead of grounding N structurally identical copies into the
 * scalar solver, the family stores each literal's status as a bitvector
 * column over entities and runs the same tri-valued fixpoint as dl.c with
 * word-wide AND/OR/NOT — 64 entities per instruction.
 *
 * Semantics are identical to dl_solve per entity (pinned by the differential
 * test in tests/test_col.c). Heterogeneous entities (per-instance rules or
 * superiority overrides) are out of scope here — per the design they
 * partition out of the family and fall back to scalar. `why?` is served by
 * re-deriving one instance scalar-style; this prototype has no trace of its
 * own.
 *
 * Atoms are family-local ids 0..natoms-1 (not interned): the schema is the
 * vocabulary. Fact columns are host-writable rows — the ECS reading: base
 * fluents are components, `dlcol_fact_row` is the column pointer. Bit e of
 * word e/64 is entity e. The host asserts facts closed-world like the world
 * tier does (for a base fluent b: bit set in row(+b) or in row(~b)). */

typedef struct dlcol dlcol;

dlcol *dlcol_new(int natoms, int nentities);
void   dlcol_free(dlcol *f);

/* Schema construction — mirrors dl_add_rule / dl_add_sup over family-local
 * atom ids. Returns a rule id usable in dlcol_add_sup. Names are cold data
 * for dlcol_why (copied; NULL becomes "?"). */
int  dlcol_add_rule(dlcol *f, const char *name, dl_rule_kind kind,
                    dl_lit head, const dl_lit *body, int nbody);
void dlcol_add_sup(dlcol *f, int winner, int loser);
void dlcol_set_atom_name(dlcol *f, uint32_t atom, const char *name);
/* Provenance suffix (§6.3) for a rule by its dlcol_add_rule handle; rendered by
 * dlcol_why after the rule kind. Copied; NULL clears it. */
void dlcol_set_prov(dlcol *f, int rule_id, const char *prov);

/* Fact columns. A row is ceil(nentities/64) words; the host may write words
 * directly (bits >= nentities are ignored). dlcol_add_fact sets one bit;
 * dlcol_clear_facts zeroes every fact column (start of a fresh assembly). */
uint64_t *dlcol_fact_row(dlcol *f, dl_lit l);
void      dlcol_add_fact(dlcol *f, dl_lit l, int entity);
void      dlcol_clear_facts(dlcol *f);
int       dlcol_row_words(const dlcol *f);

/* Full family recompute from the current fact columns (clears previous
 * statuses first — the branchless sweep is the point; see 5.8). */
void dlcol_solve(dlcol *f);

/* Heap bytes of the solve engine (columnar arrays dominate; excludes cold
 * why-only name strings). Call after dlcol_solve. */
#include <stddef.h>
size_t dlcol_footprint(const dlcol *f);

dl_verdict dlcol_definite(const dlcol *f, dl_lit q, int entity);   /* +/-Delta */
dl_verdict dlcol_defeasible(const dlcol *f, dl_lit q, int entity); /* +/-d    */

/* Read-only verdict column of the last solve: bit e set iff +d holds for
 * l at entity e. The ECS reading in the other direction — hosts consume
 * judgment columns the way they write fact columns (a movement system
 * reading `engage` bits as a component array). NULL before first solve. */
const uint64_t *dlcol_proved_row(const dlcol *f, dl_lit l);

/* Proof/defeat trace for one entity's instance of q — dl_why's format with
 * atoms and rules rendered as name[entity]. A pure read of the solved
 * columns (no scalar re-derivation): verdicts, fact bits, applicability,
 * and superiority all come from the fixpoint state. Call after dlcol_solve.
 * Pinned byte-for-byte against dl_why by tests/test_col.c. */
void dlcol_why(const dlcol *f, dl_lit q, int entity, FILE *out);

#endif
