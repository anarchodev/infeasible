// encounter5e.story — a runnable 5e combat slice in the CURRENTLY-SUPPORTED
// surface (unlike combat5e.story, which is an aspirational sketch that uses
// bands / providers / primed numeric guards the M1 parser still rejects).
//
// This file carries the two things the core is for:
//   (1) the 5e condition-interaction graph, as JUDGMENTS the host reads to
//       shape its d20 rolls (advantage / disadvantage / can-you-even-act), and
//   (2) the damage & condition ACTIONS, which are the only mutation (I2).
// The die rolls themselves live in the host driver (srd_probe P5): randomness
// sits ABOVE world_*, and the recorded action log replays exactly (I4).

sort actor

entity (
    aria, veyra   : actor          // the party: a fighter and a rogue
    grunk, snik   : actor          // the opposition: two goblins
)

state (
    hp(actor) : int in 0 .. 30     // value store; the range clamps the floor at 0
    ac(actor) : int
    alive(actor)

    // conditions (base boolean fluents, flipped only by actions)
    unconscious(actor)  stunned(actor)  paralyzed(actor)
    prone(actor)  restrained(actor)  frightened(actor)  poisoned(actor)
    invisible(actor)
)

init (
    alive(aria)  alive(veyra)  alive(grunk)  alive(snik)
    hp(aria) = 20   ac(aria) = 16
    hp(veyra) = 14  ac(veyra) = 14
    hp(grunk) = 7   ac(grunk) = 15
    hp(snik) = 7    ac(snik) = 15
)

// ---- the condition graph (the schema proven in tests/bench_5e.c) ----------

// incapacitated: three conditions feed it; it beats the "can act" default.
rule inc_u(X: actor): unconscious(X) => incapacitated(X)
rule inc_s(X: actor): stunned(X)     => incapacitated(X)
rule inc_p(X: actor): paralyzed(X)   => incapacitated(X)

rule act_base(X: actor): alive(X)         => can_act(X)
rule act_no(X: actor):   incapacitated(X) => ~can_act(X)
act_no > act_base

// attacked-at-advantage: prone/restrained/paralyzed targets are easy marks...
rule adv_prone(X: actor): prone(X)      => attacked_at_adv(X)
rule adv_rest(X: actor):  restrained(X) => attacked_at_adv(X)
rule adv_para(X: actor):  paralyzed(X)  => attacked_at_adv(X)
// ...unless the target is invisible: you can't press an advantage you can't see.
rule adv_inv(X: actor):   invisible(X)  ~> ~attacked_at_adv(X)

// attacks-at-disadvantage: your own condition fouls your swing.
rule dis_fri(X: actor): frightened(X) => attacks_at_disadv(X)
rule dis_poi(X: actor): poisoned(X)   => attacks_at_disadv(X)
rule dis_res(X: actor): restrained(X) => attacks_at_disadv(X)
rule dis_pro(X: actor): prone(X)      => attacks_at_disadv(X)   // prone: melee swings at disadv

// dropped to 0 hp (the guard reads the value store)
rule down(X: actor): hp(X) <= 0 -> down(X)

// ---- actions: the only mutation; damage rides the effect pipeline (§5.8) ---

action greatsword(A: actor, T: actor): causes hp(T) -= 8    // aria
action sneak(A: actor, T: actor):      causes hp(T) -= 6    // veyra (sneak attack)
action scimitar(A: actor, T: actor):   causes hp(T) -= 5    // goblin

action shove(A: actor, T: actor):      causes prone(T)      // fighter knocks prone
action vanish(A: actor):               causes invisible(A)  // rogue slips out of sight
action snarl(A: actor, T: actor):      causes frightened(T) // goblin's menacing snarl
