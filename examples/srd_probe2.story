// srd_probe2.story — second adversarial slice: SUMMONING and PERSISTENT
// TERRAIN, re-probed on the shipped M1 surface. (Originally a pre-parser
// sketch; see git history. The original's consolidated verdict named two
// things to settle — the set-quantified binder and first-class value
// domains — and BOTH landed; what remains below is narrower.)
//
// Verdict summary:
//   P6 Conjure Animals  ✓ fixed-count summons compile (function-provider
//                          placement, §5.6); the count-CHOSEN-AT-CAST form
//                          still waits on two marked gaps: §5.9 pool sugar
//                          (`wolf[8]`) and the binder's `limit n` rider.
//   P7 Wall of Fire     ✓ CLOSED — but by §5.6's decision, not by the
//                          construct the original asked for: the ZONE is the
//                          host's geometry behind a provider, not a fluent
//                          over cells. The hazard tick and the grease trip
//                          are ordinary rules, live below.

scene wilds

domain cell                           // §5.6 store-backed positions (landed —
                                      // was "⟂ cell value domain" in the sketch)
sort actor
enum school { conjuration, evocation }   // landed (#95/#96) — was the P1 gap

entity (
    dara  : actor                   // druid (the player)
    // ⟂ GAP (§5.9, still open): `wolf[8] : actor` pool sugar — 8 prebaked
    // slots in one declaration. Until it lands, the pool is spelled out:
    wolf1, wolf2 : actor            // (two slots suffice for this slice)
    ogre  : actor
)

function summon_spot(cell, int) : cell   // host geometry: the i-th free cell
                                         // adjacent to the caster (§5.6 — the
                                         // grid lives behind the seam)

state (
    active(actor)                   // §5.9 pool membership — doubling as "present
                                    // on the field", which also gives the spatial
                                    // rules below their positive generator (§5.2
                                    // cardinality: a provider never anchors)
    hp(actor)     : int in 0 .. hp_max(actor)
    hp_max(actor) : int
    at(actor)     : cell            // store-backed position (§5.6, landed)
    monster(actor)

    // persistent zones: the zone's EXISTENCE is engine state; its EXTENT is
    // host geometry behind the providers below (space is providers, §5.6)
    wall_of_fire_up
    grease_down
)

provider (
    in_fire_zone(actor)             // host: at(X) intersects the wall's line
    in_grease(actor)                // host: at(X) is on the greased square
)

init (
    active(dara)        hp_max(dara) = 24   hp(dara) = 24
    hp_max(wolf1) = 11  hp_max(wolf2) = 11
    active(ogre)        hp_max(ogre) = 59   hp(ogre) = 59   monster(ogre)
)

// ===========================================================================
// P6 — CONJURE ANIMALS.  "Summon fey spirits: one beast of CR 2, or two of
// CR 1, or four of CR 1/2, or eight of CR 1/4." The caster picks the count.
// ===========================================================================
//
// §5.9 says spawning is an ordinary action that flips `active` on pool
// members. A FIXED-count summon compiles today — placement is a function
// provider (the host picks free adjacent cells), hp comes up with the slot:

action summon_two_wolves(C: actor):
    requires ~active(wolf1) & ~active(wolf2)
    causes   active(wolf1) & at(wolf1) := summon_spot(at(C), 1) & hp(wolf1) := 11
           & active(wolf2) & at(wolf2) := summon_spot(at(C), 2) & hp(wolf2) := 11

// Two casters summoning in one step would contest the same slots; the
// exclusivity protocol (#159) makes one-summon-per-step CHECKED, not hoped:
exclusive summon_two_wolves(_)

// ...but the count is CHOSEN AT CAST (1/2/4/8). Writing one action per count
// is still the tell. The natural form wants to activate the first N idle
// slots:
//
//     action conjure(C: actor, n: int):
//         causes for each W: actor where ~active(W)  limit n :   // ⟂ GAP
//             active(W) & at(W) := summon_spot(at(C), 1) & hp(W) := 11
//
// ⟂ GAP — `limit n` (bounded quantification) is the binder's one unlanded
// rider (grammar: "a later slice"). The binder itself, `where` over state,
// and provider placement all landed; only the bound is missing. §5.9 still
// bounds it for free (the pool is finite), so the gap is the SURFACE for
// "first N", not groundability.
//
// A SINGLE named summon (Find Familiar) is the other §5.9 path — mechanism
// 2, scope instantiation — and needs no binder at all. Its one open point is
// the already-known §13 template identity, not new surface.

// ===========================================================================
// P7 — WALL OF FIRE / GREASE.  Persistent zones: state attached to SPACE,
// that acts on whoever occupies it, each tick, until it ends.
// ===========================================================================
//
// ✓ CLOSED — by the §5.6 decision, which is SHARPER than what the original
// probe asked for. The sketch wanted `on_fire(cell)` — a fluent over cells —
// and a binder to paint it. But cells are an OPEN domain (`domain cell`),
// not a finite sort: terrain-as-per-cell-fluents would drag the grid into
// the engine, which is exactly what "space is providers" forbids. The
// shipped shape: the zone's EXISTENCE is one fluent (I2: actions raise and
// drop it), its EXTENT is host geometry answered through a provider, and
// the hazard is an ordinary ramification gated on both:

action cast_wall_of_fire(C: actor):
    causes wall_of_fire_up
    // the host records the line's geometry when it sees this action commit
    // (it is the geometry authority, §5.6); replay re-derives it from the
    // action log the same way (I4)

rule fire_tick(X: actor):
    wall_of_fire_up & active(X) & in_fire_zone(X)  causes  hp(X) -= 5
    // "in the wall at the start of its turn" — a per-tick ramification over
    // provider-answered occupancy; structurally combat5e's torch-drop,
    // just spatial

action cast_grease(C: actor):    requires ~grease_down  causes grease_down
action grease_dries(C: actor):   requires grease_down   causes ~grease_down
    // complementary `requires` = the #160 exclusion, same move as srd_probe's
    // concentration pair: a compile proves no step contests `grease_down`

rule grease_fall(X: actor):
    grease_down & active(X) & in_grease(X)  =>  prone(X)
    // a judgment — recomputes on movement, never stored (I1)

// The wall "lasts 1 minute" -> the P4 duration decomposition (#7, decided):
// a host turn-counter, or reaction5e's engine-side cleanup countdown.

// ---- consolidated verdict across all three scripts -------------------------
//
// combat5e.story  — the curated path, now COMPILING on shipped surface:
//                    bands, team defeat, derived values with min-class
//                    layers, defeat-the-definitions, dynamic clamp, the
//                    stratified dying trigger. Its one marked gap: §6.2's
//                    `overriding` annotation.
// srd_probe.story — COMPILING: binder AoE with engine-side dice and shared
//                    draws, per-target `when`, relational-provenance
//                    retract on the concentration edge. Marked gaps: #76
//                    (set-retract sugar).
// srd_probe2      — COMPILING: fixed-count summons + provider zones. Marked
//                    gaps: §5.9 pool sugar, binder `limit n`.
//
// The original two-item freeze list (binder shape, value domains) is fully
// landed. What the probes now mark is a short tail, none of it blocking:
//   - `limit n` on the binder (P6)              — surface, not groundability
//   - §5.9 pool sugar `wolf[8]`                 — declaration convenience
//   - §6.2 `overriding` ladder-override annotation (combat5e)
//   - #76 set-retract-with-provenance sugar (srd_probe P3)
//   - negative MV effects (§5.7 family reification; srd_probe P3 note)
