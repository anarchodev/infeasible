// numeric_hp.story — numeric fluents and landmark guards (DESIGN.md §5.8),
// the read side. `hp(actor) : int` is a value store, never atoms; comparison
// guards (`hp(X) <= 0`) are harvested per ground instance and asserted
// closed-world from the stored value each evaluation — strict inputs, never
// UNDECIDED, never concluded. Effects (`:=`/`+=`/`-=`) and the declared-range
// clamp are the next slice; here the store is written by init (and, in the
// test, directly, standing in for the effect pipeline). tests/test_numeric.c
// varies the values and pins every bucket, through the query AND step paths.

sort actor
entity hero, guard : actor

state (
    hp(actor) : int
    alerted(actor)
    fleeing(actor)
)

init (
    hp(hero)  = 7
    hp(guard) = 15
    alerted(guard)
)

// strict: at 0 or below you are down (the dying-trigger's read half)
rule down(X: actor):     hp(X) <= 0 -> down(X)
// defeasible: below 10 you are bloodied
rule bloodied(X: actor): hp(X) < 10 => bloodied(X)

// an action gated by a numeric guard — exercises the columnar step path
action flee(X: actor):
    requires hp(X) < 10 & alerted(X)
    causes   fleeing(X)
