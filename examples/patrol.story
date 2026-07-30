// A worked spatial scenario (DESIGN §5.6) — the proof that space needs NO new
// engine or language primitive. Positions are cell FLUENTS; movement is a causal
// RULE that calls a host geometry FUNCTION; and the spatial relation `near` is a
// PROVIDER the host answers from its own index over at(·). The logic never sees
// the grid — "hex vs. square is just the neighbor function inside the provider."
//
// Everything below is already-shipped surface: store-backed cell fluents
// (#19 slices 1-2), a value-returning function provider (slice 3, #34), a boolean
// provider (§5.6), and an ordinary judgment rule. The engine owns none of the
// geometry; the host owns all of it, behind the provider/function seam.

domain cell
sort actor

function step(cell, int) : cell     // host grid geometry: a cell one step in a
                                    // direction. Deterministic and seedless (I4).
provider near(actor, actor)         // host: the two actors are within alert range,
                                    // computed from their at(·) positions.

entity ( guard1, intruder1 : actor )

state (
    at(actor) : cell                // store-backed position — one handle per actor
    guard(actor)
    intruder(actor)
)

init ( guard(guard1) intruder(intruder1) )

// Movement is the ONLY thing that changes a position (I2); the grid never mutates
// itself. The host's step() computes the neighbour cell (dir: 0=E 1=W 2=S 3=N).
action east(G: actor): causes at(G) := step(at(G), 0)
action west(G: actor): causes at(G) := step(at(G), 1)

// Both moves assign at(G), so co-submitting them for ONE guard would be a
// contested step. `exclusive` (#159, §5.13) makes the one-order-per-guard
// protocol CHECKED instead of a host promise: world_step rejects a violating
// action set pre-solve, and the #98 conflictable-pair warning is discharged —
// this file compiles zero-warning.
exclusive east(G: actor), west(G: actor)

// A judgment DERIVED from the provider — never stored (I1). `near(G, I)` anchors
// the two variables, so this is not a cross-product blow-up (§5.2 cardinality).
rule spot(G: actor, I: actor):
    guard(G) & intruder(I) & near(G, I) => spotted(I)
