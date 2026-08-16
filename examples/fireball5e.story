// fireball5e.story — Fireball the way the design intends (contrast spellbook5e.story,
// which used host-mutated `clustered`/`saved` stand-ins). Targeting is a spatial
// MEASUREMENT (§5.6): the stock grid returns a distance and this file picks the
// consulted at tick time and never stored. Damage is a SEEDED ROLL (§5.10): the dice
// are rolled ENGINE-SIDE from a host-supplied seed, keyed per (rule-instance, tick),
// so a save = base facts + seed + action log replays every roll exactly (I4).
//
// The host supplies nothing at all now: the grid is the library shipped with the
// not the target set, not the die outcomes. (The DEX save "half on a success" is a
// roll inside a guard — the next slice; today the roll drives damage directly.)

sort actor
sort cell                         // a place the spell is aimed at
sort placed union actor, cell     // anything with a position (#231)

// a computed relation, host-answered (never a fluent the host toggles)
function grid_chebyshev(placed, placed) : int
// A 20-foot radius is 4 squares on a 5-foot grid. The number lives HERE, in
// the story, rather than inside a provider that would answer yes or no.

entity ( vera, grik, gnok, gob, thorn : actor )
entity ( c_pack : cell )          // where the spell lands

state (
    grid_x(placed) : int in 0 .. 64
    grid_y(placed) : int in 0 .. 64
    grid_blocks(actor)
    hp(actor) : int in 0 .. 60
)

init (
    hp(vera)=22  hp(grik)=14  hp(gnok)=14  hp(gob)=14  hp(thorn)=30
    grid_x(vera)=0  grid_y(vera)=0        // the caster, well clear
    grid_x(grik)=10 grid_y(grik)=10       // the pack, and thorn caught with it
    grid_x(gnok)=11 grid_y(gnok)=10
    grid_x(gob)=10  grid_y(gob)=11
    grid_x(thorn)=12 grid_y(thorn)=10
    grid_x(c_pack)=10 grid_y(c_pack)=10
)

// "each creature in a 20-ft sphere takes 8d6 fire." Each target T rolls its own
// dice — the roll site folds in the binding (T), so grik and gnok draw independently.
// Written 3d6 here for brevity; real 5e sums eight roll(6) terms. Friendly fire falls
// out for free: if thorn is inside the radius, he burns too — no special case.
action fireball(C: actor, P: cell):
    causes for each T: actor where grid_chebyshev(T, P) <= 4:
        hp(T) -= roll(6) + roll(6) + roll(6)

rule down(X: actor): hp(X) <= 0 -> down(X)
