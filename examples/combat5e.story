// combat5e.story — surface-language sketch (aspirational; parser lands in M1).
// A 5e-flavored combat slice exercising the decisions of DESIGN.md §5.7
// (multi-valued fluents, strict-team defeat, negative heads), §5.8 (value
// store, landmark guards, the effect pipeline, the dying trigger), and §6.2
// (priority bands, intra-band `>`, the annotated ladder override).
// Unlike cellar.story, this file obeys the declared-vocabulary discipline
// (§6.1 item 2): every atom is a declared fluent or some rule's head.

scene skirmish

entity aria : actor          // elf fighter (the player)
entity grunk : actor         // goblin
entity longsword, shortbow : item

// ---- the priority ladder (§6.2): the 5e stack, verbatim ----

bands stat_stack: base < condition < feat < immunity

// ---- fluents ----

fluent hp(actor) : int in 0 .. hp_max(actor)
    // declared range = the outermost clamp stage (§5.8): "any leftover
    // damage is lost" (PHB) is schema, not a rule anyone can forget
fluent hp_max(actor) : int
fluent speed(actor) : int in 0 .. 60  combine min
    // per-fluent collision resolver (§5.8 escape hatch 3): two effects
    // set your speed -> the most restrictive applies
fluent elf(actor)
fluent monster(actor)
fluent restrained(actor)
fluent encumbered(actor)
fluent slept(actor)                 // under magical sleep
fluent invisible(actor)
fluent faerie_fired(actor)
fluent freedom_of_movement(actor)
fluent dead(actor)
fluent holding(actor, item)
fluent on_floor(item)

init elf(aria)          init monster(grunk)
init hp_max(aria) = 20  init hp(aria) = 20
init hp_max(grunk) = 7  init hp(grunk) = 7
init holding(aria, longsword)
init holding(grunk, shortbow)

// spatial guards are provider-answered, never enumerated (§5.6)
provider adjacent(actor, actor)
provider los(actor, actor)

// ---- judgments: the 5e stack through bands (§6.2) ----

// Base tier: what you are.
rule elf_speed(X):    elf(X)     => speed(X) := 30        @base
rule goblin_speed(X): monster(X) => speed(X) := 30        @base

// Condition tier beats base by the ladder — zero hand-written edges.
rule restrained_speed(X): restrained(X) => speed(X) := 0  @condition
rule heavy_load(X):       encumbered(X) => speed(X) := 10 @condition
    // restrained + encumbered on one actor: two same-band setters would be
    // a conflictable-pair error, but the fluent's `combine min` resolves
    // them to 0 — 5e's "most restrictive applies", declared once (§5.8)

// Immunity tier beats condition the same way. Freedom of movement:
// "your speed can't be reduced" — a negative head, i.e. a value-specific
// defeater (§5.7): it blocks the 0 without asserting a number, and the
// base-tier speed stands. Block + inertia-of-judgment = "unaffected".
rule fom_speed(X): freedom_of_movement(X) ~> ~(speed(X) := 0)  @immunity

// Intra-band `>` (§6.2): both are conditions; the specific interaction is
// 5e's "an invisible creature outlined by faerie fire can be seen".
rule unseen(X):   invisible(X)    => hidden(X)             @condition
rule outlined(X): faerie_fired(X) => ~hidden(X)            @condition
outlined > unseen

// The annotated ladder override (§6.2). Unconsciousness has two supports:
rule sleep_takes(X): slept(X)                   => unconscious(X)  @condition
rule zero_hp_ko(X):  hp(X) <= 0 & ~monster(X)   => unconscious(X)  @condition
// Fey Ancestry — "magic can't put you to sleep" — is a racial trait (base
// tier) that must beat sleep_takes specifically while still losing to
// zero_hp_ko. Band reassignment can't express that pair-scoped shape:
rule fey_ancestry(X): elf(X) ~> ~unconscious(X)            @base
fey_ancestry > sleep_takes  overriding stat_stack
// Unannotated, that edge is a compile error (it contradicts the ladder).
// Team defeat (§5.7) then does the real work: slept(aria) alone -> the
// only applicable supporter is sleep_takes, which no longer beats the
// fey_ancestry defeater -> blocked, aria stays awake. slept(aria) AND
// hp(aria) <= 0 -> zero_hp_ko still beats fey_ancestry by the ladder ->
// unconscious. dl_why names each step: "sleep_takes beaten by
// fey_ancestry — explicit override of stat_stack".

// Landmark guards (§5.8): the compiler harvests hp(X) <= 0 above and the
// half-max threshold below, sorts them, and emits the entailment strictly
// (hp <= 0 -> hp <= hp_max/2), so "at 0 you are also bloodied" is free.
rule bloodied(X):   hp(X) <= hp_max(X) / 2  =>  bloodied(X)
rule goblin_flees(X): monster(X) & bloodied(X) => wants_flee(X)

// ---- actions: the only mutation (I2); effects ride the pipeline (§5.8) ----

action sword_strike(X: actor, Y: actor):
    requires holding(X, longsword) & adjacent(X, Y)
    causes   hp(Y) -= 6

action arrow_shot(X: actor, Y: actor):
    requires holding(X, shortbow) & los(X, Y)
    causes   hp(Y) -= 4
    // both land on one tick? deltas SUM, order-free, and the trace is a
    // receipt: hp'(grunk) = 7 - 6 - 4 -> clamped to 0 by the declared range

action power_word_heal(X: actor, Y: actor):
    requires adjacent(X, Y)
    causes   hp(Y) := hp_max(Y)
    // `:=` competes as a value conclusion (§5.7); if it wins, undefeated
    // deltas still apply: full heal while an aura deals 4 that tick ->
    // hp_max - 4. Pipeline: base (:=) -> Σ deltas -> clamp. Never an order
    // among rules.

action cast_sleep(X: actor, Y: actor):
    requires los(X, Y)
    causes   slept(Y)

// ---- ramifications: the dying trigger, stratified (§5.8) ----

rule monster_dies(X: actor):
    hp(X)' <= 0 & monster(X)  causes  dead(X)'
    // a PRIMED numeric guard — legal because the numeric dependency graph
    // is acyclic (hp' -> hp'<=0 -> dead', nothing downstream writes hp'),
    // so the compiler orders the strata within the tick. A rule that
    // healed on hp' <= 0 would close the loop and be rejected, naming it.

rule death_drop(X: actor, T: item):
    dead(X)' & holding(X, T)  causes  ~holding(X, T)' & on_floor(T)'
    // cascades in the same step as the killing blow (boolean primed guards
    // ride the fixpoint; no stratification needed)

// ---- driving it ----
// No narrative layer (DESIGN.md §12.1). The combat loop is ordinary host
// code against the generated header (§6.3): each turn it reads judgments
// and offers the legal actions, e.g.
//   adjacent(aria, grunk) -> offer sword_strike(aria, grunk)
//   wants_flee(grunk)     -> the goblin's own intent judgment; the driver
//                            may act on it without player input
//   dead(grunk)           -> end the encounter
// This slice is the proof that a game is buildable on judgments + actions
// alone, with no narrative layer (§11, M5).
