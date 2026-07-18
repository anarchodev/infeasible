// srd_probe2.story — second adversarial slice: SUMMONING and PERSISTENT
// TERRAIN. Chosen because they are structurally unlike combat5e/srd_probe
// (fixed rosters, instantaneous effects). The question each asks is narrow:
// does the EXISTING design (§5.9 spawning, §5.6 space) already cover the SRD
// content, or does it expose a NEW unfrozen construct the M1 parser must wait
// on? Answer, up front: no new construct. Both reuse designed surface — and
// both re-summon the SAME binder srd_probe P1 found. That convergence is the
// real result: there is one gap, and it shows up everywhere.

scene wilds

sort actor
enum school { conjuration, evocation }        // ⟂ same value-domain gap as P1

entity (
    dara  : actor                   // druid (the player)
    wolf[8] : actor                 // §5.9 pool sugar — 8 prebaked slots
    ogre  : actor
)

state (
    active(actor)                   // §5.9 pool membership fluent
    hp(actor)     : int in 0 .. hp_max(actor)
    hp_max(actor) : int
    at(actor)     : cell            // §5.6 functional fluent (⟂ `cell` value domain)
    monster(actor)
    on_fire(cell)                   // ⟂ terrain state: a fluent OVER cells
    greased(cell)
)

provider (
    in_area(cell, cell, int)        // cells within R of a center cell
    at_cell(actor, cell)            // actor occupies cell (index over at(·))
)

// ===========================================================================
// P6 — CONJURE ANIMALS.  "Summon fey spirits: one beast of CR 2, or two of
// CR 1, or four of CR 1/2, or eight of CR 1/4." The caster picks the count.
// ===========================================================================
//
// §5.9 says spawning is an ordinary action that flips `active` on pool
// members. A FIXED-count summon writes cleanly, no new surface:

action summon_two_wolves(C: actor, a: cell, b: cell):
    causes active(wolf_1) & at(wolf_1) := a & hp(wolf_1) := 11
         & active(wolf_2) & at(wolf_2) := b & hp(wolf_2) := 11

// ...but the count is CHOSEN AT CAST (1/2/4/8). Writing one action per count
// is the tell: the natural form wants to activate the first N idle slots —
//
//     action conjure(C: actor, n: int, where: set of cell):
//         for each W: actor where wolf(W) & ~active(W)  limit n {   // ⟂ GAP
//             causes active(W) & at(W) := next(where) & hp(W) := 11
//         }
//
// ⟂ GAP 6 — THIS IS P1's BINDER AGAIN, on the spawn side. "Apply an effect to
// N members of a provider/pool-answered set" is the identical missing
// construct as Fireball's "apply an effect to each target in radius." The only
// new rider is `limit n` (bounded quantification) — and §5.9 already bounds it
// for free: the pool is size 8, so `limit` degrades to "up to the pool." Not a
// second gap; the SAME gap, which strengthens the case that its shape must be
// settled before M1 effect parsing.
//
// A SINGLE named summon (Find Familiar) is the other §5.9 path — mechanism 2,
// scope instantiation — and needs no binder at all. Its one open point is the
// already-known §13 template identity, not new surface. Confirmed, not novel.

// ===========================================================================
// P7 — WALL OF FIRE / GREASE.  Persistent zones: state attached to SPACE, that
// acts on whoever occupies it, each tick, until it ends.
// ===========================================================================
//
// §5.6 keeps the logic spatially blind: zone membership is a PROVIDER answer,
// the zone itself is a fluent over cells. Both pieces already exist in the
// design. The hazard is then an ordinary ramification (§5.3) gated by the
// provider — structurally identical to combat5e's torch-drop, just spatial:

rule fire_tick(X: actor, c: cell):
    at_cell(X, c)' & on_fire(c)'  causes  hp(X)' -= 5
    // "ends its turn in the wall" — a per-tick ramification over occupancy.

rule grease_fall(X: actor, c: cell):
    at_cell(X, c) & greased(c)  =>  prone(X)      // a judgment, recomputes on move (I3)

action cast_wall_of_fire(C: actor, line: set of cell):
    causes on_fire(each c in line)               // ⟂ GAP 7 = the binder, on cells

// ⟂ GAP 7 — setting a zone is "assert on_fire(c) for each c in the area": the
// SAME set-quantified effect, its bound variable ranging over cells instead of
// actors. And the wall "lasts 1 minute" -> P4's turn-counter decomposition,
// unchanged. No new construct: value-domain (`cell`) + provider (designed) +
// ramification (designed) + the binder + P4. All already on the list.

// ---- consolidated verdict across all three scripts -------------------------
//
// combat5e.story  — the curated happy path. Bands, multi-valued defeat, the
//                    ladder override, primed numeric guards, the pipeline. ✓
// srd_probe.story  — combat corners: AoE, reactions, concentration, duration,
//                    the die roll.
// srd_probe2.story — summoning + terrain.
//
// Everything that stalls reduces to exactly TWO things to settle before the
// M1 effect parser — no more, no less:
//
//   (1) THE SET-QUANTIFIED EFFECT BINDER — `for each T where <guard> [limit n]:
//       <effect> [when <cond>]`. Found in P1 (Fireball), reappears in P3
//       (concentration retract), P6 (summon N), P7 (zone paint). Its riders:
//       per-target `when`, transient action-scoped inputs, relational
//       provenance (`faerie_fire_by`), `limit n`. This IS the M3 grounding
//       risk. It is the one thing whose SHAPE must be frozen first.
//
//   (2) FIRST-CLASS VALUE DOMAINS beyond entity sorts — `cell`, `point`,
//       `enum school { … }`. Needed by targeted/area/typed-value spells.
//       Smaller, orthogonal, and mechanical once decided.
//
// Everything else is already designed or already a known open question:
//   - reactions      -> host two-phase loop protocol (P2), a DESIGN note
//   - randomness     -> above world_*, injected as action params (P5), a note
//   - duration       -> host turn-counter + retract ramification (P4), a note
//   - summon identity, cross-scope identity -> §13, already open
//   - spawning, space -> §5.9 / §5.6, already designed; SRD content fits them
//
// So: M1 is NOT blocked wholesale. Declarations, judgment rules, bands, simple
// (fixed-arity) actions can be parsed now. Only the EFFECT grammar waits on
// (1)+(2). That is a much smaller freeze than "the surface isn't ready."
