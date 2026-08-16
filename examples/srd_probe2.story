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
//   P7 Wall of Fire     ✓ CLOSED, and BY THE CONSTRUCT THE ORIGINAL ASKED
//                          FOR. The verdict here used to read "the zone is
//                          the host's geometry behind a provider, not a
//                          fluent over cells" — that was true while a game
//                          could ship its own provider. It cannot (#253), and
//                          the zone turns out to be exactly the fluent over
//                          cells the original wanted: `on_fire(cell)` is
//                          ordinary state, so the wall is IN THE SAVE and
//                          replay re-derives nothing. Occupancy is a
//                          measurement, not a host memory.

scene wilds

sort cell                             // a PLACE, and now an entity: a zone is
                                      // state over cells and a spell is aimed
                                      // at one, so cells must be nameable in
                                      // an action (and therefore in the log)
sort placed union actor, cell         // anything with a position (#231)
sort actor
enum school { conjuration, evocation }   // landed (#95/#96) — was the P1 gap

entity (
    dara  : actor                   // druid (the player)
    // ⟂ GAP (§5.9, still open): `wolf[8] : actor` pool sugar — 8 prebaked
    // slots in one declaration. Until it lands, the pool is spelled out:
    wolf1, wolf2 : actor            // (two slots suffice for this slice)
    ogre  : actor
)
// The places a spell is aimed at. A cell is an entity because an action
// argument is one — `cast_wall_of_fire(dara, c_gap)` is in the log, so the
// wall replays exactly without anyone remembering where it went.
entity ( c_gap, c_ford : cell )

// Placement needs no geometry function at all. A summoned creature is put at
// coordinates ARITHMETIC on the caster's — ordinary numeric effects on
// ordinary state — which is cheaper than the `summon_spot(cell, int)` host
// call this file used to declare, and it is in the save.
function grid_chebyshev(placed, placed) : int

state (
    active(actor)                   // §5.9 pool membership — doubling as "present
                                    // on the field", which also gives the spatial
                                    // rules below their positive generator (§5.2
                                    // cardinality: a provider never anchors)
    hp(actor)     : int in 0 .. hp_max(actor)
    hp_max(actor) : int
    grid_x(placed) : int in 0 .. 64 // positions the stock grid reads (#255)
    grid_y(placed) : int in 0 .. 64
    grid_blocks(actor)
    on_fire(cell)                   // the wall's extent — STATE, so it saves
    greased(cell)                   // likewise the grease
    monster(actor)

    // persistent zones: the zone's EXISTENCE is engine state; its EXTENT is
    // host geometry behind the providers below (space is providers, §5.6)
    wall_of_fire_up
    grease_down
)

// No provider answers occupancy. A ZONE IS STATE OVER CELLS — see P7 — and
// "X is standing in it" is a measurement of distance zero, which the stock
// grid returns and the story thresholds.

init (
    grid_x(dara)=2  grid_y(dara)=2    // the ogre wades the ford; dara holds
    grid_x(ogre)=6  grid_y(ogre)=2    // the gap the wall will close
    grid_x(c_gap)=4 grid_y(c_gap)=2
    grid_x(c_ford)=6 grid_y(c_ford)=2
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
    causes   active(wolf1) & hp(wolf1) := 11
           & grid_x(wolf1) := grid_x(C) + 1 & grid_y(wolf1) := grid_y(C)
           & active(wolf2) & hp(wolf2) := 11
           & grid_x(wolf2) := grid_x(C) - 1 & grid_y(wolf2) := grid_y(C)

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

// The wall is PAINTED ONTO CELLS. Its extent is state, so it is in the save
// and replay re-derives nothing — where the host-authority version needed the
// host to watch the action commit and remember where the line went.
action cast_wall_of_fire(C: actor, P: cell):
    causes wall_of_fire_up
         & for each K: cell where grid_chebyshev(K, P) <= 1 : on_fire(K)

rule fire_tick(X: actor, K: cell):
    wall_of_fire_up & active(X) & on_fire(K) & grid_chebyshev(X, K) <= 0
        causes  hp(X) -= 5
    // "in the wall at the start of its turn". Distance ZERO is "standing on
    // that cell" — occupancy as a measurement rather than a host memory, and
    // still structurally combat5e's torch-drop.

action cast_grease(C: actor, P: cell):
    requires ~grease_down  causes grease_down & greased(P)
action grease_dries(C: actor):   requires grease_down   causes ~grease_down
    // complementary `requires` = the #160 exclusion, same move as srd_probe's
    // concentration pair: a compile proves no step contests `grease_down`

rule grease_fall(X: actor, K: cell):
    grease_down & active(X) & greased(K) & grid_chebyshev(X, K) <= 0
        =>  prone(X)
    // still a judgment — recomputes on movement, never stored (I1). What
    // changed is only where the occupancy comes from: a cell the story
    // greased, not a square the host remembered.

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
