// mv_door.story — multi-valued fluents (DESIGN.md §5.7), the state-transition
// slice: a `: { … }` value domain, an `init` assignment, value guards in
// `requires`/rule bodies, and `causes` effects that erase to the whole value
// family (chosen value + sibling negations) so exactly one value holds each
// tick and a flip-flop is a contested step. tests/test_mv.c pins all of it.
//
// The engine stays propositional: door = v is the boolean atom "door=v", and
// this file compiles to the same shape test_multival.c builds by hand.

sort actor
entity hero : actor

state (
    door : { locked, closed, open }
    has_key(actor)
)

init (
    door = locked
    has_key(hero)
)

// a judgment reading a value guard — grounded over actors (you can pass if the
// door is open and you're the one holding the key)
rule can_pass(X: actor): door = open & has_key(X) => can_pass(X)

// unlock needs the key and a locked door; it lands on closed
action unlock(X: actor):
    requires has_key(X) & door = locked
    causes   door = closed

// shove opens a closed door
action shove(X: actor):
    requires door = closed
    causes   door = open

// two always-applicable writers of different values — stepping both is a
// contested step (the multi-valued flip-flop), rejected with state untouched
action jam_open:   causes door = open
action jam_closed: causes door = closed
