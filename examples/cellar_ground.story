// cellar_ground.story — the boolean-predicate slice of the cellar, exercising
// the M1 grounder (DESIGN.md §5.2, §11 M3 boundary): typed variables over
// declared sorts, predicate fluents grounded closed-world over the cross
// product, per-actor `unless` defeaters, and — the load-bearing bit —
// superiority grounded over the SHARED variable, not the cross product.
//
// Two actors run through the same rules and reach opposite conclusions purely
// by grounding: the guard holds the antidote, the hero does not. tests/
// test_ground.c compiles this and pins every ground verdict.

sort actor, item

entity (
    hero, guard : actor
    antidote    : item
)

state (
    poisoned(actor)
    holding(actor, item)
    strong(actor)
    door_closed
)

init (
    poisoned(hero)
    poisoned(guard)
    strong(hero)
    strong(guard)
    holding(guard, antidote)   // only the guard is holding the antidote
    door_closed
)

// ---- judgments: derived per actor, never stored ----

// Poison weakens you — unless you're holding the antidote. The `unless`
// sugars to a per-instance defeater, so it blocks weakened(guard) but not
// weakened(hero).
rule poison_weakens(X: actor): poisoned(X) => weakened(X)  unless holding(X, antidote)

// Strong + a closed door => you can shoulder it open…
rule can_force(X: actor): strong(X) & door_closed => can_force_door(X)
// …but not while weakened, and that exception wins — for the SAME actor.
rule too_weak(X: actor):  weakened(X)             => ~can_force_door(X)
too_weak > can_force

// ---- the only mutation: a parameterized action ----

action force_door(X: actor):
    requires can_force_door(X) & door_closed
    causes   ~door_closed
