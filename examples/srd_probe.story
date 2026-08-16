// srd_probe.story — a DELIBERATELY HOSTILE 5e slice, re-probed on the shipped
// M1 surface. (Originally a pre-parser sketch "written at full ambition to
// find exactly where the pen stalls" — see git history. This is the same five
// probes, re-run: what stalled then is mostly LANDED now, and each verdict
// below records where its gap went.)
//
// Verdict summary (details inline):
//   P1 Fireball      ✓ CLOSED — the §13 effect binder + provider targeting
//                       (§5.6) + engine-side seeded rolls (§5.10) + the save
//                       as a named-value guard (#82). All live below.
//   P2 Shield        ✓ CLOSED — reaction5e.story authors the full two-phase
//                       protocol with phases-as-a-fluent (EPIC #117); the AC
//                       bump is a `prior` layer. Sketched below, worked there.
//   P3 Faerie Fire   ✓ mechanism CLOSED — relational base fluents + primed
//                       retract ramification, live below. #76 remains open
//                       for the sugared set-retract-with-provenance form.
//   P4 duration      ✓ DECIDED (#7) — no time primitive; host turn-counters,
//                       or an engine-side countdown (reaction5e's cleanup).
//   P5 the save roll  ✓ RESOLVED — but the original verdict was REVERSED:
//                       randomness lives INSIDE the engine as a seeded
//                       keyed lookup (§5.10), not above it. The host supplies
//                       a seed, never outcomes.
//
// Remaining genuine gaps this file still marks: `limit n` (bounded
// quantification) and transient set-valued action inputs — see srd_probe2.

scene arena

sort actor, item
sort cell                                // a place a spell can be aimed at
sort placed union actor, cell            // anything with a position (#231)

enum spell { faerie_fire, fireball }    // was ⟂ GAP(minor): closed by `enum`
                                        // as a first-class value domain (#95/#96)

entity (
    vera            : actor         // wizard (the player)
    grik, gnok, gob : actor         // three goblins, clustered — the AoE fodder
    thorn           : actor         // an ally fighter, also in blast range
)
entity ( c_pack, c_far : cell )     // the places the spells below are aimed at

// ---- state -----------------------------------------------------------------

state (
    // positions the stock grid reads (#255); movement would be an ordinary
    // effect on these, and the library needs no host beside them
    grid_x(placed) : int in 0 .. 64
    grid_y(placed) : int in 0 .. 64
    grid_blocks(actor)
    hp(actor)     : int in 0 .. hp_max(actor)
    hp_max(actor) : int
    acb(actor)    : int             // armor class, base (the `ac` value layers on it)
    dead(actor)
    monster(actor)

    // conditions & spell marks
    faerie_fire_by(actor, actor)    // (caster, target) — relational provenance, P3;
                                    // the caster-less "is outlined" boolean is a
                                    // DERIVED projection of this, never stored
    invisible(actor)
    shielded(actor)

    // resources — spell slots as a small bounded count
    slots3(actor) : int in 0 .. 4   // remaining level-3 slots

    // concentration: at most one, named by the spell it sustains
    concentrating(actor)
    conc_spell(actor) : spell       // an enum-domained MV fluent — the exact
                                    // form the original probe asked for
)

// The stock square grid (#255) answers both. The original probe asked for
// `in_radius(actor, point, int)` and flagged `point` as a missing value type;
// the answer recorded here used to be "resolved by design, not by a type — the
// point and radius are the HOST's geometry behind a provider". That is no
// longer the answer, and the probe's original ask is now GRANTED: the point is
// a cell entity, and the radius is a threshold the story writes on a
// measurement the library returns.
provider grid_adjacent(actor, actor)
function grid_chebyshev(placed, placed) : int

init (
    // the goblins clustered around c_pack, thorn caught with them, vera clear
    grid_x(vera)=0  grid_y(vera)=0
    grid_x(grik)=10 grid_y(grik)=10   grid_x(gnok)=11 grid_y(gnok)=10
    grid_x(gob)=10  grid_y(gob)=11    grid_x(thorn)=12 grid_y(thorn)=10
    grid_x(c_pack)=10 grid_y(c_pack)=10
    grid_x(c_far)=40  grid_y(c_far)=40
    hp_max(vera) = 22   hp(vera) = 22   acb(vera) = 12   slots3(vera) = 2
    hp_max(thorn) = 30  hp(thorn) = 30  acb(thorn) = 18
    hp_max(grik) = 7    hp(grik) = 7    acb(grik) = 15   monster(grik)
    hp_max(gnok) = 7    hp(gnok) = 7    acb(gnok) = 15   monster(gnok)
    hp_max(gob)  = 7    hp(gob)  = 7    acb(gob)  = 15   monster(gob)
)

// ===========================================================================
// P1 — FIREBALL.  "Each creature in a 20-foot-radius sphere must make a DEX
// save, taking 8d6 fire damage on a failure, or half as much on a success."
// ===========================================================================
//
// This was the probe that broke — three gaps (set-quantified effect, per-
// target conditional effect, transient save input). ALL CLOSED; written the
// way the author wanted to write it, and it compiles:
//
//   - the binder (`for each T … where …`) is the §13 construct, landed;
//   - `when` branches the effect per bound target;
//   - the save is not an injected set: it is an engine-side roll (§5.10),
//     read through a NAMED VALUE (#82) so the save d20 and the damage dice
//     are each drawn once per target and shared by every reader — "half as
//     much" halves THE SAME dice, not a fresh roll.

value save_d20(actor)  : int
value fire_dmg(actor)  : int
rule the_save(T: actor):  => save_d20(T) = roll(20)
rule the_dice(T: actor):  => fire_dmg(T) = roll(6) + roll(6) + roll(6)
    // 3d6 for brevity; real 5e sums eight roll(6) terms

// BOTH outcomes are derived POSITIVELY — a deliberate beat: `~saved(T)` in a
// body would need something to CONCLUDE the negation. Derived judgments are
// not closed-world (only base fluents flip under `~`; the test_exprtest pin),
// so a REFUTED `saved` does not make `~saved` fire. The complementary guards
// make the pair exclusive by construction.
rule saved(T: actor):       save_d20(T) + 2 >= 15  => saved(T)
rule save_failed(T: actor): save_d20(T) + 2 < 15   => save_failed(T)
    // flat +2 DEX vs DC 15 for brevity; per-actor mods are one more fluent

// A 20-foot radius is 4 squares on a 5-foot grid. That number lives HERE,
// where a remixer can change it, rather than inside a provider that would
// have answered yes or no and accounted for nothing.
action fireball(C: actor, P: cell):
    requires slots3(C) >= 1
    causes   slots3(C) -= 1
           & for each T: actor where grid_chebyshev(T, P) <= 4: {
                 hp(T) -= fire_dmg(T)      when save_failed(T) ,  // full
                 hp(T) -= fire_dmg(T) / 2  when saved(T)          // half
             }

// friendly-fire falls out for free: thorn is inside the radius too, so the same
// binder hits an ally. No special case — desirable, and a point in favor of
// the binder over any "target the enemies" sugar.

// ===========================================================================
// P2 — SHIELD.  Reaction: "when you are hit by an attack, +5 AC until your
// next turn, potentially turning the hit into a miss."
// ===========================================================================
//
// ✓ CLOSED, exactly along the line the original probe drew: the mechanic is
// a host-visible two-phase protocol plus an ordinary AC bump. What landed:
//   - the AC bump is a `prior` LAYER on a derived value (#82), below;
//   - the two-phase tick is PHASES AS A FLUENT (EPIC #117) — no engine
//     construct; reaction5e.story authors the full round (declare → react →
//     resolve → cleanup), with the reaction window as a JUDGMENT the host
//     reads, and the locked d20 re-tested against the LIVE ac in resolve.
// The one core capability the probe said was needed — "read a proposed
// action's judgment before committing" — became reified attack-in-flight
// fluents (`pending`, `atk_die`), which is strictly more replayable (I4).

value ac(actor) : int
rule ac_base(X: actor):                => ac(X) = acb(X)
rule ac_shielded(X: actor): shielded(X) => ac(X) = prior + 5

// ===========================================================================
// P3 — FAERIE FIRE (concentration).  Targets are outlined; on concentration
// end, ALL of them stop being outlined.
// ===========================================================================
//
// The original GAP — "a boolean faerie_fired(T) has lost its source; you need
// relational state" — is CLOSED IN MECHANISM: relational base fluents ground
// fine, the binder marks (caster, target) pairs at cast, and a primed
// ramification retracts exactly this caster's set when concentration ends.

action cast_faerie_fire(C: actor, P: cell):
    requires slots3(C) >= 1 & ~concentrating(C)
    causes   slots3(C) -= 1 & concentrating(C) & conc_spell(C) = faerie_fire
           & for each T: actor where grid_chebyshev(T, P) <= 4 & monster(T):
                 faerie_fire_by(C, T)

// the mark is a PROJECTION of the relation (I1) — never stored, so two
// casters on one target can never contest it (the #98/#160 check REJECTED
// the stored-boolean version of this file: cast by C1 and end-of-C2's-
// concentration retract could hit the same `faerie_fired(T)` in one step;
// deriving it dissolves the conflict instead of ordering it)
rule ff_marked(C: actor, T: actor): faerie_fire_by(C, T) => faerie_fired(T)
rule outlined(X: actor):            faerie_fired(X)      => ~hidden(X)

// Any damage-driven CON save the host adjudicates ends it. The `requires`
// is load-bearing beyond taste: it makes break/cast statically exclusive,
// which the #98/#160 conflictable-pair ERRORS demand — a compile proves no
// step can contest `concentrating` (§5.13).
action break_concentration(C: actor):
    requires concentrating(C)
    causes   ~concentrating(C)

rule end_ff(C: actor, T: actor):
    concentrating(C) & ~concentrating(C)' & faerie_fire_by(C, T)
    causes ~faerie_fire_by(C, T)
    // fires exactly on the BREAKING EDGE (held now, gone next tick) — the
    // primed read cascades the retract in the same step as the break, and
    // the unprimed `concentrating(C)` conjunct is what lets the #160 check
    // SEE this can never co-fire with a fresh cast. Only the relation is
    // retracted; the derived mark drops with it. `conc_spell` keeps its
    // last value, guarded by `concentrating` (negative MV effects need the
    // §5.7 family reification — still out).

// What REMAINS of the original gap is the sugar, tracked as #76: set-retract
// with provenance as one construct ("concentration ends -> un-mark exactly
// the caster's set") instead of the hand-written pair fluent + ramification
// above. The cross-scope identity question it reopens is still §13's.

// ===========================================================================
// P4 — DURATION.  Shield lasts "until your next turn"; Faerie Fire "1 minute."
// ===========================================================================
//
// ✓ DECIDED (#7): no time primitive, by design (I4). "Time" is turns, and
// turns are host-driven — a host-tracked counter plus an ordinary retract
// ramification. reaction5e.story shows the fully engine-side variant: a
// countdown fluent decremented by a cleanup-phase ramification
// (`bless_left -= 1`, expiry clears the mark), no host bookkeeping at all.

// ===========================================================================
// P5 — THE SAVE ROLL.  Where does randomness enter without breaking I4?
// ===========================================================================
//
// ✓ RESOLVED — and the original probe's "cleanest finding" was REVERSED by
// the design that landed. The probe concluded rolls must happen in the host
// and enter as action parameters. §5.10 instead put the dice INSIDE the
// engine as a seeded keyed lookup: roll sites key on (rule instance, binding,
// tick), the host supplies one SEED, and a save is base facts + seed +
// action log — replay is exact (I4) with the host supplying no outcomes at
// all. P1 above runs on it: the save d20, the damage dice, and their
// sharing across the full/half branches are all engine-side. The original
// host-injected-outcome route survives only where the host genuinely OWNS
// the decision (which creatures a wall of wolves blocks, say) — see
// srd_probe2 on transient set inputs.

// ---- what this proves ------------------------------------------------------
// The original run of this probe found ONE missing construct (the set-
// quantified effect binder) plus riders, and flagged it as the M3 grounding
// risk. That construct landed (§13 binder, provider guards, `when`); its
// riders landed as better designs than the probe asked for (engine-side
// rolls for the save; named values for die sharing; relational fluents for
// provenance). Still open, and marked in srd_probe2: `limit n` (bounded
// quantification) and transient set-valued action inputs (#76 for the
// retract sugar).
