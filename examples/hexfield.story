// hexfield.story — the same questions, the other topology (§5.6, #253).
//
// §5.6 says hex vs. square is just the neighbour function inside the provider,
// and this file is what that buys: the rules below are the square story's
// rules with a different vocabulary in front of them. Nothing in the engine
// knows a hex from a square; the library does, and it is the only thing that
// does.
//
// The vocabulary is by convention, and it is a SEPARATE one on purpose. A hex
// story keeps its positions in `hex_q`/`hex_r` — axial coordinates — so a rule
// asking `hex_adjacent` cannot quietly read a square story's `grid_x`: an
// undeclared position reads 0, which would stand the whole cast on one hex and
// report everyone adjacent to everyone. The compiler refuses that (#263).
//
// The field, in axial coordinates — six neighbours, no diagonals to argue
// about:
//
//      ranger(0,0)  wolf(1,0)  boulder(2,0)  ...  elk(4,0)
//              crow(0,3)                stag(3,-1)
//
//   ranger -> wolf     distance 1, adjacent
//   ranger -> boulder  distance 2, in the way
//   ranger -> elk      distance 4, sight blocked, 100% hidden
//   ranger -> stag     distance 3, sight CLEAR, 33% hidden — two of its six
//                      corners sit behind the boulder, which is exactly the
//                      case a `has_cover` boolean could not have told apart

sort actor

entity ( ranger, wolf, boulder, elk, crow, stag : actor )

state (
    hex_q(actor) : int in -32 .. 32
    hex_r(actor) : int in -32 .. 32
    hex_blocks(actor)               // read by the library's rays
    awake(actor)
    quarry(actor)
)

provider (
    hex_adjacent(actor, actor)      // hex distance 1 — six neighbours, no diagonal
    hex_los(actor, actor)           // no blocker on the line between them
)

// The measurements: the library returns numbers, the story picks thresholds.
function hex_distance(actor, actor) : int
function hex_occlusion(actor, actor) : int

init (
    hex_q(ranger)=0   hex_r(ranger)=0
    hex_q(wolf)=1     hex_r(wolf)=0
    hex_q(boulder)=2  hex_r(boulder)=0   hex_blocks(boulder)
    hex_q(elk)=4      hex_r(elk)=0
    hex_q(crow)=0     hex_r(crow)=3
    hex_q(stag)=3     hex_r(stag)=-1
    awake(ranger) awake(wolf) awake(elk) awake(crow) awake(stag)
    quarry(elk) quarry(stag)
)

// ---- judgments over the stock geometry --------------------------------------

rule beside(A: actor, B: actor):
    awake(A) & awake(B) & hex_adjacent(A, B)     => in_reach(A, B)

rule spots(A: actor, B: actor):
    awake(A) & awake(B) & hex_los(A, B)          => can_see(A, B)

// Three hexes is a shout; the number lives here, not in C.
rule shouting(A: actor, B: actor):
    awake(A) & awake(B) & hex_distance(A, B) <= 3  => in_shout(A, B)

// And half the outline is concealment — the same band the square story writes,
// over the same measurement, at a topology that counts six corners instead of
// four.
rule shrouded(A: actor, T: actor):
    awake(A) & quarry(T) & hex_occlusion(A, T) >= 50 => concealed(A, T)

rule stalkable(A: actor, T: actor):
    quarry(T) & can_see(A, T)                    => can_stalk(A, T)

rule lost(A: actor, T: actor):
    concealed(A, T)                              => ~can_stalk(A, T)

lost > stalkable

// ---- the transition ---------------------------------------------------------
//
// Movement is an ordinary numeric effect on ordinary state, exactly as on the
// square: a hex has six directions, so it takes six actions and no geometry
// function at all.

action east(A: actor):  requires awake(A) causes hex_q(A) += 1
action west(A: actor):  requires awake(A) causes hex_q(A) -= 1
exclusive east(A: actor), west(A: actor)

action northeast(A: actor): requires awake(A) causes hex_q(A) += 1 & hex_r(A) -= 1
action southwest(A: actor): requires awake(A) causes hex_q(A) -= 1 & hex_r(A) += 1
exclusive northeast(A: actor), southwest(A: actor)

action southeast(A: actor): requires awake(A) causes hex_r(A) += 1
action northwest(A: actor): requires awake(A) causes hex_r(A) -= 1
exclusive southeast(A: actor), northwest(A: actor)
