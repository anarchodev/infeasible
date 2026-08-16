// blast.story — area of effect with no host code (§13's binder, #231, #255).
//
// An AoE centres on a place, not on a person, and a place is not an entity a
// story usually has. The three pieces that make it expressible here all landed
// separately and meet for the first time in this file:
//
//   - the target is a CELL ENTITY, so `fireball(mage, c22)` is an ordinary
//     ground action, named in the action log and therefore replayed exactly.
//     An action argument cannot carry a value, and a `domain point` parameter
//     reaches a provider as a placeholder the host resolves out of band — no
//     use to a story with no host — so the entity is not a workaround, it is
//     the only form that survives I4.
//   - `sort placed union actor, cell` (#231) lets ONE `grid_x` carry the
//     position of anything on the map. Without the cover a cell and an actor
//     cannot share a coordinate predicate, and the stock grid would have to
//     learn a second pair of names.
//   - `grid_chebyshev` is a MEASUREMENT (§5.6): the library answers a distance
//     and the STORY picks the radius, so "a fireball is 20 feet" is a line a
//     remixer edits. A `grid_in_blast` provider would have compiled the radius
//     into C and answered yes or no, accounting for nothing.
//
// Everything is answered by the square-grid library shipped with the engine,
// so this file has no game code behind it at all.

sort actor
sort cell
sort placed union actor, cell     // anything with a position on the map

function grid_chebyshev(placed, placed) : int
provider grid_adjacent(actor, actor)

entity ( mage, grik, gnok, thorn : actor )
entity ( c22, c55 : cell )        // the places a spell can be aimed at

state (
    grid_x(placed) : int in 0 .. 64
    grid_y(placed) : int in 0 .. 64
    grid_blocks(actor)            // read by the stock provider's LoS walk
    alive(actor)
    burning(actor)
    fire_ward(actor)              // the exception the library must not own
    hp(actor) : int in 0 .. 30
)

init (
    alive(mage) alive(grik) alive(gnok) alive(thorn)
    hp(mage)=30 hp(grik)=30 hp(gnok)=30 hp(thorn)=30
    grid_x(mage)=0  grid_y(mage)=0
    grid_x(grik)=2  grid_y(grik)=2     // on the centre
    grid_x(gnok)=3  grid_y(gnok)=3     // one cell out — caught
    grid_x(thorn)=9 grid_y(thorn)=9    // well clear
    grid_x(c22)=2   grid_y(c22)=2
    grid_x(c55)=5   grid_y(c55)=5
    fire_ward(thorn)
)

// ---- the blast ------------------------------------------------------------
//
// A set-quantified effect binder (§13) over a measurement the story
// thresholds. The affected set is resolved at tick time from state, so nothing
// here enumerates cells or precomputes a template.

action fireball(C: actor, P: cell):
    requires alive(C)
    causes for each T: actor where alive(T) & grid_chebyshev(T, P) <= 1 :
        { hp(T) -= 8 when ~fire_ward(T),
          burning(T) when ~fire_ward(T) & hp(T) >= 9 }

// The `hp(T) >= 9` is not decoration. A target the blast drops to zero would be
// set burning by this action and unset by the death ramification below in the
// same step — a contested step, which #98 refuses to compile rather than let
// the runtime discover. Surviving the hit is exactly the condition under which
// catching fire means anything.

// A warded target is spared by the STORY, not by the library — which is the
// point of shipping the distance rather than the ruling. `why? burning(thorn)`
// has a rule to show; a `grid_in_blast` provider would have had only a verdict.

rule scorched(X: actor): burning(X) & ~fire_ward(X) => singed(X)

// The dead stop burning: an ordinary ramification over the same state.
rule dies(X: actor): hp(X) <= 0 causes ~alive(X) & ~burning(X)
