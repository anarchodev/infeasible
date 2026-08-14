// grid_pure.story — a spatial story with NO game host code (§5.6, #255).
//
// `cellar_pure.story` and `duel_pure.story` use no providers at all, because a
// provider needs someone to answer it and a pure cart has no game code to be
// that someone. So a hostless story could not express "next to": its only
// options were a cross product the grounder refuses past 2^20 instances, or
// writing the host code the pure cart exists to avoid.
//
// A STOCK provider closes that. `grid_adjacent` and `grid_los` are answered by
// the square-grid library shipped with the engine (`src/stock/grid.c`),
// identical for every cart, so this file declares them and uses them and no
// game code exists anywhere.
//
// The vocabulary is by convention: positions are ordinary numeric fluents named
// `grid_x`/`grid_y`, which is why movement below is a plain `+=` effect and
// needs no geometry function at all. `grid_blocks` marks a sight blocker.

sort actor

provider (
    grid_adjacent(actor, actor)     // Chebyshev distance 1 — a diagonal counts
    grid_los(actor, actor)          // no blocker stands between them
)

entity ( scout, sentry, ally, wall : actor )

state (
    grid_x(actor) : int in 0 .. 64
    grid_y(actor) : int in 0 .. 64
    grid_blocks(actor)              // read by the stock provider's LoS walk
    awake(actor)
    blinded(actor)
    hostile(actor)
)

init (
    grid_x(scout)=1  grid_y(scout)=1
    grid_x(sentry)=2 grid_y(sentry)=1     // adjacent to the scout
    grid_x(ally)=8   grid_y(ally)=1       // far, and behind the wall
    grid_x(wall)=5   grid_y(wall)=1
    grid_blocks(wall)
    awake(scout) awake(sentry) awake(ally)
    hostile(sentry)
)

// ---- judgments over the stock geometry --------------------------------------
//
// The provider answers the MEASURED premise and the story draws the ruling from
// it (§5.6). There is no `can_see` in the library precisely so that this rule —
// and the exception under it — live here, where `why?` can reach them.

rule beside(A: actor, B: actor):
    awake(A) & awake(B) & grid_adjacent(A, B)   => adjacent_to(A, B)

rule spots(A: actor, B: actor):
    awake(A) & awake(B) & grid_los(A, B)        => can_see(A, B)

rule threatened(A: actor, B: actor):
    adjacent_to(A, B) & hostile(B)              => in_melee(A)

// The exception the engine exists for: a blinded sentry sees nothing, however
// clear the line is. A library that shipped `can_see` instead of `los_clear`
// would have swallowed this rule, and `why? can_see(sentry, ally)` would have
// had nothing to say but "the host said no".
rule blind(A: actor, B: actor):
    blinded(A) & awake(B)                       => ~can_see(A, B)

// The exception outranks the general rule — which #98 will not let this file
// omit: two rules concluding complementary `can_see` with nothing ordering them
// is a refusal to compile, not a silent coin toss.
blind > spots

// ---- the transition ---------------------------------------------------------
//
// Movement is an ordinary numeric effect on ordinary state. The grid provider
// re-reads positions when the tick moves, so the geometry follows for free.

action east(A: actor):  requires awake(A) causes grid_x(A) += 1
action west(A: actor):  requires awake(A) causes grid_x(A) -= 1
exclusive east(A: actor), west(A: actor)

action wake(A: actor):  requires ~awake(A) causes awake(A)
action sleep(A: actor): requires awake(A)  causes ~awake(A)
exclusive wake(A: actor), sleep(A: actor)
