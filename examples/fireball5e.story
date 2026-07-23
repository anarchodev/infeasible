// fireball5e.story — Fireball the way the design intends (contrast spellbook5e.story,
// which used host-mutated `clustered`/`saved` stand-ins). Targeting is a spatial
// PROVIDER (§5.6): `in_blast` is a computed relation the host answers from positions,
// consulted at tick time and never stored. Damage is a SEEDED ROLL (§5.10): the dice
// are rolled ENGINE-SIDE from a host-supplied seed, keyed per (rule-instance, tick),
// so a save = base facts + seed + action log replays every roll exactly (I4).
//
// The host supplies only geometry (answers `in_blast(T)` from `at(·)`) and a seed —
// not the target set, not the die outcomes. (The DEX save "half on a success" is a
// roll inside a guard — the next slice; today the roll drives damage directly.)

sort actor

// a computed relation, host-answered (never a fluent the host toggles)
provider in_blast(actor)

entity ( vera, grik, gnok, gob, thorn : actor )

state ( hp(actor) : int in 0 .. 60 )

init ( hp(vera)=22  hp(grik)=14  hp(gnok)=14  hp(gob)=14  hp(thorn)=30 )

// "each creature in a 20-ft sphere takes 8d6 fire." Each target T rolls its own
// dice — the roll site folds in the binding (T), so grik and gnok draw independently.
// Written 3d6 here for brevity; real 5e sums eight roll(6) terms. Friendly fire falls
// out for free: if thorn is in_blast, he burns too — no special case.
action fireball(C: actor):
    causes for each T: actor where in_blast(T):
        hp(T) -= roll(6) + roll(6) + roll(6)

rule down(X: actor): hp(X) <= 0 -> down(X)
