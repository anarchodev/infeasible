// cellar_prop.story — propositional subset of cellar.story (M1, first slice).
//
// Expresses the SAME world that tests/test_world.c builds by hand in
// test_cellar(), in the variable-free / boolean-fluent fragment the current
// engine supports directly. No typed variables, no multi-valued domains, no
// numeric guards yet — those land later in M1 with the grounder and the
// §5.7/5.8 fluent compilation. tests/test_parse.c compiles this file and runs
// the identical assertions, so a green build means the parser reproduces the
// hand-built world through the front door.

// The base facts. weakened and can_force_door are NOT here: they are
// conclusions (rule heads), not fluents — closed-world applies to fluents,
// and I1 keeps conclusions out of the store.
state (
    poisoned
    has_antidote
    strong
    door_closed
    door_open
)

// Everything unlisted is closed-world false: has_antidote and door_open start
// false, so the player is weakened and the door is shut.
init (
    poisoned
    strong
    door_closed
)

// ---- judgments: derived from state, never stored ----

// Poison normally weakens you — unless you're holding the antidote.
// `unless` sugars to a defeater blocking `weakened`.
rule poison_weakens: poisoned => weakened  unless has_antidote

// You can shoulder a closed door open if you're strong…
rule can_force: strong & door_closed => can_force_door
// …but not while weakened, and that exception wins.
rule too_weak:  weakened => ~can_force_door
too_weak > can_force

// ---- actions: the only things that change base facts ----

action force_door:
    requires can_force_door & door_closed
    causes   door_open & ~door_closed
