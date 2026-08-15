// tactics.story — the world half of §13's tactics slice (the third skin).
//
// Space comes from the STOCK GRID (§5.6, #255): positions are ordinary numeric
// fluents named `grid_x`/`grid_y`, and the geometry is the square-grid library
// shipped with the engine. So this file needs no game host code at all, which
// is not a preference — the slice's client is a cart, and a cart has none. The
// opaque-cell seam (`domain cell` + a host `neighbor` function) is the right
// shape for a host with irregular topology of its own, and `patrol.story`
// keeps it as the worked §5.6 example.
//
// Two consequences worth seeing in the source: movement below is a plain `+=`
// on ordinary state rather than a call into host geometry, and the ranges are
// CONTENT — `grid_chebyshev(G, I) <= 6` is a line a remixer edits, where an
// `in_range` provider would have compiled the six into C and answered yes or
// no while accounting for nothing.
//
// The rule set carries the four cases a demo must show to be visibly
// defeasible rather than an if-else chain with extra steps:
//   1. a PRIORITY CHAIN on one conclusion, resolved by a band;
//   2. SPECIFIC BEATS GENERAL, authored as an individual exception;
//   3. one CRISS-CROSS — two supports, two attackers, each support trumping
//      its own exception, with no single champion (the case §13 keeps team
//      defeat for);
//   4. SPATIAL ANCHORING — every engagement rule gated by the grid, so §5.2's
//      sparse-anchor discipline is visible rather than asserted, and (#257)
//      the anchor can GENERATE its pairs rather than filtering a cross product.

sort actor

provider grid_adjacent(actor, actor)        // Chebyshev 1 — a diagonal counts
function grid_chebyshev(actor, actor) : int // the measurement; the story rules

// The ladder the chain in §03 resolves on. Condition beats a forced
// displacement beats the base intent; immunity sits above everything (§6.2).
bands intent: base < forced < condition < immunity

entity ( r1, r2, r3, r4 : actor )
entity ( b1, b2, b3, b4 : actor )

state (
    grid_x(actor) : int in 0 .. 64
    grid_y(actor) : int in 0 .. 64
    grid_blocks(actor)              // read by the stock provider's LoS walk
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
    grid_x(r1)=1 grid_y(r1)=1  grid_x(r2)=1 grid_y(r2)=3
    grid_x(r3)=1 grid_y(r3)=5  grid_x(r4)=1 grid_y(r4)=7
    grid_x(b1)=2 grid_y(b1)=1  grid_x(b2)=8 grid_y(b2)=3
    grid_x(b3)=8 grid_y(b3)=5  grid_x(b4)=8 grid_y(b4)=7
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

// ---- 4. spatial anchoring: every engagement rule sits on the grid ---------
//
// The provider anchors both variables, so these are not cross-products (§5.2
// cardinality). With a generator-capable provider (#254/#257) the anchor
// PRODUCES its pairs instead of filtering them, which is what takes a
// spatially-anchored judgment from N^2 to roughly linear.

rule spots(G: actor, I: actor):
    alive(G) & alive(I) & red(G) & blue(I) & grid_chebyshev(G, I) <= 6
        => sighted(G, I)

rule engages(X: actor, Y: actor):
    alive(X) & alive(Y) & grid_adjacent(X, Y) & ~guarding(X) => engaged(X, Y)

// ---- the transition ------------------------------------------------------
//
// Movement is an ordinary numeric effect on ordinary state; the stock provider
// re-reads positions when the tick moves, so the geometry follows for free.

action east(G: actor):  requires alive(G) & ~rooted(G) & ~stunned(G)
    causes grid_x(G) += 1
action west(G: actor):  requires alive(G) & ~rooted(G) & ~stunned(G)
    causes grid_x(G) -= 1
exclusive east(G: actor), west(G: actor)

// `hp >= 1` is not decoration: without it `brace` and the `dies` ramification
// can both fire on the same tick with opposite effects on `guarding`, which
// #98 refuses to compile — a contested step, not a defeat.
action brace(G: actor): requires alive(G) & hp(G) >= 1 causes guarding(G)
action relax(G: actor): requires alive(G) causes ~guarding(G)
exclusive brace(G: actor), relax(G: actor)

// A per-pair attack, which is how a designer writes it and how it should read.
// It is also the one construct here that keeps the transition off the lanes:
// a 2-var action grounds one step rule per (attacker, target) pair (#243,
// blocked on #238's binary-relation wall). The crowd-scale form is to compute
// the pairing as a JUDGMENT — which the matcher plus a generator-capable
// provider now does in roughly linear time — and apply damage per unit, the
// shape bench_slice's hand-written reference uses; that awaits #261.
action strike(A: actor, T: actor): requires alive(A) & alive(T) & grid_adjacent(A, T)
    causes hp(T) -= 4

// A ramification: dropping to zero is death, and the dead stop guarding.
rule dies(X: actor):    hp(X) <= 0  causes ~alive(X) & ~guarding(X)

// A casterless broadcast (#240): the tick's upkeep, one cast for the field.
action upkeep: causes for each X: actor where stunned(X) : hp(X) -= 1
