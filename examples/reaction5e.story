// reaction5e.story — the EPIC #117 probe (#118): one full 5e combat round,
// authored end-to-end with turn PHASES as an ORDINARY multi-valued fluent.
// There is no phase construct; there is a fluent, guards, and ramifications.
//
// The load-bearing patterns, in order of novelty:
//
//   1. PHASES AS A FLUENT. `phase : { declare, react, resolve, cleanup }`.
//      Action legality is a `phase = …` guard; phase ADVANCEMENT is causal
//      rules/ramifications, never a host write (I2). The host only submits
//      actions and empty steps; the turn order replays from the log (I4).
//
//   2. THE DECOMPOSED REACTION (Shield). A round is MANY steps — the step is
//      the largest unit nothing can legally interject into. The attack-in-
//      flight is REIFIED as fluents (`pending`, `atk_die`, `atk_mod` — MTG's
//      stack, one level deep): the d20 and the attacker's modifier LOCK at
//      declaration, while the defender's AC stays LIVE. Casting Shield in the
//      react phase layers +5 onto the `ac` value, so the same locked roll that
//      hit in `react` misses in `resolve` — the retroactive swing is just a
//      judgment re-read against a changed world, no rollback machinery.
//
//   3. THE WINDOW IS A JUDGMENT. `can_react(T)` derives from phase + the
//      incoming hit + spell knowledge + reaction economy. The host READS it
//      and asks the player; it never decides it. A declined open window is a
//      logged `pass` action (a real choice, I4); a window that never opens is
//      skipped by RULE (`skip_react`), with no log entry — auto-advance is
//      derivable, so it must not be logged.
//
//   4. ONE DIE, TWO TESTS. The locked `atk_die` is tested against live AC
//      (hit) and against 20 (crit); the crit doubles the damage die branch-
//      free via `test(crit(A))` (#86). Death is an indirect effect of the
//      damage in the same step (`hp'` — the §5.8/#87 stratified read).
//
//   5. DURATIONS TICK IN CLEANUP. Bless is a value layer while `blessed`
//      holds; `bless_left` counts down each cleanup and an expiry
//      ramification clears the mark — no host bookkeeping.
//
// tests/test_reaction.c drives both branches of the window (cast / pass)
// from a scripted deterministic player and pins the round as a golden test.

sort actor

entity ( grunk, vera : actor )     // a goblin, and a wizard who knows Shield

state (
    // the turn clock — an ordinary MV fluent (EPIC #117: no construct).
    // `split` (#121) is a compilation hint with zero semantic content: the
    // step schema specializes per phase value. Deleting it changes time,
    // never meaning — test_reaction's goldens pin exactly that.
    phase : { declare, react, resolve, cleanup } split

    alive(actor)
    hp(actor)   : int in 0 .. 30
    acb(actor)  : int              // armor class, base (the `ac` value layers on it)
    atkb(actor) : int              // attack bonus, base
    dmgb(actor) : int              // damage bonus

    // the attack-in-flight, reified (pattern 2)
    pending(actor, actor)          // A has declared an attack on T
    atk_die(actor) : int in 0 .. 20   // the d20, locked at declaration
    atk_mod(actor) : int              // the modifier, snapshotted at declaration

    has_shield(actor)              // knows Shield (slot bookkeeping elided)
    shielded(actor)                // Shield is up (until end of turn, simplified)
    reacted(actor)                 // this turn's reaction is spent

    blessed(actor)                 // cast before the fight; caster elided
    bless_left(actor) : int in 0 .. 10
)

init (
    phase = declare
    alive(grunk)  alive(vera)
    hp(grunk) = 7    acb(grunk) = 13   atkb(grunk) = 4   dmgb(grunk) = 2
    hp(vera)  = 15   acb(vera)  = 14   atkb(vera)  = 5   dmgb(vera)  = 3
    has_shield(vera)
    blessed(vera)  bless_left(vera) = 2
)

// ---- layered values (#82): the LIVE half of the hit equation ---------------

value ac(actor) : int
rule ac_base(X: actor):               => ac(X) = acb(X)
rule ac_shield(X: actor): shielded(X) => ac(X) = prior + 5    // the reaction lever

value atk(actor) : int
rule atk_base(X: actor):              => atk(X) = atkb(X)
rule atk_bless(X: actor): blessed(X)  => atk(X) = prior + roll(4)   // Bless d4

// ---- judgments -------------------------------------------------------------

// the LOCKED roll vs the LIVE ac — Shield retroactively swings this
rule incoming(A: actor, T: actor):
    pending(A, T) & (atk_die(A) + atk_mod(A)) >= ac(T) => incoming_hit(A, T)

rule crit(A: actor, T: actor): pending(A, T) & atk_die(A) >= 20 => crit(A)

// window-openness is a judgment (pattern 3): the host reads it, never decides it
rule window(A: actor, T: actor):
    phase = react & incoming_hit(A, T) & has_shield(T) & ~reacted(T)
    => can_react(T)

rule down(X: actor): hp(X) <= 0 -> down(X)

// ---- actions: the ONLY host inputs; none of them touches `phase` -----------

// the die AND the modifier lock here by snapshot — rolls are per-tick, so a
// multi-step protocol must carry them in the store (#129)
action strike(A: actor, T: actor):
    requires phase = declare & alive(A) & alive(T)
    causes   pending(A, T) & atk_die(A) := roll(20) & atk_mod(A) := atk(A)

// One strike per attacker per tick (#159): atk_die(A)/atk_mod(A) key on the
// attacker alone, so two same-tick strikes by one A would contest the die.
// The protocol is now declared and checked, not assumed. (The remaining #98
// warning on to_react/skip_react is real and stays: two pendings on targets
// with mixed shield states in ONE tick would contest `phase` — the deeper
// one-attack-in-flight invariant is #129/#131 territory.)
exclusive strike(A: actor, _)

action cast_shield(T: actor):
    requires phase = react & can_react(T)
    causes   shielded(T) & reacted(T)

action pass(T: actor):             // a DECLINED open window must be logged (I4)
    requires phase = react
    causes   reacted(T)

// ---- phase advancement: ramifications only ---------------------------------

// a declared attack opens the window — or skips it when it cannot open
// (`~has_shield` is closed-world over a base fluent, so the skip is provable;
// gating entry on the hit itself needs a primed judgment — #131)
rule to_react(A: actor, T: actor):
    phase = declare & pending(A, T)' & has_shield(T)   causes phase = react
rule skip_react(A: actor, T: actor):
    phase = declare & pending(A, T)' & ~has_shield(T)  causes phase = resolve

rule after_react(T: actor):
    phase = react & reacted(T)'                        causes phase = resolve

// the attack lands (or not — the judgment is re-read HERE, post-Shield)
rule land_hit(A: actor, T: actor):
    phase = resolve & pending(A, T) & incoming_hit(A, T)
    causes hp(T) -= roll(4) + dmgb(A) + test(crit(A)) * roll(4)

rule finish(A: actor, T: actor):
    phase = resolve & pending(A, T)  causes  ~pending(A, T) & phase = cleanup

// death is an indirect effect of the damage, same step (§5.8 strata, #87)
rule slain(X: actor): hp(X)' <= 0 & alive(X) causes ~alive(X)

// ---- cleanup: durations tick, buffs drop, the clock returns ----------------

rule tick_bless(X: actor):
    phase = cleanup & blessed(X)                       causes bless_left(X) -= 1
rule expire_bless(X: actor):
    phase = cleanup & blessed(X) & bless_left(X) <= 1  causes ~blessed(X)
rule drop_shield(X: actor): phase = cleanup & shielded(X)  causes ~shielded(X)
rule clear_react(X: actor): phase = cleanup & reacted(X)   causes ~reacted(X)

rule next_turn: phase = cleanup causes phase = declare
