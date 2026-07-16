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

// ---- narrative: queries conclusions, fires actions ----

=== cellar_door ===
The cellar door is shut fast.
{ door = locked: A heavy padlock glints in the torchlight. }

+ { holding(player, rusty_key) & door = locked } [Try the rusty key]
      do unlock(player)
      The padlock falls away with a clunk.
      -> cellar_door

+ { can_force_door(player) } [Shoulder the door open]
      do force_door(player)
      It gives way in a shower of splinters. -> inside

+ { weakened(player) & door = closed } [Shoulder the door open]
      You brace yourself — and your poisoned limbs betray you.
      -> cellar_door

* [Retreat upstairs] -> hallway
