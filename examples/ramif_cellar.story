// ramif_cellar.story — the §5.4 flagship ramification through the surface
// language: a slain guard drops its torch in the SAME step it dies. Authored,
// not hand-built — the surface twin of test_world.c's torch ramification.
//
// The cascade turns on time: the death (`~alive(X)'`) is read in the NEXT
// state, so the drop reacts to it inside one fixpoint; the torch it still
// carries (`holding(X, T)`) is read NOW, before the drop falsifies it. This is
// where the design beats a STRIPS add/delete list — the indirect effect is a
// rule, not a hand-maintained list entry.

sort actor, item

entity (
    guard : actor
    torch : item
)

state (
    alive(actor)
    holding(actor, item)
    on_floor(item)
)

init (
    alive(guard)
    holding(guard, torch)
)

action slay(X: actor):
    causes ~alive(X)

// A ramification: `rule … causes …`, no action trigger — it fires in any step
// whose state matches. `~alive(X)'` is next-state (the death happening this
// step); `holding(X, T)` is current-state (what the guard still holds as it
// falls). Author writes the prime explicitly; there is no inference.
rule drop_on_death(X: actor, T: item):
    ~alive(X)' & holding(X, T)  causes  ~holding(X, T) & on_floor(T)
