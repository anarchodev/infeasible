// cover.story — the cover ruling is CONTENT, not a library call (§5.6, #253).
//
// The stock grid could have shipped `has_cover(A, T)`. It deliberately does
// not: half at 50% and three-quarters at 75% are 5e's numbers, and the
// exception under them ("Sharpshooter ignores cover") is a feat, not geometry.
// A boolean provider would have swallowed all three, and `why? harder_shot`
// would have had nothing to say but "the host said yes".
//
// So the library answers the smallest MEASUREMENT that still admits the
// ruling — what percent of the target's outline a blocker hides, counted by
// casting a ray at each corner of its cell — and everything below is a line of
// content a remixer can edit.
//
// The layout, with the archer at (0,0) and two blockers on the field:
//
//        y
//        4  grunt . . . .         grunt   0% — nothing in the way
//        3  . . . . sniper        sniper 75% — three-quarters, line still open
//        2  . . barrel . .        flanker 50% — half
//        1  . . . . flanker       pinned 100% — total, and not a target at all
//        0  archer . crate . pinned
//           0  1  2  3  4   x
//
// `sniper` is the case that makes the argument: the centre line to it is
// CLEAR — `grid_los` says yes — and three of its four corners are hidden. One
// question cannot answer the other, which is why the library ships both.

sort actor

entity ( archer, grunt, flanker, sniper, pinned, crate, barrel : actor )

state (
    grid_x(actor) : int in 0 .. 32
    grid_y(actor) : int in 0 .. 32
    grid_blocks(actor)              // read by the library's rays
    hp(actor) : int in 0 .. 20
    ready(actor)
    sharpshooter(actor)
)

provider grid_los(actor, actor)

// The measurement, not a ruling: the story owns every threshold below.
function grid_occlusion(actor, actor) : int

init (
    grid_x(archer)=0  grid_y(archer)=0
    grid_x(crate)=2   grid_y(crate)=0    grid_blocks(crate)
    grid_x(barrel)=2  grid_y(barrel)=2   grid_blocks(barrel)
    grid_x(grunt)=0   grid_y(grunt)=4
    grid_x(flanker)=4 grid_y(flanker)=1
    grid_x(sniper)=4  grid_y(sniper)=3
    grid_x(pinned)=4  grid_y(pinned)=0
    hp(grunt)=12  hp(flanker)=12  hp(sniper)=12  hp(pinned)=12
    ready(archer)
)

// ---- the ruling -------------------------------------------------------------
//
// Three bands over one measurement, each beating the one below it. Written as
// defeat rather than as `>= 50 & < 75`, because that is what the bands ARE: a
// better cover overrides a lesser one, and the ordering says so once.

// `ready(A) & hp(T) >= 1` is not decoration: a measurement call cannot anchor
// a rule's grounding the way a sparse relation does, so without a premise that
// does, each of these would range over the whole actor cross product — and the
// compiler says so rather than letting it happen quietly (§5.2).

rule light(A: actor, T: actor):
    ready(A) & hp(T) >= 1 & grid_occlusion(A, T) >= 50   => half_cover(A, T)

rule heavy(A: actor, T: actor):
    ready(A) & hp(T) >= 1 & grid_occlusion(A, T) >= 75   => three_quarters_cover(A, T)

rule outbid(A: actor, T: actor):
    ready(A) & hp(T) >= 1 & grid_occlusion(A, T) >= 75   => ~half_cover(A, T)

rule total(A: actor, T: actor):
    ready(A) & hp(T) >= 1 & grid_occlusion(A, T) >= 100  => total_cover(A, T)

rule outbid_heavy(A: actor, T: actor):
    ready(A) & hp(T) >= 1 & grid_occlusion(A, T) >= 100  => ~three_quarters_cover(A, T)

outbid > light
outbid_heavy > heavy

// ---- what the ruling is for -------------------------------------------------
//
// A shot is ordinary until something makes it hard, and total cover means
// there is nothing to shoot at. Both are defeasible, so the feat below only
// has to name the rule it beats.

rule open_shot(A: actor, T: actor):
    ready(A) & hp(T) >= 1        => ~harder_shot(A, T)

rule behind_half(A: actor, T: actor):
    half_cover(A, T)             => harder_shot(A, T)

rule behind_heavy(A: actor, T: actor):
    three_quarters_cover(A, T)   => harder_shot(A, T)

behind_half > open_shot
behind_heavy > open_shot

// The exception the library must not own: Sharpshooter ignores half and
// three-quarters cover — and only those two. Total cover is not cover the feat
// can see through, and the rule below never mentions it.
rule sharp_eye(A: actor, T: actor):
    sharpshooter(A) & hp(T) >= 1 => ~harder_shot(A, T)

sharp_eye > behind_half
sharp_eye > behind_heavy

rule in_the_open(A: actor, T: actor):
    ready(A) & hp(T) >= 1        => targetable(A, T)

rule out_of_sight(A: actor, T: actor):
    total_cover(A, T)            => ~targetable(A, T)

out_of_sight > in_the_open

// A line of sight is the cheaper question and the story keeps it separate:
// `sniper` is visible down the centre line and still three-quarters hidden.
rule sighted(A: actor, T: actor):
    ready(A) & grid_los(A, T)    => can_see(A, T)

// ---- the transition ---------------------------------------------------------

action snap_shot(A: actor, T: actor):
    requires targetable(A, T) & ~harder_shot(A, T)
    causes hp(T) -= 6

action careful_shot(A: actor, T: actor):
    requires targetable(A, T) & harder_shot(A, T)
    causes hp(T) -= 3

exclusive snap_shot(A: actor, T: actor), careful_shot(A: actor, T: actor)

// Movement is an ordinary numeric effect, so the cover bands follow the field
// without anything recomputing them: step out from behind the crate and the
// measurement changes, which changes the ruling.
action sidestep(A: actor): requires hp(A) >= 1 causes grid_y(A) += 1
action backstep(A: actor): requires hp(A) >= 1 causes grid_y(A) -= 1
exclusive sidestep(A: actor), backstep(A: actor)
