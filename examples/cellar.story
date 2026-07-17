// cellar.story — surface-language sketch (aspirational; parser lands in M1).
// The scaffold's tests build this same world through the C API.

scene cellar

entity guard : actor
entity rusty_key, torch, antidote : item

fluent holding(actor, item)
fluent hp(actor) : int
fluent door : { locked, closed, open }
fluent poisoned(actor)

init door = locked
init holding(guard, torch)
init hp(guard) = 12
init poisoned(player)

// ---- rules: judgments derived from state (never stored) ----
// `->` strict, `=>` defeasible, `unless` introduces a defeater

rule dead(X: actor):     hp(X) <= 0            ->  dead(X)
rule weakened(X: actor): poisoned(X)           =>  weakened(X)
                         unless holding(X, antidote)

rule can_force(X: actor): strength(X) >= 4 & door = closed  =>  can_force_door(X)
rule too_weak(X: actor):  weakened(X)                       =>  ~can_force_door(X)
too_weak > can_force     // superiority: the exception beats the norm

// ---- actions: the only things that change base facts ----

action unlock(X: actor):
    requires holding(X, rusty_key) & door = locked
    causes   door = closed

action force_door(X: actor):
    requires can_force_door(X) & door = closed
    causes   door = open

rule drop_on_death(X: actor, T: item):        // ramification: fires during any step
    dead(X) & holding(X, T)  causes  on_floor(T)

// ---- driving it ----
// There is no narrative layer (DESIGN.md §12.1). A host presents the door
// by querying these judgments and offering the actions whose `requires`
// currently hold:
//   can_force_door(player)                  -> offer force_door(player)
//   holding(player, rusty_key) & door=locked -> offer unlock(player)
//   weakened(player) & door=closed          -> the shoulder attempt fails
// Chosen actions go into the next world_step; a greyed-out option is
// world_why(can_force_door(player)) away from an explanation.
