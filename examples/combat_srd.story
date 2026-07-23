// combat_srd.story — a faithful-as-today 5e melee. Every die is rolled
// ENGINE-SIDE and seeded (§5.10): attack rolls AND damage. Adjacency is a
// PROVIDER the host answers from positions (§5.6). Advantage comes from the
// condition graph. The host passes only a seed, adjacency, and the chosen
// actions; the engine resolves hit/miss/damage — and it all replays from the
// seed + action log (I4). Nothing is host-rolled or host-injected.

sort actor

provider adjacent(actor, actor)          // answered host-side from positions

entity ( aria, grunk, snik : actor )

state (
    hp(actor)  : int in 0 .. 40
    ac(actor)  : int
    atk(actor) : int                     // attack bonus (to-hit)
    dmg(actor) : int                     // damage bonus
    prone(actor)
)

init (
    hp(aria)=24  ac(aria)=16  atk(aria)=5  dmg(aria)=3     // elf fighter
    hp(grunk)=7  ac(grunk)=15 atk(grunk)=4 dmg(grunk)=2     // goblin
    hp(snik)=7   ac(snik)=15  atk(snik)=4  dmg(snik)=2      // goblin
)

// down at 0 hp — a read-side numeric-guard judgment (the host queries it)
rule down(X: actor): hp(X) <= 0 -> down(X)

// 5e: a melee attack against a prone creature has advantage
rule advantage(A: actor, T: actor): adjacent(A, T) & prone(T) => advantage(A, T)

// to-hit — the d20 rolled INSIDE the guard. Advantage is modelled the clean
// defeasible way: a SECOND independent d20 that also concludes `hit`, gated by
// advantage. Without advantage only the first die is rolled (hit iff it clears);
// with advantage, hit iff *either* die clears — which is exactly "roll two, take
// the higher" for a threshold check. (Avoids `~advantage`: negating a derived
// defeasible pred is not closed-world — the engine won't prove ~advantage just
// because advantage failed, so the two-rule form is both correct and provable.)
rule hit(A: actor, T: actor):
    roll(20, 1) + atk(A) >= ac(T)                    -> hit(A, T)
rule hit_adv(A: actor, T: actor):
    advantage(A, T) & roll(20, 2) + atk(A) >= ac(T)  -> hit(A, T)

// a melee attack: must be adjacent and land the hit; damage is 1d6 + bonus,
// rolled engine-side. A miss = the requires fails = no damage (the step still
// commits — the turn is spent).
action strike(A: actor, T: actor):
    requires adjacent(A, T) & hit(A, T)
    causes   hp(T) -= roll(6) + dmg(A)

// knock a target prone — sets up advantage for the next attacker
action shove(A: actor, T: actor):
    requires adjacent(A, T)
    causes   prone(T)
