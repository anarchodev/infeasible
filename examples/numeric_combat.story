// numeric_combat.story — the numeric-fluent WRITE side (DESIGN.md §5.8),
// through the surface language. `hp(actor) : int in 0..20` is a value store
// with a declared clamp range; actions change it with the closed effect
// operators (`:=`/`+=`/`-=`) whose RHSs are full expressions through the
// compiler's expression VM. Two effects on one fluent in one tick combine by
// summation (order-free); the declared range is the outermost clamp; a `<= 0`
// guard reads the store to conclude `dead`. tests/test_numeff.c compiles this
// and drives the step function.

sort actor
entity hero, goblin : actor

state (
    hp(actor) : int in 0..20
    burning(actor)
)

init (
    hp(hero)   = 12
    hp(goblin) = 7
)

// read side: at 0 or below you are dead (the guard reads the value store)
rule slain(X: actor): hp(X) <= 0 -> dead(X)

// a plain constant delta
action strike(A: actor, T: actor):
    causes hp(T) -= 4

// two effects in one action: damage plus a status flip
action immolate(T: actor):
    causes hp(T) -= 6 & burning(T)

// an expression RHS: heal to at least 10, the range caps the top at 20
action mend(T: actor):
    causes hp(T) := max(hp(T), 10)
