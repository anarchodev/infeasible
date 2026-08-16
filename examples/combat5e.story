// combat5e.story — the flagship 5e combat slice, on the SHIPPED M1 surface.
// (Originally a pre-M1 aspirational sketch; modernized once the surface caught
// up — see git history for the original and #8 for the scoreboard.)
// Exercises §5.7 (team defeat, defeaters), §5.8 (value store, expression
// guards, the effect pipeline, the stratified dying trigger), §6.2 (priority
// bands, intra-band `>`), and #82 (derived values, `prior` layers, and
// defeat-the-definitions).
// Obeys the declared-vocabulary discipline (§6.1 item 2): every atom is a
// declared fluent, a provider, or some rule's head.

scene skirmish

sort actor, item

entity (
    aria  : actor          // elf fighter (the player)
    grunk : actor          // goblin
    longsword, shortbow : item
)

// ---- the priority ladder (§6.2): the 5e stack ----

bands stat_stack: base < condition < feat < immunity

// ---- state ----

state (
    // positions the stock grid reads (#255): ordinary numeric fluents, so
    // moving is an ordinary effect and the library needs no host beside it
    grid_x(actor) : int in 0 .. 64
    grid_y(actor) : int in 0 .. 64
    grid_blocks(actor)                  // read by the library's LoS walk
    hp(actor)     : int in 0 .. hp_max(actor)
        // declared range = the outermost clamp stage (§5.8): "any leftover
        // damage is lost" (PHB) is schema, not a rule anyone can forget.
        // The upper bound is DYNAMIC — a per-actor fluent read at commit.
    hp_max(actor) : int
    elf(actor)
    monster(actor)
    restrained(actor)
    encumbered(actor)
    slept(actor)                    // under magical sleep
    invisible(actor)
    faerie_fired(actor)
    freedom_of_movement(actor)
    dead(actor)
    holding(actor, item)
    on_floor(item)
)

init (
    grid_x(aria)=1  grid_y(aria)=1      // adjacent to grunk, and in line of sight
    grid_x(grunk)=2 grid_y(grunk)=1
    elf(aria)              monster(grunk)
    hp_max(aria) = 20      hp(aria) = 20
    hp_max(grunk) = 7      hp(grunk) = 7
    holding(aria, longsword)
    holding(grunk, shortbow)
)

// Spatial guards come from the STOCK GRID (§5.6, #255) — the square-grid
// library shipped with the engine, so this file needs no host code of its own.
// Positions are ordinary numeric fluents the library reads by entity.
provider (
    grid_adjacent(actor, actor)
    grid_los(actor, actor)
)

// ---- speed: a DERIVED value (#82) ----
// The sketch stored speed with `combine min`; the modern form DERIVES it.
// "Two effects set your speed: the most restrictive applies" is a pair of
// min-class layers — same-class layers commute, so no order is needed (#94).

value speed(actor) : int
rule speed_base(X: actor):                      => speed(X) = 30
rule restrained_speed(X: actor): restrained(X)  => speed(X) = min(prior, 0)
rule heavy_load(X: actor):       encumbered(X)  => speed(X) = min(prior, 10)

// Freedom of movement — "your speed can't be reduced" — is DEFEAT THE
// DEFINITIONS (#82): each definition grounds a marker judgment, an ordinary
// defeasible literal, so a defeater against the reducing layers blocks them
// without asserting a number, and the base stands. Team defeat does the
// rest: an unbeaten defeater blocks its target (§5.7).
rule fom_restrained(X: actor): freedom_of_movement(X) ~> ~restrained_speed(X)
rule fom_load(X: actor):       freedom_of_movement(X) ~> ~heavy_load(X)

// a reader, so the derived speed is observable (values are inlined at reads,
// never stored — a judgment guard is how a host or test sees one)
rule rooted(X: actor): speed(X) <= 0 => rooted(X)

// ---- judgments: the 5e stack through bands (§6.2) ----

// Intra-band `>` (§6.2): both are conditions; the specific interaction is
// 5e's "an invisible creature outlined by faerie fire can be seen".
rule unseen(X: actor):   invisible(X)    => hidden(X)             @condition
rule outlined(X: actor): faerie_fired(X) => ~hidden(X)            @condition
outlined > unseen

// Unconsciousness has two supports; Fey Ancestry — "magic can't put you to
// sleep" — must beat sleep_takes specifically while still losing to
// zero_hp_ko. That pair-scoped shape is §6.2's ANNOTATED LADDER OVERRIDE
// (`fey_ancestry > sleep_takes overriding stat_stack`).
// ⟂ GAP — `overriding` is not in the parser yet (§6.2, the one piece of the
// bands design still unimplemented). Until it lands, this cluster stays off
// the ladder and writes the pairwise edges directly — same semantics, minus
// the ladder-contradiction check the annotation is for:
rule sleep_takes(X: actor): slept(X)                    => unconscious(X)
rule zero_hp_ko(X: actor):  hp(X) <= 0 & ~monster(X)    => unconscious(X)
rule fey_ancestry(X: actor): elf(X) ~> ~unconscious(X)
fey_ancestry > sleep_takes
zero_hp_ko > fey_ancestry
// Team defeat (§5.7) then does the real work: slept(aria) alone -> the only
// applicable supporter is sleep_takes, which loses to the fey_ancestry
// defeater -> blocked, aria stays awake. slept(aria) AND hp(aria) <= 0 ->
// zero_hp_ko still beats fey_ancestry -> unconscious. dl_why names each step.

// Landmark guards (§5.8): `hp(X) <= 0` above is a plain threshold; the
// half-max comparison below reads TWO fluents, so it is an EXPRESSION guard.
// (⟂ #130: an expression guard cannot START with a numeric fluent read —
// the parenthesized lead is the shipped workaround.)
rule bloodied(X: actor):   (hp(X) * 2) <= hp_max(X)  =>  bloodied(X)
rule goblin_flees(X: actor): monster(X) & bloodied(X) => wants_flee(X)

// ---- actions: the only mutation (I2); effects ride the pipeline (§5.8) ----

action sword_strike(X: actor, Y: actor):
    requires holding(X, longsword) & grid_adjacent(X, Y)
    causes   hp(Y) -= 6

action arrow_shot(X: actor, Y: actor):
    requires holding(X, shortbow) & grid_los(X, Y)
    causes   hp(Y) -= 4
    // both land on one tick? deltas SUM, order-free, and the trace is a
    // receipt: hp'(grunk) = 7 - 6 - 4 -> clamped to 0 by the declared range

action power_word_heal(X: actor, Y: actor):
    requires grid_adjacent(X, Y)
    causes   hp(Y) := hp_max(Y)
    // `:=` is the pipeline's base stage; undefeated deltas still apply:
    // full heal while something deals 4 that tick -> hp_max - 4. Pipeline:
    // base (:=) -> Σ deltas -> clamp. Never an order among rules.

action cast_sleep(X: actor, Y: actor):
    requires grid_los(X, Y)
    causes   slept(Y)

// ---- ramifications: the dying trigger, stratified (§5.8 / #87) ----

rule monster_dies(X: actor):
    hp(X)' <= 0 & monster(X)  causes  dead(X)
    // a PRIMED numeric guard — legal because the numeric dependency graph
    // is acyclic (hp' -> hp'<=0 -> dead, nothing downstream writes hp),
    // so the compiler orders the strata within the tick. A rule that
    // healed on hp' <= 0 would close the loop and be rejected, naming it.

rule death_drop(X: actor, T: item):
    dead(X)' & holding(X, T)  causes  ~holding(X, T) & on_floor(T)
    // cascades in the same step as the killing blow (boolean primed reads
    // ride the fixpoint; no extra stratum needed)

// ---- driving it ----
// No narrative layer (DESIGN.md §2). The combat loop is ordinary host code
// against the world_* surface (§6.3): each turn it reads judgments and
// offers the legal actions, e.g.
//   grid_adjacent(aria, grunk) -> offer sword_strike(aria, grunk)
//   wants_flee(grunk)     -> the goblin's own intent judgment; the driver
//                            may act on it without player input
//   dead(grunk)           -> end the encounter
// This slice is the proof that a game is buildable on judgments + actions
// alone, with no narrative layer (§11, M5).
