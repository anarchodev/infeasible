// srd_probe.story — a DELIBERATELY HOSTILE 5e slice (aspirational; no parser).
//
// Unlike combat5e.story, this file is not curated to fit the surface. It is
// the opposite: five SRD mechanics chosen because they look like they will
// NOT fit, written at full ambition to find exactly where the pen stalls.
// Every stall is marked `⟂ GAP` with the smallest surface that would close it
// and a pointer to the risk it lands on. Reading order = the probes below.
//
// Verdict summary (details inline):
//   P1 Fireball      ⟂ set-quantified effects, per-target conditional effects,
//                       action-scoped transient inputs (the save). Big one.
//   P2 Shield        ✓ mostly a host-loop protocol, not new surface — but it
//                       forces a two-phase step the current loop doesn't model.
//   P3 Faerie Fire   ⟂ relational effect-provenance + set retract on a
//                       ramification. Needs cross-tick binding (caster→targets).
//   P4 duration      ⟂ no time primitive; decomposes to host turn-counters.
//   P5 the save roll  ✓ resolves cleanly: randomness lives ABOVE world_* (I4),
//                       injected as an action parameter. Worth stating in DESIGN.

scene arena

sort actor, item

entity (
    vera            : actor         // wizard (the player)
    grik, gnok, gob : actor         // three goblins, clustered — the AoE fodder
    thorn           : actor         // an ally fighter, also in blast range
)

// ---- state -----------------------------------------------------------------

state (
    hp(actor)     : int in 0 .. hp_max(actor)
    hp_max(actor) : int
    ac(actor)     : int in 0 .. 30
    dead(actor)
    bloodied(actor)
    monster(actor)

    // conditions & spell marks
    faerie_fired(actor)
    invisible(actor)
    hidden(actor)

    // resources — spell slots as a small multi-valued count
    slots3(actor) : int in 0 .. 4   // remaining level-3 slots

    // concentration: at most one, named by the spell it sustains
    concentrating(actor)
    conc_spell(actor) : spell        // ⟂ GAP(minor): `spell` is not a `sort`.
        // A multi-valued fluent whose domain is an enum of spell ids. Sorts
        // are for entities (§entity-sorts). Candidate: `enum spell { ... }`
        // as a first-class value domain, distinct from `sort`. Low risk.
)

provider (
    in_radius(actor, point, int)     // ⟂ GAP(minor): `point` is not declared.
        // Targeted-point spells need a location value type. Candidate: a
        // built-in `point` sort or `coord` value. combat5e never needed it
        // because all its actions target actors, not squares.
    adjacent(actor, actor)
)

init (
    hp_max(vera) = 22   hp(vera) = 22   ac(vera) = 12   slots3(vera) = 1
    hp_max(thorn) = 30  hp(thorn) = 30  ac(thorn) = 18
    hp_max(grik) = 7    hp(grik) = 7    ac(grik) = 15   monster(grik)
    hp_max(gnok) = 7    hp(gnok) = 7    ac(gnok) = 15   monster(gnok)
    hp_max(gob)  = 7    hp(gob)  = 7    ac(gob)  = 15   monster(gob)
)

// ===========================================================================
// P1 — FIREBALL.  "Each creature in a 20-foot-radius sphere must make a DEX
// save, taking 8d6 fire damage on a failure, or half as much on a success."
// ===========================================================================
//
// This is the probe that breaks. Written the way an author wants to write it:

action fireball(C: actor, at: point, save_failed: set of actor):
    requires slots3(C) >= 1
    causes   slots3(C) -= 1

    // ⟂ GAP 1a — SET-QUANTIFIED EFFECT.  There is no surface for "apply an
    // effect to the set of T matching a guard." combat5e actions have fixed
    // arity and one target (`sword_strike(X, Y)`). Candidate: an effect-body
    // binder that ranges over a provider-answered set —
    //
    //     for each T: actor where in_radius(at, T, 20) {
    //         causes hp(T) -= 8   when T in save_failed          // full
    //         causes hp(T) -= 4   when T not in save_failed      // half
    //     }
    //
    // This is the flagged grounding risk made concrete: the target set is
    // DYNAMIC and provider-answered, resolved at tick time, not enumerable at
    // compile time. That is the M3 join matcher, not anything M0 can ground.
    // Everything below (1b, 1c) is downstream of accepting some binder here.

    // ⟂ GAP 1b — PER-TARGET CONDITIONAL EFFECT.  `causes ... when <guard>` is
    // new. combat5e effects are unconditional once `requires` passes. Half-on-
    // success needs the effect itself to branch per bound target, not the
    // action to gate as a whole. Without it you cannot write "8d6 or half."

    // ⟂ GAP 1c — ACTION-SCOPED TRANSIENT INPUT.  `save_failed` is not world
    // state — it is a decision made THIS action and gone next tick. It is
    // neither a fluent (never persisted, I1/I2) nor a provider (not a stable
    // world relation). Candidate: action parameters may carry transient
    // sets/values the host supplies per invocation. This is also the seam
    // where P5 (the die roll) enters — see below.

// friendly-fire falls out for free: thorn is in `in_radius` too, so the same
// binder hits an ally. No special case — desirable, and a point in favor of
// the binder over any "target the enemies" sugar.

// ===========================================================================
// P2 — SHIELD.  Reaction: "when you are hit by an attack, +5 AC until your
// next turn, potentially turning the hit into a miss."
// ===========================================================================
//
// The mechanic itself fits the existing model better than expected:
//   - hit/miss is a JUDGMENT (attack_roll >= ac), never stored (I1).
//   - Shield sets a temporary AC-boost fluent; the hit judgment recomputes.
// So no *rule* surface is missing — a defeasible AC bump plus P4's duration.
//
rule ac_shielded(X): shielded(X) => ac(X) += 5    @condition   // (needs shielded fluent)
//
// ⟂ GAP 2 — is a TIMING/LOOP gap, not a language gap. Shield fires AFTER the
// attacker commits and BEFORE damage is resolved, and it changes whether the
// triggering action lands. The current step model is one-shot: actions in,
// next state out, no interruption point. A reaction needs a two-phase tick —
//     propose(attack) -> offer reactions -> resolve(attack + reactions)
// — which is a HOST-LOOP protocol (§6.3 driver code), not new .story syntax.
// Worth an explicit note in DESIGN: reactions are a driver contract. The one
// thing the core must expose to make it possible: the host can read the
// hit/miss judgment of a *proposed* action before committing the step.

// ===========================================================================
// P3 — FAERIE FIRE (concentration).  Targets are outlined; on concentration
// end, ALL of them stop being outlined.  Cf. combat5e's `outlined > unseen`.
// ===========================================================================

action cast_faerie_fire(C: actor, at: point, targets: set of actor):
    requires slots3(C) >= 1 & ~concentrating(C)
    causes   slots3(C) -= 1 & concentrating(C) & conc_spell(C) := faerie_fire
    // for each T in targets: causes faerie_fired(T)          // same 1a binder

rule outlined(X): faerie_fired(X) => ~hidden(X)   @condition   // (as combat5e)

// The break. Any damage-driven CON save the host adjudicates ends it:
action break_concentration(C: actor):
    causes ~concentrating(C)

// ⟂ GAP 3 — SET RETRACT NEEDS RELATIONAL PROVENANCE.  "outlined ends when the
// caster's concentration ends" is a ramification over a set the engine must
// remember — WHICH actors did THIS caster outline? A boolean `faerie_fired(T)`
// has lost its source. You need relational state:
//
//     state faerie_fire_by(actor, actor)      // (caster, target)
//     rule end_ff(C, T):
//         ~concentrating(C)' & faerie_fire_by(C, T)  causes  ~faerie_fired(T)'
//                                                          & ~faerie_fire_by(C, T)'
//
// That is expressible ONCE relational base facts + the 1a binder exist, and it
// reopens the §13 cross-scope identity question (the target set must keep
// stable identity from the cast tick to the retract tick). Not M0-groundable.

// ===========================================================================
// P4 — DURATION.  Shield lasts "until your next turn"; Faerie Fire "1 minute."
// ===========================================================================
//
// ⟂ GAP 4 — NO TIME PRIMITIVE, BY DESIGN.  The core has no clock (I4: no
// wall-clock, determinism). "Time" is turns, and turns are host-driven. There
// is no `for 1 minute` / `until end of next turn` surface, and arguably there
// should not be one in the core. It decomposes to a host-tracked counter plus
// an ordinary retract ramification:
//
//     state expires_at(actor) : int          // turn number, host-advanced
//     rule shield_expires(X):
//         turn() >= expires_at(X)  causes  ~shielded(X)'
//
// Open: does `turn()` live in the core as a distinguished monotone fluent the
// host bumps, or entirely host-side with the host issuing the retract action?
// Either works; it wants a DESIGN decision, not a parser feature.

// ===========================================================================
// P5 — THE SAVE ROLL.  Where does randomness enter without breaking I4?
// ===========================================================================
//
// ✓ RESOLVES CLEANLY, and the resolution is worth writing into DESIGN.
// I4 forbids unseeded randomness in the logic core. So the roll happens in the
// HOST (the JS driver), and its OUTCOME is injected as an action parameter —
// exactly the `save_failed: set of actor` in P1 and `targets` in P3. The core
// stays a pure function of (state, action + its resolved inputs). A replay log
// of actions-with-resolved-inputs is therefore still exact (I4), because the
// randomness was consumed above the core and recorded in the action. This is
// the single cleanest finding in the probe: randomness lives above world_*,
// never inside it, and the action parameter is its entry point.

// ---- what this proves ------------------------------------------------------
// Frozen-able now: P5 (randomness-above-core) and P2 (reactions = a two-phase
// driver protocol, needing only "read a proposed action's judgment"). Both are
// DESIGN notes, not parser work.
// NOT frozen: the effect-side surface. P1/P3 all trace back to ONE missing
// construct — a set-quantified effect binder over a provider-answered set —
// plus its riders (per-target `when`, transient action inputs, relational
// provenance). That construct is also the M3 grounding risk. Building the M1
// parser around effects BEFORE this binder's shape is decided is the specific
// rework the probe was meant to catch.
