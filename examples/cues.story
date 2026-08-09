// cues.story — burst cues (#11, DESIGN.md §12): the presentation interface's
// one-shot event channel, authored end to end.
//
// A cue is a fire-and-forget signal the step hands the renderer: a hit spark,
// a floating "Resisted!", a death cry. GAS splits cues in two, and so does
// this engine — but only one half needs a construct:
//
//   PERSISTENT cues (an aura VFX while a condition holds) are already covered.
//   They are a function of a fluent being set, so the client subscribes to the
//   fluent's delta and starts/stops the loop on the transition. `burning(X)` is
//   state; nothing new is needed.
//
//   BURST cues have no fluent to watch — a hit spark is over before the next
//   tick — so they get `emit`: write-only vocabulary, fired from a `causes`
//   clause, delivered in the step's emission buffer and then gone. An emission
//   is the transient TWIN OF AN ACTION: an action is a transient input to a
//   step, an emission a transient output of one. Neither is ever a fact.
//
// The discipline that keeps that honest:
//   - a cue is never read. No rule body, guard, `requires` or `init` may
//     mention one — reading a cue back would make it state (I1), and would let
//     the renderer's channel leak into the logic.
//   - a cue is never negated. `~spark(X)` is a compile error: a cue fires when
//     its rule fires, so a suppression condition belongs in the rule's BODY.
//     Defeasibility is upstream, where it always was — `resisted` below fires
//     off a judgment that superiority decides.
//   - a cue is a proposition about the transition, not a count. Two rules
//     firing `spark(grunk)` in one step emit it once; distinguish sources by
//     parameterizing the atom.
//
// tests/test_emit.c pins the emission stream of the steps below.

scene cues

sort actor

entity ( grunk, vera : actor )

state (
    alive(actor)
    hp(actor) : int in 0 .. 30
    fire_resistant(actor)
    marked(actor)              // hunter's-mark-ish: extra damage, extra cue
    quiet                      // a stealth scene: the loud cues stay home
)

// The write-only vocabulary. Same shape as a state declaration, no value type:
// everything numeric about the tick is already in the delta and the §5.8
// commit receipt, so a cue carries only WHO and WHAT.
emit (
    spark(actor)               // something connected
    resisted(actor)            // the target shrugged off the fire
    death_cry(actor)           // fired by a ramification, not by the action
    thunder                    // arity 0: a global cue, no subject
)

init (
    alive(grunk)  alive(vera)
    hp(grunk) = 7
    hp(vera)  = 15
    fire_resistant(grunk)
)

// ---- judgments: where the defeasibility lives -----------------------------

// Resistance is an ordinary defeasible conclusion, and `marked` beats it
// (the mark burns through resistance) — the cue below simply reads whichever
// way the argument settled. `no_resist` supplies the NEGATIVE conclusion:
// judgments are not closed-world (only fluents are), so "nothing concluded
// resistance" is UNDECIDED until some rule concludes its complement.
rule resist(X: actor):       fire_resistant(X)             => resists_fire(X)
rule no_resist(X: actor):    ~fire_resistant(X)            => ~resists_fire(X)
rule burn_through(X: actor): fire_resistant(X) & marked(X) => ~resists_fire(X)
burn_through > resist

rule down(X: actor): hp(X) <= 0 -> down(X)

// ---- actions: state and cues in the same `causes` clause -------------------

// The cue rides alongside the damage. `spark(T)` is fired unconditionally with
// the hit; `resisted(T)` only where the judgment holds, so the two guarded
// arms of one action are two rules, exactly as they would be for state.
action firebolt(A: actor, T: actor):
    requires alive(A) & alive(T) & ~resists_fire(T)
    causes   hp(T) -= 6 & spark(T)

action firebolt_resisted(A: actor, T: actor):
    requires alive(A) & alive(T) & resists_fire(T)
    causes   hp(T) -= 3 & spark(T) & resisted(T)

// The mark that burns through resistance. It changes state and says so with a
// cue in one clause — the ordinary case.
action hunters_mark(A: actor, T: actor):
    requires alive(A) & alive(T)
    causes   marked(T) & spark(T)

// A cue with no state change at all: the whole point of the action is the
// noise it makes. `quiet` gates it, so a cue can be conditioned like anything
// else — by its rule's body, never by negating the cue.
action thunderclap(A: actor):
    requires alive(A) & ~quiet
    causes   thunder

// ---- ramifications: cues on indirect effects -------------------------------

// Death is an indirect effect of the damage in the same step (§5.8 strata),
// and its cue rides with it. Note the shape the client sees: one step, one
// emission buffer, both the spark and the cry — the renderer does not have to
// reconstruct causality from a state diff.
rule slain(X: actor):
    hp(X)' <= 0 & alive(X)  causes  ~alive(X) & death_cry(X)
