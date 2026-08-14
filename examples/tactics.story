// tactics.story — the world half of §13's tactics slice (the third skin).
//
// The spatial seam is patrol.story's, with a squad on it: positions are
// store-backed cell fluents, grid geometry is a host FUNCTION, and proximity
// is a host PROVIDER answered from the host's own index over at(·) (§5.6).
// The engine owns no geometry — "hex vs. square is just the neighbour
// function inside the provider."
//
// No presentation vocabulary lives here. That is derived cart-side, from what
// a tactics game turns out to need, which is the whole point of tactics being
// the third data point after the cellar and the duel.
//
// The rule set carries the four cases a demo must show to be visibly
// defeasible rather than an if-else chain with extra steps:
//   1. a PRIORITY CHAIN on one conclusion, resolved by a band;
//   2. SPECIFIC BEATS GENERAL, authored as an individual exception;
//   3. one CRISS-CROSS — two supports, two attackers, each support trumping
//      its own exception, with no single champion (the case §13 keeps team
//      defeat for);
//   4. SPATIAL ANCHORING — every interesting rule gated by a provider, so
//      §5.2's sparse-anchor discipline is visible rather than asserted.

domain cell
sort   actor

function step(cell, int) : cell     // host grid geometry: one cell in a direction
provider near(actor, actor)         // host index over at(·): alert radius
provider in_reach(actor, actor)     // host index over at(·): melee reach

// The ladder the chain in §03 resolves on. Condition beats a forced displacement beats the
// base intent; immunity sits above everything (§6.2).
bands intent: base < forced < condition < immunity

entity ( r1, r2, r3, r4 : actor )
entity ( b1, b2, b3, b4 : actor )

state (
    at(actor) : cell                // store-backed position, one per actor
    red(actor)
    blue(actor)
    alive(actor)
    stunned(actor)                  // no intent survives this
    rooted(actor)                   // may act, may not move
    knocked(actor)                  // displaced this tick
    guarding(actor)                 // holding ground deliberately
    champion(actor)                 // the authored exception of case 2
    zealous(actor)                  // criss-cross support A
    disciplined(actor)              // criss-cross support B
    shaken(actor)                   // criss-cross attacker A
    exhausted(actor)                // criss-cross attacker B
    hp(actor) : int in 0 .. 30
)

init (
    alive(r1) alive(r2) alive(r3) alive(r4)
    alive(b1) alive(b2) alive(b3) alive(b4)
    red(r1) red(r2) red(r3) red(r4)
    blue(b1) blue(b2) blue(b3) blue(b4)
    hp(r1)=30 hp(r2)=30 hp(r3)=30 hp(r4)=30
    hp(b1)=30 hp(b2)=30 hp(b3)=30 hp(b4)=30
    champion(r1)
    zealous(b2) disciplined(b2) shaken(b2) exhausted(b2)
)

// ---- 1. the priority chain: four rules, one conclusion, one ladder --------
//
// This is the ordering bug every hand-written gameplay codebase has. Here it
// is four rules and a band, and `why? advance(X)` renders which one won.

rule walks(X: actor):    alive(X)    =>  advance(X)      @base
rule shoved(X: actor):   knocked(X)  => ~advance(X)      @forced
rule held(X: actor):     rooted(X)   => ~advance(X)      @condition
rule frozen(X: actor):   stunned(X)  => ~advance(X)      @immunity

// ---- 2. specific beats general: an authored individual exception ----------
//
// A champion shrugs off being rooted — authored one-off, not derived. It must
// out-rank `held`, which sits in `condition`, so the edge is explicit and
// crosses the ladder (§6.2's `overriding` case, spelled as a pairwise `>`).

rule unbowed(X: actor):  champion(X) & rooted(X) => advance(X)   @condition
unbowed > held

// ---- 3. the criss-cross: no single champion ------------------------------
//
// Two supports for `presses(X)`, two attackers, each support trumping its own
// exception and neither trumping the other's. Single-champion defeat would
// silently REFUTE this; team defeat proves it, and that is the distinction
// §13 keeps team defeat for.

rule zeal(X: actor):     zealous(X)      =>  presses(X)
rule drill(X: actor):    disciplined(X)  =>  presses(X)
rule nerve(X: actor):    shaken(X)       => ~presses(X)
rule weary(X: actor):    exhausted(X)    => ~presses(X)
zeal  > nerve
drill > weary

// ---- 4. spatial anchoring: every engagement rule sits on a provider -------
//
// `near`/`in_reach` anchor both variables, so these are not cross-products
// (§5.2 cardinality) — the host walks its own index and the engine never
// learns what a distance is.

rule spots(X: actor, Y: actor):
    alive(X) & alive(Y) & red(X) & blue(Y) & near(X, Y)  => sighted(X, Y)

rule engages(X: actor, Y: actor):
    alive(X) & alive(Y) & in_reach(X, Y) & ~guarding(X)  => engaged(X, Y)

// ---- the transition ------------------------------------------------------

action east(G: actor):  requires alive(G) & ~rooted(G) & ~stunned(G)
    causes at(G) := step(at(G), 0)
action west(G: actor):  requires alive(G) & ~rooted(G) & ~stunned(G)
    causes at(G) := step(at(G), 1)
exclusive east(G: actor), west(G: actor)

// `hp >= 1` is not decoration: without it `brace` and the `dies` ramification
// below can both fire on the same tick with opposite effects on `guarding`,
// which #98 refuses to compile — a contested step, not a defeat.
action brace(G: actor): requires alive(G) & hp(G) >= 1 causes guarding(G)
action relax(G: actor): requires alive(G) causes ~guarding(G)
exclusive brace(G: actor), relax(G: actor)

action strike(A: actor, T: actor): requires alive(A) & alive(T) & in_reach(A, T)
    causes hp(T) -= 4

// A ramification: dropping to zero is death, and the dead stop guarding.
rule dies(X: actor):    hp(X) <= 0  causes ~alive(X) & ~guarding(X)

// A casterless broadcast (#240): the tick's upkeep, one cast for the field.
action upkeep: causes for each X: actor where stunned(X) : hp(X) -= 1
