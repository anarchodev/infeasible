# Infeasible — Engine Design

*A narrative game engine in C where the world is a logic database: defeasible
rules for judgments, defeasible inertia for change, and host code driving it
through a generated, vocabulary-checked API. raylib for presentation, CMake
for builds.*

*A narrative/dialogue layer is deliberately **out of scope** (§2). Everything
below is the rules engine.*

---

## 1. Thesis

Games encode enormous amounts of "normally X, unless Y, and Y beats X"
knowledge — status effects, rules interactions, quest logic — and almost every
engine encodes it as imperative flag manipulation, which decays into ordering
bugs and stale-state bugs at scale. Infeasible's bet is that **one
non-monotonic logic can serve as the single semantics for both what is true
(stats, judgments) and what happens next (state transitions)**, executed by
well-understood forward-chaining algorithms, with an Ink-like narrative
language on top that only ever *queries* conclusions and *fires* actions.

The scale target is BG3-class **systemic and narrative depth** (Larian's
Osiris is the existence proof that a rule database can carry a 100-hour CRPG),
at indie-class presentation. The engine's compensating advantage for being
small: every conclusion has a proof tree, so authors get a `why?` debugger no
ad-hoc flag system can offer.

## 2. Goals / non-goals

**Goals**

- One custom language (working name: `.story` files) spanning two layers:
  declarations + rules, and actions.
- Defeasible logic as the *only* inference semantics; forward chaining as the
  *only* execution model.
- Deterministic, serializable, replayable: a save is base facts + action log.
- Author tooling as a first-class deliverable: `why?` proof traces,
  compile-time conflict detection, grounding-cardinality warnings.
- C11, CMake, raylib. Renderer-agnostic logic core (pure functions over fact
  stores; raylib touches nothing below `app/`).

**Non-goals**

- AAA presentation. raylib caps us at indie visuals; the logic core doesn't care.
- General-purpose logic programming. The language is game-shaped; escape
  hatches go through C providers, not logic-side Turing-completeness.
- GOAP-style planning (defeasible action theories make planning research-grade;
  revisit only if NPC planning becomes core).
- A narrative/dialogue layer. Games are built as host code against the
  generated header (§6.3); a knot/choice/divert front end would be a client
  above that surface, and is out of scope. If one is ever added it enters as
  a front end on the interface artifact, keeps computation in rules or
  providers (not the dialogue layer), and re-opens §6.3's total-erasure rule
  deliberately.

## 3. Influences and prior art

- **Osiris (Larian)** — proof that an event-driven, forward-chaining fact
  database scales to a shipped CRPG. Also our catalogue of pains to fix: no
  negation (modders maintain shadow `DB_Not_*` databases), no exception
  priorities (flag gymnastics), manual truth maintenance (stale-fact bugs).
  Osiris is monotonic in-state with destructive assert/retract; Infeasible
  replaces all three pain points with real semantics.
- **Ceptre (Martens)** — linear-logic multiset rewriting for game state; we
  considered and rejected that route in favor of defeasible inertia (see §5.3),
  but its rule-firing-trace debugging culture carries over.
- **Action languages / event calculus** (Gelfond & Lifschitz; 𝒞+; Kowalski &
  Sergot; Shanahan; Mueller) — the frame problem treated via default inertia.
- **Defeasible logic** (Nute; Billington; Antoniou, Billington, Governatori,
  Maher) — the inference core, chosen for its linear-time complexity and
  explicit superiority relation.
- **D&D 5e** — "specific beats general" is the PHB's stated rules-interaction
  principle; the 5e condition/feat/immunity stack is a defeasible theory in
  prose. A 5e-ish combat slice is a milestone acceptance test. Note what 5e
  does *not* do: it states no procedure for deciding which of two rules is
  more specific. Every interaction is spelled out individually — "an
  invisible creature outlined by faerie fire can be seen" is a sentence
  someone authored, not a consequence anyone derived. So "specific beats
  general" is a description of the *outcomes* the designers hand-wrote, not
  an inference rule, and reading it as one invites the lex specialis mistake
  §6.2 refuses. Our bands encode the tiers; the individual exceptions stay
  hand-written, exactly as the PHB has them.

## 4. Architecture

Two tiers, one semantics:

```
┌────────────────────────────────────────────────────────────┐
│  Inference (defeasible logic engine)                       │
│  judgments derived from base facts — never stored          │
│  step function: base facts + actions ─► next base facts    │
│  (causal rules > generated inertia rules, primed atoms)    │
├────────────────────────────────────────────────────────────┤
│  Fact store (EDB)                                          │
│  base facts = the ONLY mutable state; closed-world;        │
│  scene-partitioned; event-driven wake-ups at scale         │
└────────────────────────────────────────────────────────────┘
```

- **Fact store**: interned ground atoms; the single source of truth. Scalars
  (hp, gold) live in a value store beside the atoms; rules see them only
  through evaluated guard literals (`hp(X) < 10`) — see §5.8 for the full
  numeric design.
- **Inference** runs in two modes over the same rule format: *query* mode
  (what is defeasibly provable now?) and *step* mode (given occurring actions,
  what are the next base facts?).
- **Everything above these two tiers is a client**, not a tier: renderer,
  editor, debugger, host game loop. Clients ask (query/`why?`) and do
  (propose actions); they never mutate facts. See §4.2.
- The shape is **model–view–update with a logic database as the model**:
  `world_step` is the reducer (inertia generated instead of hand-copied),
  judgments are selectors (with proof trees), clients are pure views whose
  only write-back channel is `do action`, and wake-ups are the subscription
  mechanism. Exact time travel (§6.1 item 6) falls out because the action
  log is the save format.

### 4.1 Two-tier evaluation at scale

- **Scene tier**: grounding and inference scoped to the loaded
  region/encounter; full recompute per action (turn-based combat gives
  milliseconds of budget; recompute-from-base-facts *is* our truth
  maintenance).
- **Global tier**: the persistent world database (quest flags, faction state).
  Storage and *triggering* are Osiris-shaped: rules subscribe to atoms; a
  base-fact change wakes only the rules in its static dependency cone, which is
  then recomputed defeasibly.
- **Wake-ups fire only into the demand cone.** Subscriptions
  (`world_subscribe`, §11 M2) define a *demand cone*: the backward closure,
  over the same static dependency graph as §5.4's wake-ups, from every
  subscribed literal, every step-rule body literal, and every scope-interface
  literal — closed under attack (a demanded literal drags in its complement's
  rules and the superiority edges among them; defeat is adversarial, so
  relevance must be too, and expect this closure to be bigger than intuition
  suggests — the large-cone subscription warning of §11 M2 covers it). A
  base-fact change then recomputes only conclusions that are both *reachable*
  (forward cone of the change) **and** *demanded*: an unwatched judgment cone
  is never grounded or maintained, no matter how often its inputs flip. This
  laziness is licensed by I1 and applies to judgments only — derived
  conclusions are never stored and never feed state, so skipping one is
  unobservable. The **step-relevant cone is always live**: causal rules,
  ramifications, inertia, and every judgment feeding a step-rule body run for
  the whole scope every step, because state evolution is observer-independent
  (the dead guard drops the torch whether or not any client subscribed to
  `on_floor`), and because §12 lockstep needs per-peer-identical step theories
  while subscriptions are per-client. Storage is likewise never demand-trimmed:
  base facts are the state (I4); what goes lazy is derivation and rule
  matching, never the fact store. `world_query` on an undemanded literal stays
  legal as the slow path — ground and solve its cone on demand — because the
  `why?` debugger, the editor, and §5.3's dry-run query are ad hoc by nature.
  Declared interest buys the incremental path; it does not gate the ask.

This split is an optimization, not a second semantics — see §5.4.

### 4.2 Kernel, driver, clients

The tier diagram above is the *semantics*. The runtime architecture around
it is a kernel with clients:

```
                driver (host loop — dumb transport)
       player input ──┐          ┌── NPC intents (judgments)
                      ▼          ▼
                   action set ──► world_step
                      ▲               │ commit; wake-ups
       clients ───────┘ (propose)     ▼
       clients: host game code · renderer · editor · debugger ◄── query / why?
```

- **The kernel is the world**: facts, rules, step, query. Two ports — ask
  (query/`why?`) and do (actions). Nothing else exists at this layer.
- **The driver decides nothing.** Someone must assemble each step's action
  set; the driver is that owner, and it is deliberately dumb transport:
  collect player input, read intent judgments (`wants_flee(X)` — NPC
  decision-making stays in rules, where it is defeatable and
  `why?`-traceable), assemble the set, call `world_step`, let clients
  re-query. All reasoning lives in rules; the driver is a loop with no
  opinions.
- **Clients propose; they never step.** A client queries judgments and
  *proposes* actions into the driver's set — so every client's `do`
  composes in one fixpoint with everything else that tick. No client calls
  `world_step` itself. This is the middleware posture: the kernel is asked,
  it does not own the caller.
- **Client state lives in the store** — the **externalized-state pattern**,
  and the engine's central offer to any layer built on top. A client that
  keeps its own state between turns forfeits saves, replay, and `why?` for
  that state. A client that models its state as fluents mutated through
  registered actions is stateless between turns, survives saves, replays
  exactly (I4), shows up in the fact-store diff, and gets time travel for
  free. The rule generalizes: **if it is state, it is a fluent; if it
  changes, it is an action.**
- **Triggering runs both directions.** Host-initiated: input plus a gate
  judgment (`can_parley`) → the host acts. Logic-initiated: rules conclude
  what should happen next — `pending_scene(X) := guard_warning`, a
  multi-valued judgment whose values are interned atoms like everything
  else — wake-ups flip it, and the host acts when presentation allows.
  Rules decide *which* and *whether*; the host decides *when* and what it
  means. Competing conclusions are conflicting rules arbitrated by bands
  (`@quest` beats `@ambient`) — a drama manager for free, with proof traces
  for "why did this fire?". Valve's response-rules system (Ruskin, GDC
  2012) is the shipped precedent: most-specific-match bark selection is
  "specific beats general" as ad-hoc scoring; here it falls out of
  superiority. Note this needs **no narrative layer** — `pending_scene` is
  an ordinary multi-valued judgment, and the host is free to render it as
  dialogue, a barked line, a camera cut, or nothing.
- **Clients use zero private APIs** — the acceptance test of this layering.
  Everything a client does (declare fluents, register actions, emit rules,
  query, propose) goes through the public surface every client gets;
  `world.h` already *is* that surface, and the hand-built test worlds are
  the proof. Because there is no blessed client, the test is now carried
  entirely by having a *second* one: M2 ships a trivial second client in
  tests to pin the claim the way golden tests pin semantics (§11).

## 5. Logical foundations

### 5.1 Defeasible logic core

Standard propositional defeasible logic (Billington 1993; Antoniou et al.,
ACM TOCL 2001), ambiguity-blocking, **with team defeat**:

- **Literals**: atoms and their negations (`p`, `~p`).
- **Rules**: strict (`->`), defeasible (`=>`), defeaters (`~>` — can only
  block, never conclude), plus an acyclic **superiority relation** `>` between
  rules with conflicting heads.
- **Proof statuses** per literal: `+Δ/−Δ` (definitely provable / demonstrably
  not, strict rules only) and `+∂/−∂` (defeasibly provable / demonstrably not).
- `+∂q` holds iff `+Δq`, or: some strict/defeasible rule for `q` has all
  antecedents `+∂`, `~q` is `−Δ`, and every rule for `~q` is either
  inapplicable (an antecedent is `−∂`) or beaten (`t > s`) by some applicable
  supporting rule for `q` (team defeat).

Key property (the reason forward chaining is the native execution model): the
logic is **non-monotonic in premises but monotone in computation** — within
one evaluation, literal statuses move irrevocably from *unknown* to *decided*;
nothing is retracted mid-pass. Inference is a fixpoint reached by one
propagation sweep.

### 5.2 Complexity and the compilation pipeline

- Maher, *"Propositional Defeasible Logic has Linear Complexity"* (TPLP 2001):
  all conclusions computable in O(N) via unit-propagation-style forward
  chaining with rule counters and theory simplification.
- Antoniou et al., *"Representation Results for Defeasible Logic"* (TOCL
  2001): size-preserving build-time transformations eliminating defeaters and
  compiling away superiority.
- **Grounding is where cost hides**, not inference. Discipline (all
  compiler-enforced):
  1. *Safety / range restriction*: every rule variable must occur in a
     positive body atom over a finite relation (Datalog safety).
  2. *Typed variables* (`X : actor`) over declared sorts bound every domain.
  3. *Dense computed relations come from providers*: spatial/LOS-style guards
     (`near(X,Y,5)`) are answered by engine-side indices registered in C; the
     logic layer only ever joins sparse results. The logic engine consumes the
     broadphase; it is never the broadphase.
  4. Match against the live fact store at tick time (joins over actual facts —
     cost ∝ matches), rather than pre-grounding the nᵏ cross product.
  5. Cardinality warnings: any rule ranging over a large cross product with no
     sparse anchor is a compile-time warning with the estimated count.

**Scaffold status**: the current C engine implements the standard semantics
with a straightforward tri-valued fixpoint (correct, O(n·rules) worst case)
over pre-ground rules. Maher's counter-based linear algorithm and the
transformation pipeline are planned optimizations behind the same API; the
golden tests pin the semantics so the swap is safe. Cyclic rule graphs may
leave literals undecided; the compiler will stratify/reject cycles.

### 5.3 State and time: defeasible inertia

Defeasible logic defines consequence, not time. Time lives in the fact store:

- **Base facts (EDB)** are the state, **closed-world**: for every declared
  fluent `f`, exactly one of `f` / `~f` is a fact each tick.
- **Derived judgments (IDB)** are a pure function of base facts, recomputed on
  demand, **never stored** (see invariant I1).
- A **step** is one defeasible evaluation over a doubled vocabulary: each
  fluent `f` gets a primed atom `f'` ("next"). Per step we assemble:
  - current facts (+ occurring action atoms as facts),
  - the static judgment rules (over now-atoms),
  - **causal rules**: `conditions & action ⇒ effect'` (defeasible),
  - **generated inertia rules**, one pair per fluent, never author-written:
    `f ⇒ f'` and `~f ⇒ ~f'`,
  - superiority: every causal rule `>` each inertia rule it conflicts with.
  The next state reads off the `+∂`-primed literals; swap buffers. A logical
  double-buffer.
- **Ramifications** (indirect effects — the dead guard drops the torch) are
  causal rules with no action trigger whose bodies may mention primed *and*
  now atoms: `~alive(X)' & holding(X,T) ⇒ ~holding(X,T)' & on_floor(T)'`.
  Cascades happen inside the same fixpoint. This is where the design beats
  STRIPS add/delete lists.
- **Conflict = authoring error**: if neither `f'` nor `~f'` is provable
  (two applicable causal rules with no superiority), the step function rejects
  the step and reports the fluent and the fighting rules. Detected at run
  time now; the compiler will detect conflictable pairs statically.
- Sanity anchor: the **Yale shooting problem** behaves correctly by
  construction — inertia on `loaded` is only defeated by a rule that actually
  fires against it; nothing fires during `wait`. This is a golden test.
- **Reactions are a host protocol, not new step semantics.** A step is
  one-shot: an action set goes in, the next state comes out. Interrupts that
  fire in response to an action and change whether it lands (5e Shield turning
  a hit into a miss, Counterspell nullifying a cast) are a *driver* two-phase
  drive — propose an action, let the host read the resulting judgments, offer
  reactions that add facts or actions, then commit the resolved step — layered
  above `world_step`, needing no `.story` syntax. The one core affordance it
  requires is a **dry-run query**: the host can evaluate a proposed action's
  judgments (its would-be hit/miss) *without committing* the step. Shield then
  needs nothing special — it sets a temporary AC-boost fluent and the hit
  judgment (never stored, I1) recomputes on the committed step; its duration is
  the host turn-counter of §5.10's `tick`.

### 5.4 Why the two-tier optimization is semantically invisible

- **Defeat is non-monotonic, but dependency is monotonic.** Whether rule R can
  influence literal p is a property of the static rule graph (bodies, heads,
  superiority edges, provider inputs), independent of who wins at runtime. The
  reachable cone of a base-fact change is therefore a *sound
  overapproximation* of every conclusion that could change. Event-driven
  wake-ups are monotonic *reachability*, not a second logic; all conclusions
  are still made by the defeasible pass. (Same shape as delete–rederive in
  incremental Datalog: overapproximate the damage monotonically, recompute
  exactly within it.)
- **Scopes must be dependency-closed.** Scene-scoped recompute is sound iff no
  rule's cone crosses the scene boundary. The compiler verifies declared
  partitions against the dependency graph (or derives partitions from its
  block structure). Cross-scene influence travels as explicit base facts
  through declared interfaces — which is the module system.

### 5.5 Nested scopes: lifetime and visibility

The two-tier split of §4.1 partitions the world for *recompute cost*. Authors
also want to partition it for *lifetime and visibility*: global quest/faction
state that persists in the save, area state that lives while a region is
loaded, encounter state that is born when a fight starts and gone when it
resolves. These are the same construct — a **nested scope** is at once a
dependency partition (§5.4) and a lifetime — but the two axes are worth keeping
distinct, because the partitioning half is already proven semantically
invisible while the lifetime half is new surface.

A scope tree, outer to inner: `world` (global, persistent) ⊃ `area` (loaded)
⊃ `encounter` (ephemeral). Nesting generalizes the flat `scene` of §4.1 from a
single partition to a tree; it is **not** a second semantics. (Keywords here
are tentative; M1 fixes the syntax.)

**The discipline (lexical scope + an effect rule):**

1. **Scopes do not share vocabulary; reading down is a generated defeasible
   import.** There is no atom `can_force_door` — there is
   `world:can_force_door` and `cellar_fight:can_force_door`, and they are
   *different atoms*. An inner rule that names an outer atom does not reach
   the outer atom; the compiler emits an import rule
   (`world:f => cellar_fight:f`) and rewrites the reference to the local
   twin. Inner scopes see outer conclusions only through those imports; the
   reverse is forbidden. Authors write the unqualified name and never see
   the machinery (§6.1's cross-cutting rule); provenance renders it —
   "import of `can_force_door` from `world` (generated; declared
   world.story:12)".
2. **Imports lose to local rules** (generated superiority, as inertia loses
   to causal rules). This is the whole point of 1: an encounter that
   concludes `~can_force_door` beats its own *import*, not the world's rule.
   The world's answer is unchanged, the encounter's differs, and nothing is
   contested — "in this fight the rules are different" without reaching into
   another scope's rulebook, and without naming a rule the scope does not
   own.
3. **Outer facts are pinned during an inner pass.** While an encounter's
   defeasible pass runs, every fact it reads from an enclosing scope is an
   immutable input — nothing outer changes mid-fixpoint. The inner cone reaches
   outward but only across a frozen boundary, so the scoped recompute is sound
   for exactly the §5.4 reason: dependency is monotonic, and a pinned input
   cannot be part of the reachable-change cone of the inner step.
4. **Write only your own scope — facts *and* judgments.** A step commits
   `+∂`-primed literals only for fluents declared in the scope it runs in,
   and a rule concludes only into its own scope's vocabulary. Both halves of
   the store obey one rule: this is what point 1 buys.
5. **Escalate outward only through a declared action.** The single way an
   inner scope affects an outer one is by firing an action whose effect lands
   on an outer fluent — a declared interface, which is what §5.4 already calls
   the module system. This keeps I1/I2 intact: cross-scope influence still
   travels as base facts through the step function, never as a stored
   conclusion and never as a silent write.
6. **Outer changes wake inner subscribers.** The global-tier subscription
   mechanism of §4.1 points *downward*: a world-fact change wakes the inner
   scopes whose static cones include it, and they recompute defeasibly.

**Import is inertia across space.** Rules 1–2 introduce no new concept; they
are §5.3's construct pointed at the other axis:

| | generated default | beaten by |
|---|---|---|
| **inertia** (time) | `f => f'` | causal rules |
| **import** (space) | `world:f => encounter:f` | local rules |

"Things stay as they were unless something changes them" and "outer truth
holds inside unless something local overrides it" are the same sentence.
Same generated rule, same generated superiority edge, same erasure to plain
DL, same provenance obligation. Private vocabulary is what lets an encounter
override locally without contesting the world and without naming a rule it
does not own (the §6.2 asymmetry would otherwise cross the scope boundary):
the inner rule beats its own import, and `world:can_force_door` is untouched.
The construction is Bikakis & Antoniou's **Contextual Defeasible Logic**
(§14): local theories, defeasible mappings,
contexts ranked for the conflicts that survive.

**This applies to the scope axis only.** Extending a module (§6) shares
vocabulary deliberately and is *not* affected — see there for the two verbs
and why they differ.

**Lifetime falls out of vocabulary, not of extra machinery.** An encounter's
fluents exist only while the encounter does; at teardown the vocabulary
disappears, so there is nothing to carry inertia across the boundary — the
frame problem (§5.3) never crosses a scope, because primed atoms are always
same-scope. World fluents keep their inertia in the persistent tier. A scope's
lifetime is therefore the lifetime of its declared fluents, no more.

The same fact answers runtime creation: **spawning is scope instantiation.**
Summons and reinforcements never insert vocabulary at runtime (key sets are
frozen at build, §5.8); an encounter *template* scope declares its entities,
and "summon" loads an instance — the vocabulary arrives whole,
arena-allocated, and despawn is teardown. Dynamic creation without dynamic
vocabulary. Instantiating a template more than once raises an
entity-identity question (two wolves from one template need distinct ids) —
open, §13.

**Unloading is a reachability optimization, and it is provably invisible.**
§5.4's argument — dependency is monotonic, scopes are dependency-closed —
was made to justify scoped *recompute*; it extends verbatim to *existence*:
if no in-scope evaluation can reach an unloaded scope's facts except
through its interface, not loading it is unobservable. **A scope at rest is
its interface facts.** The area exports `gate_open`; while unloaded, that
fact lives on in the outer tier and outer rules keep working, while the
interior facts do not exist in memory — virtual-memory semantics for the
fact base, with the interface as the resident set. Unloading is nearly
free, and I1 is why: the persistent footprint is base facts and values
only (conclusions recompute on reload; rules and bytecode are static
content), so suspension is "serialize a few arrays, release the arena."
And the optimization is *checked*: a rule reaching past an interface is a
compile-time partition violation, not a runtime "actor not found" — the
bug class where open-world engines bleed. One dependency graph now serves
four masters: wake-ups (§4.1), partition checking (§5.4), load soundness
(here), and the LSP's cone queries (§6.1 item 7). Prior art: region-based
memory management (Tofte & Talpin) — lifetimes as lexical regions,
whole-region deallocation, statically checked escapes. Arena-per-scope
plus the closure check is region typing for facts, and the classic
region-incompatibility weakness (a value that must outlive its region) is
answered here by escalation *copying outward* through a declared action
rather than referencing inward.

**Implementation shape (M4).** The current API is flat — one `world`, one
`intern`, one fact set. Two representations:

- *(A) nested `world`s* — child holds a parent pointer, queries walk up for
  unresolved facts, one arena per scope makes teardown a single arena release.
  Clean lifetime; awkward when one pass needs several scopes at once.
- *(B) one theory, scope-tagged atoms* — all facts in a single store, each atom
  carrying a scope id; one `dl_solve` sees the visible union; teardown drops
  every atom with that tag. Faithful to "one logic, partitioned," and lets
  superiority cross tiers within a single pass.

Take the **hybrid**: logically (B), so a single defeasible pass spans the
visible stack and the generated imports (points 1–2) are ordinary rules in
one theory; physically (A)'s arena-per-scope, so an encounter tears down in
one free and solve stays allocation-free (§7). The scope id is a small tag on
the atom — which is also what makes `world:f` and `encounter:f` distinct
atoms in the first place, and what gives scope *instances* their identity
(§6.4); grounding and wake-up respect it.

**Golden test to pin the meaning** (mirroring how Yale-shooting pins inertia):
an encounter fluent set true does **not** survive the encounter's teardown,
while a world fluent set true from inside the encounter (via a declared
escalating action) **does** persist after teardown — and an inner rule reading
an outer fact produces the same verdict whether evaluated scoped or whole.

**Stress test: sectors (MMO/open-world scale).** Recorded not as new
mechanism but as evidence the constructs compose — three classic large-world
problems reduce to what is already on this page:

- *Sectors are sibling `area` scopes.* "Process only active sectors" is scope
  loading; a sector at rest is its interface facts.
- *Crossing is a handoff, not a move.* An entity's sector-local fluents live
  in the sector's vocabulary, so migration is an escalation action carrying
  the survivable facts outward (rule 5), the source sector freeing its pool
  slot, and the target sector's spawn action (§5.9's complete-effect-list
  reset) instantiating them — two actions in two logs, replay-exact,
  `why?`-traceable, no reference ever dangling across a boundary. The
  identity that survives the hop is §13's cross-scope-identity question in
  MMO clothes, and this case argues for its shape: durable id owned by the
  outer tier, sector-local state keyed by pool slot.
- *The border problem dissolves into co-scoping.* Combat straddling a sector
  line is a classic headache because interaction is ambient in most engines.
  Here two entities contest conclusions only inside a shared scope, so a
  cross-border fight *forces* an encounter scope spanning members of both
  sectors: sectors are the storage/paging partition, encounters are the
  interaction partition, and they may disagree. Overlap zones and handoff
  hysteresis are replaced by the partition checker — reaching across an
  interface is a compile error, not a ghost-entity bug.
- *Sibling scopes may step concurrently.* Siblings share no vocabulary; their
  only common dependency is the outer tier, pinned during their passes
  (rule 3) and mutated only through escalation actions. Dependency closure
  *is* the isolation proof, so sector steps parallelize lock-free. The one
  new obligation is I4's: escalations arriving at the outer tier from
  concurrently-stepping siblings need a **canonical merge order** (e.g.
  sector id, then log position — never arrival time) so replay is
  independent of scheduling. Open, §13; decide with M4.

### 5.6 Space and movement

A grid or hex battlefield is the canonical *dense computed relation* §5.2
exists to keep out of the logic core. The design's position is a boundary, not
a feature: **the logic layer is spatially blind.** It knows an actor's cell and
can *ask* about proximity, but it never enumerates cells, never knows hex from
square, and never pathfinds. That blindness is what keeps a BG3-scale
battlefield inside the linear inference budget (§8).

The integration splits along the EDB / provider line:

- **Position is a base fact; movement is an action.** An actor's location is a
  **functional fluent** `at(X) : cell` — exactly one value, not a boolean.
  Movement is an ordinary causal rule (I2): `move(X, dir)` `requires` a
  passable neighbor and `causes at(X)' = neighbor(at(X), dir)`. The grid never
  mutates itself; every position change goes through the step function, so it
  is replayable (I4) and `why?`-traceable like any other fact. **Hex vs. square
  is just the neighbor function inside the provider** — the logic never sees the
  difference.
- **Spatial relations are providers over the position fluents.** Adjacency,
  distance, and line-of-sight (`adjacent(a,b)`, `near(X,Y,2)`, `los(a,b)`) are
  answered by a C-side index built from `at(·)`, registered per §5.2 point 3.
  A move updates positions (base facts) → the index updates → **I3 invalidates
  exactly the guard cones that touched it** → those rules recompute. The logic
  consumes the broadphase; it is never the broadphase.
- **Grounding stays sparse or it warns.** "Enemies within 2 tiles go alerted"
  is anchored by the sparse `near(X,Y,2)` provider result (§5.2 points 3–4).
  The same rule written *without* a spatial anchor ranges over the actor cross
  product and is a compile-time cardinality warning — never a silent nᵏ
  blow-up.
- **Pathfinding is a provider service, not logic.** A* returning a path is a
  provider answer ("path from a to b"); the *logic* only ever fires the
  resulting `move` one step at a time and reasons about that step's
  consequences. Multi-step spatial planning stays on the C side — treating it
  as defeasible action theory is the GOAP research hole §2 rules out. The index
  and any pathing must be seeded and float-free to preserve I4.

**Functional fluents are the one new primitive this forces**, and they are
now pinned: `at(X) : cell` is a multi-valued fluent (§5.7) over an entity
domain, store-backed per §5.8's implementation shape — one `uint32` per
actor, never |cells| atoms. Closed-world (§5.3) generalizes from "`f` or `~f`" to "exactly one
`at(X)=c` per tick"; inertia generalizes to "position persists unless a move
fires" — Yale-shooting for coordinates, correct by the same construction.
Space is the construct's first heavy consumer.

**Scale and scopes fit without new machinery.** The active map is a scene-tier
concern; loaded actors' positions are scene facts. An encounter grid is a set
of encounter-scoped position fluents (§5.5) that free on teardown. Off-screen
crowds stay renderer-side until explicitly promoted into the fact store (§8).

**Golden test to pin it:** a `move` action changes exactly one actor's cell and
leaves every other position inert across the step (spatial Yale-shooting); a
proximity rule fires iff the provider reports the actors within range, and
recomputes when a move changes that range (I3); and a rule with no spatial
anchor over a populated grid raises the cardinality warning rather than
grounding the cross product.

### 5.7 Multi-valued fluents and defeat across values

Fluents generalize from booleans to **finite domains**: `door : { locked,
closed, open }` declares a fluent whose value is exactly one element of its
domain each tick; a boolean fluent is the two-element special case. The
construction is 𝒞+'s multi-valued fluent constants (Giunchiglia et al. 2004):
closed-world generalizes from "`f` or `~f`" to "exactly one `f=v`", and each
fluent compiles to one propositional atom per domain value plus mutual
exclusion — after compilation the engine is still propositional DL and Maher's
linearity result still applies. (That is the *semantics*; §5.8's
implementation shape lets the compiler back a fluent with a value-store slot
instead of atoms when no judgment rule concludes its values.) Functional fluents (`at(X) : cell`, §5.6) are
this with an entity domain. Inertia gets *cheaper*, not more complex: since
exactly one value holds, the step generates a single inertia instance per
fluent — `f=v ⇒ f'=v` for the currently held `v` — with the usual
causal-beats-inertia superiority. Numeric fluents are **not** this construct;
they never become atoms at all (§5.8).

What 𝒞+ cannot decide for us — it has no superiority relation — is what
defeat means among more than two competitors. Decisions:

- **Attack = concludes a different value; team = concludes the same value.**
  `+∂(f=a)` holds iff `+Δ(f=a)`, or: some strict/defeasible rule for `f=a` is
  applicable, no other value is `+Δ`, and every rule concluding a different
  value (or `~(f=a)`) is inapplicable or beaten by some applicable rule for
  `f=a`. At `|domain| = 2` this is verbatim ABGM team defeat (§5.1) — the
  existing golden suite pins the general definition automatically, and that
  degeneration is the acceptance criterion for any implementation.
- **Strict teams, not coalitions.** With three or more values it becomes a
  real question whether an attacker of `a` may be beaten by a rule from a
  *third* value's team ("coalition defeat"). Example: `r1 => f=a`,
  `r2 => f=b`, `r3 => f=c`, with `r1 > r3 > r2` and no `r1 > r2` edge, all
  applicable. Coalition semantics quietly proves `f=a` (`r3`, though itself
  defeated, still beats `r2` on `a`'s behalf); strict teams reject the state
  as contested. We choose **strict**: priority bands (§6.1 item 3) are totally
  ordered, hence transitive, so for banded rules the highest applicable band
  beats every attacker directly and the two semantics agree; the only programs
  they disagree on are intransitive hand-written chains — almost always a
  forgotten edge, which should be a legible error ("`r1` doesn't beat `r2` —
  missing `r1 > r2`, or assign bands?") rather than a silently clever answer.
  Same tiebreaker as §5.5: prefer the variant whose failure mode is legible.
- **Negative heads are value-specific defeaters.** `~(f=v)` cannot be
  *concluded* ("some other value" is a disjunction; DL has none) but is
  coherent as an attacker: it conflicts with `f=v` and nothing else — not with
  `f=u`. A rule with a negative head may win by superiority, and winning only
  ever means blocking — so it *is* a defeater on that value; the surface forms
  `~> f=v` and `=> ~(f=v)` collapse into one construct. A negative head
  withdraws the **whole assignment** it attacks — see reification below.
- **Assignments are reified; the family is the unit of defeat** (decided;
  `tests/test_multival.c`). The naive erasure leaks: `sealed ~> ~(door=open)`
  blocks the primary `open'`, but the assignment's sibling shadow `~locked'`
  shares the body, stays applicable, and still defeats inertia — so block +
  frame axiom compose into a **contested step**, not the "sealed blocks
  open, inertia keeps locked" the prose intends. That is an erasure defect,
  not a semantics to teach around: the author wrote *one* assignment, and
  half an assignment is not a state the surface language can express. Every
  other optimization here is semantically invisible (§5.4); this one was
  not. The fix is one more erasure layer, not an engine change — each
  assignment gets a fresh atom, `body => fires_R`, with the value shadows
  hanging off it (`fires_R => open'`, `fires_R => ~locked'`) and negative
  heads retargeted from the value to `fires_R`. Blocking `fires_R` withdraws
  the family whole; it stays propositional and linear. Consequences, all
  pinned by tests:
  - Conflict and superiority stay at the **value** level. `fires_A` and
    `fires_B` do not conflict with each other, so exclusion must not migrate
    up to the family atom — a flip-flop would then silently commit instead
    of being rejected.
  - The unit is the **assignment, not the rule**. `causes door := open,
    lamp := fallen` is two families; a defeater on `door=open` withdraws the
    door family and the lamp still falls. This is the minimal repair of the
    leak: it keeps negative heads value-specific, as their definition above
    requires, instead of silently suppressing effects the defeater never
    mentioned. All-or-nothing across a rule's effects is what `requires` is
    for.
  - Only domains of 3+ values reify. A boolean erases to a single head with
    no sibling shadow, so the boolean-degeneration criterion above holds
    untouched.
  - Cost is one atom and one rule per multi-head assignment instance, which
    lands on grounding cardinality — measure it in M1 (§8) rather than
    assume it.
  `requires ~sealed` remains the right pattern for a hard precondition, and
  behaves identically under either encoding.
- **At most one value wins.** Under strict teams and acyclic superiority, two
  values both `+∂` would each need to beat the other's applicable supporters —
  a superiority cycle. The step function's read-off relies on this; the engine
  asserts it. "No value `+∂`" remains the §5.3 contested-step error,
  generalized.

**Golden tests — implemented in `tests/test_multival.c`** via the erasure
encoding (per-value atoms; rule families with same-body shadows against
sibling values; mirrored superiority; exactly-one-value facts; no strict
exclusion axioms, which would cycle): the multi-valued flip-flop (step
rejected, state untouched; single writer commits exactly-one); the sealed
door **trio** (naive erasure pins the leak that motivates reification;
the reified version pins the decided semantics — family withdrawn, inertia
holds, and the normal override restored when unsealed; the
requires-condition version pins the hard-precondition pattern), plus
reified conflict survival (two families fire, values still contest, and a
mirrored superiority still resolves); the intransitive chain
leaving no value provable — with the deadlock landing exactly on the
would-be winner, which is why "add `r1 > r2`" is the right compile
diagnostic — while bands 3/2/1 resolve cleanly with at most one winner (the
strict-vs-coalition decision, executable); and two-value domains collapsing
to the boolean suite's behavior.

### 5.8 Numeric fluents: a value store, not a solver

`hp(actor) : int` is *not* a big domain — numbers never become atoms. The
prior art splits cleanly: single-valued fluents over small finite domains are
solved (§5.7); unbounded numerics are never solved *inside* a grounded logic,
only escaped. Bounded grounding (CCalc's declared `0..20` ranges) is the
toy-problem regime; the mature systems (ASPMT / functional stable models —
Bartholomew & Lee; Lee & Meng 2013) stop grounding and delegate arithmetic to
an SMT solver. But a solver answers "what values would make this true?" — a
search this engine never performs (§2 rules out planning). A game engine only
asks "given these values, what is true?" — evaluation. So the numeric layer
keeps the DPLL(T) *interface* — opaque guard atoms at the boundary — with a
lookup where the solver would be:

- **The value store is numeric EDB.** Base values are the only mutable
  numeric state, written exclusively by step effects (I2); guard atoms are
  derived, never stored (I1); a save is base facts + base values + action log
  (I4). Scalars never enter the intern table.
- **Landmark abstraction** (predicate abstraction: Graf & Saïdi 1997; quantity
  spaces: Forbus 1984). The compiler harvests every comparison guard over a
  numeric fluent — the predicate set *is* the set of guards the author wrote;
  nothing to discover — and mints one guard atom per threshold per ground
  instance. To the solver these are strict inputs: asserted closed-world each
  evaluation (never UNDECIDED), usable as antecedents, never concluded by any
  rule. On a value change the provider re-buckets (one binary search over the
  fluent's sorted thresholds) and only the atoms that actually flipped root
  invalidation cones (I3) — chip damage that crosses no threshold wakes no
  rules.
- **Generated entailment rules.** Thresholds are ordered constants, so the
  compiler emits the ordering as strict rules (`hp<=0 -> hp<10`). Arithmetic
  entailment becomes a finite strict chain: visible to defeat, traced by
  `why?` like any rule, still linear. This is SMT-style theory propagation,
  compiled statically. The same pass flags unsatisfiable guard conjunctions
  (`hp<5 & hp>=10`) for free. Entailment across *different* fluents stays
  invisible by design — if "badly wounded implies wounded" matters, the author
  writes that rule. `why?` traces bottom out at the guard with the evaluated
  value ("`hp(guard) < 10` — value store: 8"), so the moat has no hole at the
  numeric boundary.
- **Effects are a closed operator set** (PDDL 2.1-shaped: `:=`, `+=`, `-=`;
  exact set is M1 syntax), executed against the value store at commit time.
  Right-hand sides are expressions through an ordinary expression compiler —
  constant folding, then bytecode on a small expression VM (M1);
  AOT-to-C stays open for shipping builds. **Integer/fixed-point only — no
  floats in the core.** Cross-platform FP divergence (FMA contraction,
  reassociation, libm variance) breaks exact replay, and threshold comparison
  is a divergence *amplifier*: one ulp of drift flips a guard atom, which
  flips a verdict, and the replay is a different story. Floats live on the
  renderer side of the I4 wall, which is the same line as the raylib wall.
- **The compiler may solve; the engine only evaluates.** Build-time
  diagnostics (conflictable-pair witnesses §6.1 item 4, vacuous guards) are
  satisfiability queries — cheap and decidable over finite interval
  partitions. Determinism and the microsecond budget bind at runtime only.

**Stratified primed numeric guards.** A tick is *evaluate* (arithmetic →
guard-atom facts) → *propagate* (pure table-driven fixpoint, no arithmetic) →
*commit* (effect arithmetic against the value store). A ramification guard
over a *primed* numeric ("if `hp' <= 0` then `dead'`" — the dying trigger)
breaks the sandwich: the guard needs next-state arithmetic mid-fixpoint.
(Boolean ramifications are unaffected — the fixpoint is their evaluator; the
problem is only that numeric guards are answered by a foreign oracle. Primed
guards over *multi-valued* fluents — "if `door'=open` then …" — are likewise
free: §5.7 fluents compile to propositional atoms, so the fixpoint evaluates
them like any boolean; no stratification needed. A golden test pins this.)
Decision — **layer it**, the same hammer as §5.2's cycle rejection: the
compiler builds the dependency graph among numeric fluents through primed
guards; if acyclic, it orders strata within one tick — settle every rule that
can write `hp'`, compute `hp'`, assert its primed guard atoms, resume
propagation downstream — so arithmetic still never runs *inside* a propagation
stratum; the phases repeat per layer. If cyclic, compile error naming the
loop: primed-numeric cycles genuinely oscillate ("heal if `hp'<5`, curse if
`hp'>=5`") and have no answer to converge to. A program with no primed
numeric guards is the degenerate one-stratum case.

**Concurrent effects: combine by operator class, never by order.** Two causal
rules writing the same numeric fluent in one step (two damage sources on one
tick) is normal gameplay, not an authoring error — but any answer that picks
an order among *rules* (declaration order, commit order, timestamps) is the
Osiris disease reintroduced. The resolution is a fixed order among operator
*classes* — arithmetic precedence, not execution order — with order-*freedom*
inside each class:

- **Absolute effects (`:=`) are value conclusions.** "Set hp to 10" claims a
  value exactly as `door=open` does, so `:=` effects compete under §5.7:
  strict-team defeat, bands, superiority; two unresolved `:=`s on one fluent
  = contested step, statically detectable as a conflictable pair. No new
  semantics.
- **Relative effects (`+=`, `-=`) combine by summation** (additive fluents:
  Lee & Lifschitz 2003) — the genuinely new class, possible only because
  numeric domains have group structure. Addition commutes, so contributions
  from distinct rules sum order-free, and the `why?` trace is an itemized
  receipt: "hp' = 5: base 12, −3 (goblin_stab), −4 (fire_aura)". A defeated
  effect of either class contributes nothing (defeat is all-or-nothing).
- **The pipeline** is global, fixed, and small: *base* (winning `:=`, else
  inertia) → *Σ undefeated deltas* → *clamp to the fluent's declared range*
  (`state hp : int in 0..20` — the schema is the outermost clamp, so
  explicit min/max effects are rarely needed). Full-heal while standing in
  fire gives full − 4, deterministically, every contribution named.
- **Admissibility criterion for the closed operator set**: an effect operator
  (or collision resolver) is admissible iff its combine is commutative and
  associative. `sum`, `min`, `max` qualify; "first" and "latest" never do.
  Multipliers get no `*=` stage — whether a game wants `(base+adds)×mult` or
  `base×mult+adds` is game-specific, so multiplication lives in effect
  *expressions*, where the author has parentheses.

Prior art: MTG's layer system (Comprehensive Rules 613) resolves simultaneous
continuous effects by a fixed global pipeline with *setting* effects before
*additive* ones — set-before-add, shipped for decades in the most
rules-lawyered game in existence. Its cautionary half is timestamps
(order-among-rules within a layer, and exactly where the confusing judge
calls live): we take the layer pipeline and refuse the timestamps. PDDL 2.1
similarly forbids simultaneous `assign` + `increase`; superiority lets us be
slightly more generous.

**Escape hatches** — flexibility lives in *what* composes and *who wins*,
never in *when* anything ran. Parentheses work in arithmetic because one
author owns the whole expression; cross-rule composition has no such owner,
so the hatches are shaped for strangers:

1. *Within one rule*: effect RHSs are full expressions —
   `causes hp := max(1, hp - damage)` — the parenthesis, for single-author
   composition.
2. *Suppression across rules*: defeat. "Heal ignores this tick's damage" is
   the heal rule beating the damage rules (bands/superiority), already
   traced by `why?`.
3. *Per-fluent collision resolution*: a fluent may declare a commutative
   resolver for same-stage `:=` collisions — `state speed : int combine min`
   gives 5e's "two effects set your speed: the most restrictive applies".
   Static, declared in one place, admissibility-checked, named in the trace.
4. *Bespoke pipelines are modeled, not configured*: a real damage pipeline
   (base → resistances → vulnerability → clamp) is written as derived
   judgment values (`incoming_damage(X)` through the ordinary defeasible
   layer, bands arbitrating modifiers) committed by a *single* effect — the
   MTG move of writing CR 613 as rules. Costs no engine feature; `why?`
   traces every stage because every stage is a rule. Worked example —
   **typed damage**, the 5e/BG3 shape: damage types are a small closed
   sort, so the pipeline is one judgment per type. Attack rules conclude
   base values into `incoming_fire(X)`, `incoming_acid(X)`, …; resistance,
   immunity, and vulnerability are ordinary defeasible rules whose
   rewritten value beats the base conclusion (bands/superiority); one
   commit effect sums the types into `hp(X) -=`. Every stage being a rule,
   the trace reads "fire 8 → 4 (`tiefling_resistance` beats
   `base_fire_damage`)" — richer than the tooltip it imitates, and each
   roll is §5.10-site-keyed, so the receipt can show the dice.

**The receipt is structured data, not only a rendering.** BG3-style floating
combat text — every hit displaying its source and damage type — is a *view of
the commit receipt*. The commit already computes the multiset of undefeated
contributions in order to sum them, so each step's subscription delta (§11
M2) carries them as data: per changed value, the winning base, then each
contribution with its ground source rule (provenance retains bindings — the
M1 constraint), its type/stage tags, and its pre-defeat value where a
modifier rewrote it. This keeps the renderer a pure query client (§8); the
alternative — re-asking `why?` after each step and parsing the trace — would
make the trace load-bearing as a *string format*, the wrong coupling.
Cross-step tallies (a multi-attack sequence's running total) are
renderer-side arithmetic over successive receipts and touch no semantics.
And the tempting alternative is deliberately rejected: an accumulating
damage *buffer* that rules append to and a later phase drains is mutable
intermediate state with an ordering — the Osiris disease wearing a queue
costume. The "list of damage to apply" is a projection of one fixpoint's
winners, never a store.

Deliberately not offered: per-rule stage reordering (recreates the conflict
one level up), timestamps in any costume, and a content-configurable global
pipeline (fixed stage order is why the system is learnable; MTG's pain is
the timestamps, not the fixedness).

**Implementation shape: the value store is entity-indexed arrays.**
Interning makes the obvious "map" degenerate into something better: entity
domains are finite and declared, ids are dense `uint32`s, so a one-key
fluent (`hp(actor) : int`, `at(X) : cell`) is a flat array indexed by entity
id, allocated once from the owning scope's arena at world build — no
hashing, no allocation in the solve loop, teardown rides the scope's arena
release (§5.5), and serialization is an integer array dump (I4-exact). Each
fluent carries a second buffer: effects write `next[]`, commit swaps — the
§5.3 logical double-buffer made literal on the numeric side. The key set is
**frozen at build time**: a slot exists because a fluent and its keyed
domain were declared; there is no runtime key insertion — dynamic vocabulary
would break closed-world assembly and the orphan pass. Compound keys
(`distance(X,Y)`) stay provider territory; store fluents take one entity key
in M1.

Dense arrays are the base representation; a fluent may declare a
**default** (`state terrain(cell) : tile default floor`), backing it with
exceptions only — storage and serialization hold just the cells that
differ, and the editor emits init facts only for the interesting ones.
"Absent means default" is the closed-world assumption wearing storage
clothes; dense vs. default-plus-exceptions is one more backing the compiler
picks invisibly (by domain size and init density). This is what lets a
pico-8-scale tile map — terrain as a store-backed fluent keyed by cell,
mutable only through actions (destructible walls with inertia, exact
replay, and `why?` traces), read directly by the renderer as its tilemap —
live in the world model without ceremony, while sprite *pixels* stay out
(§8).

**A third backing: columnar evaluation of homogeneous families (speculative,
post-M3).** ECS is relational algebra — components are columns, entities are
rows — and grounding already produces the correspondence: a judgment over a
sort (`near(X, player) & ~asleep(X) => alerted(X)`, `X : actor`) grounds to N
structurally identical rule instances differing only in the entity index.
When a family is *homogeneous* — one rule-graph shape, same attackers, same
superiority edges, only the fact bits differing per instance — the fixpoint
can run set-at-a-time instead of tuple-at-a-time (semi-naive Datalog's move):
lift the M3 counters to vectors indexed by entity id, pack boolean fluents
over the sort into bitvectors, and a propagation round over the family
becomes word-wide AND/OR/NOT over proved⁺/proved⁻/undecided masks — 64
entities per instruction, the RTS-crowd regime, on the entity-indexed arrays
this section already mandates. Heterogeneity (a rule naming `goblin_3`, a
per-instance superiority override) partitions the family exactly as
default-plus-exceptions partitions storage: the bulk goes columnar,
exceptions fall back to scalar propagation; cross-entity joins stay provider
territory (sparse pairs scattered into the columns). Whole-family recompute
tensions with sparse wake-ups the same way scene-tier full recompute tensions
with demand tracking (§8), and resolves the same way: at large homogeneous N
the branchless sweep beats the bookkeeping, and the compiler picks per
family, invisibly (§6.1's cross-cutting rule). `why?` survives because
judgments are pure — one instance re-derives scalar-style on demand.
Semantics untouched: one more backing, pinned by the same golden tests that
keep the M3 swap honest.
*Status*: prototyped in `src/logic/dl_col.c` — the tri-valued fixpoint with
statuses lifted to entity-indexed bitvector columns and the ts_min/ts_max
algebra lifted to Kleene 3-valued AND/OR on (true, false) mask pairs.
`tests/test_col.c` pins it differentially: a schema exercising strict
chains, team defeat, defeaters, unresolved conflict, and negative body
literals must match `dl_solve` bit-for-bit per entity, tail words included.
`bench_col` (Release, per-unit AI family of 10 rules): full-family
recompute at 10k units 0.06 ms, 100k units 0.6 ms, 1M units 8.2 ms —
vs 5 ms / 65 ms / 667 ms for the same workload grounded into the scalar
solver (~80–110×), putting RTS-crowd judgment eval well inside a frame
budget. Heterogeneous partitioning, store-fluent guards, and the world-tier
step integration remain open.

The store also generalizes §5.7's implementation: exactly-one-value fluents
admit two faithful backings of the same semantics. *Logic-backed* — one atom
per domain value plus exclusion — is required when judgment rules *conclude*
values defeasibly mid-fixpoint (`stance(X) = aggressive` as a derived,
defeatable judgment). *Store-backed* — one slot, with equality guards
(`door = locked`) minted landmark-style only for the values rules actually
mention — costs one word per instance, and loses nothing for writes because
this section already resolves `:=` competition at the *rule* level, not the
literal level: the winning causal rule's value lands in the slot at commit.
The compiler chooses per fluent, invisibly (§6.1's cross-cutting rule):
concluded by a judgment, or small domain → logic-backed; otherwise
store-backed. The payoff case is §5.6's position fluent: logic-backed
`at(X) : cell` over a real battlefield would mint |cells| atoms per actor
plus exclusion — the blow-up the provider wall exists to prevent — while
store-backed position is one `uint32` per actor, equality guards only for
the rare named-cell rule ("anyone reaches the altar"), and the spatial index
reads the array directly as its broadphase input.

**Golden tests to pin it:** the dying trigger concludes `dead'` in the same
step as the damage, and the torch ramification cascades from it within that
step; the heal/curse oscillator is rejected at compile time naming the cycle;
a damage effect that crosses no threshold provably wakes no rules; two damage
sources on one tick sum; heal-plus-fire yields full-minus-delta through the
full pipeline (base, deltas, clamp exercised in one step); two unresolved
`:=`s reject naming both rules; `combine min` resolves two speed-setters to
the most restrictive; and the trace test asserts the itemized receipt.
*Status*: the guard-boundary half (threshold entailment chains; guard atoms
as closed-world strict inputs, never UNDECIDED) is pinned in
`tests/test_landmark.c`; the pipeline, dying-trigger, and oscillator tests
await the M1 value store and are listed in that file's header so the gap
stays visible.

### 5.9 Spawning: pools, scopes, counts, promotion

Vocabulary is closed — atoms interned at build, value-store arrays sized to
declared domains, rules ground per entity, replay logs referencing stable
ids — so nothing ever mints identity at runtime. The rule: **new instances
of prebaked kinds, arriving only at load boundaries — never new kinds, and
never mid-fixpoint.** Every engine lives this discipline somewhere (Quake's
fixed edict array, object pools, "no malloc in the frame loop"); here the
capacity is declared and checkable instead of a `#define` that fails by
corruption. Four mechanisms, chosen by what kind of thing spawns:

1. **Pools — same-scope spawning.** `entity goblin[8] : actor` (sugar for
   `goblin_1..goblin_8`) plus an `active(X)` fluent; spawning is an
   ordinary action (`causes active(goblin_3) & at(goblin_3) := gate &
   hp(goblin_3) := 7`), so inertia, saves, replay, and `why?` see a normal
   fluent flip. Inactive members cost memory and grounding size, not tick
   time: no facts, no joins (M3). Pool exhaustion is a detectable,
   traceable condition, and the declared bound feeds the §8 budget and the
   cardinality estimates. Recycling a slot is a spawn action with a
   complete effect list (compiler sugar: `reset(X)`); replay stays exact
   because the reset is in the log. Tooling should display generation
   counters so two "lives" of `goblin_3` read distinctly — semantics needs
   none.
2. **Scope instantiation — encounter-lifetime spawning** (§5.5). A summon
   loads an instance of a template scope; the vocabulary arrives whole,
   grounding happens at the load boundary (never mid-fixpoint), and
   despawn is arena teardown. Template identity is the §13 question.
3. **Counts — fungible items are not entities.** Identity is the expensive
   thing; grant it only when a rule must track *this one* rather than *how
   many*. Loot is `causes potions(X) += 1` on a numeric fluent (§5.8);
   only distinct items (the rusty key, a named artifact) need entity-hood,
   and those are authored, not spawned.
4. **Promotion — binding clutter to a slot.** §8's renderer-side crowds
   get logical existence by binding to a pool member the moment a rule
   first needs to reason about one, and demote back after.

Spawn *decisions* (encounter rolls, loot tables) are seeded-provider
answers feeding an action — never wall-clock or unseeded RNG (I4) — so the
roll is in the log and replay reproduces the ambush.

Honest fit: this suite covers the BG3-shaped target (authored worlds,
bounded encounters, story entities). Unbounded procedural spawning
(roguelike floors, survival hordes) would lean hard on pool recycling —
the same trade Osiris made, accepted for the same reason.

**Golden test to pin it:** spawn from a pool, act, despawn, respawn the
slot; replay from the log reproduces every state exactly, and the
exhausted-pool condition reports rather than corrupts. *Status*: the
lifecycle, recycle-reset, inactive-member inertia, and exact-replay tests
are pinned in `tests/test_spawn.c` (including the stale-flag-on-inactive-
slot behavior that motivates the reset rule); exhaustion reporting is
compiler surface and awaits M1.

### 5.10 Randomness: a seeded lookup, not a stream

Games need dice; a deterministic core (I4) cannot hold an RNG. The resolution:
**a random value is a provider (§5.2), not core state and not a host-injected
outcome.** The logic consumes a draw exactly as it consumes adjacency — a
seeded, side-effect-free answer computed engine-side — so the core neither
holds RNG state nor computes the draw, and I4's "no unseeded randomness in the
logic core" holds by construction.

**Lookup, not draw — the mental model.** Picture one immutable table per
`(seed, tick)`, its entries keyed by a stable *site* string, each holding a
number. `roll(site)` is a table lookup: the same site returns the same number,
and reading never consumes it. That property is what lets randomness sit
*inside* the fixpoint. A defeasible solve re-evaluates a guard many times as it
converges (§5.1); a stream would advance on each re-read and the solve would
not be well-defined, and two sites drawn in evaluation order would shear if
that order changed. A keyed lookup is **idempotent under re-read** and
**independent across sites** — order-blind in exactly the two ways the solver
needs. Conceptually the table is infinite and precomputed; in practice each
consulted entry is `hash(seed, tick, site)`, computed on demand, nothing
stored. The `tick` is a monotone step counter (deterministic, not wall-clock —
the same distinguished counter §5.3's durations want), so a site rolls afresh
each step; the `seed` is save-file state and selects the whole family of
per-tick tables at once.

**The site is ambient — keyed per ground rule-instance.** An author never
spells out which rule or which entities a roll belongs to. When a rule consults
`roll(...)`, the engine folds the *calling rule's identity and its current
binding tuple* into the site — the rule is the namespace (lexical scoping for
roll sites), so a tag need only be unique within a rule, and every ground
instance (`save(grik)` vs `save(gnok)`) gets its own independent roll for free.
The author supplies only the residual they control:

```
rule save(X):  ... roll()         // keyed (seed, tick, save#X)
rule dmg(A,D): ... roll("d6", i)  // 8d6: a tag + index disambiguate intra-rule draws
```

Two rules writing `roll("d6")` never collide — their rule identities differ.
Intentional *correlation* (one environmental d20 several rules must agree on)
is the explicit exception: `shared_roll("storm")` names a rule-independent
global site. Safe-because-independent is the zero-boilerplate default;
sharp-because-shared is visible. Die-shaping is ordinary numeric work: the
provider yields a number and `roll() + mod(A) >= dc(D)` is a value-store guard
(§5.8) — the raw draw is the only new primitive.

**Replay and lockstep.** A save is base facts + **seed** + action log. Because
*which* draws happen is decided by rule evaluation — part of the
deterministically-compiled theory (I4 extends to compile, §12) — re-solving the
log from the seed reproduces every roll exactly. Editing a rule reshapes its
instance keys and so shifts its rolls; within a version (all live play, all
lockstep peers) that never arises, and across versions it is the ordinary "a
rule edit changes behavior" concern — pin a roll with an explicit rule label (a
new reason for the optional labels of §6.2) or log outcomes rather than the
seed when a stored *old* log must replay stably. In lockstep, every peer's
table is fixed by the shared seed and identical compiled rules, so all peers
derive identical rolls with nothing to reconcile; the per-tick state hash (§12)
catches any peer whose build diverged.

**Golden test to pin it:** a rule consulting `roll()` yields the same value
across repeated solves of one tick (idempotence) and a different value the next
tick; two ground instances of one rule draw independently; `shared_roll` gives
two rules the same value; and a full action-log replay from a stored seed
reproduces every rolled outcome exactly (I4).

### Invariants (compiler/engine enforced)

- **I1 — No write-back.** Derived conclusions are never stored as base facts.
  Storing one recreates Osiris's stale-fact problem and breaks purity.
- **I2 — Actions are the only mutation.** Clients and all gameplay code
  change facts exclusively via the step function.
- **I3 — Providers are dependencies.** Index-backed guards invalidate their
  cones when their underlying index changes.
- **I4 — Determinism.** No wall-clock, and no unseeded randomness inside the
  logic core: randomness enters only as a seeded lookup provider (§5.10). A
  save is base facts + seed + action log; replay is exact.

## 6. Language sketch

See `examples/cellar.story` for the running example. One language: the
**core language** (declarations, rules, actions) compiles to engine
structures plus an **interface artifact** (§6.3), which is the contract
every client and any future front end checks against.

| Construct | Compiles to |
|---|---|
| `sort`, `entity`, `state` | fact-store schema (`sort` = entity type; `state` = fluent) |
| `module` / `extend` / `scene … in` | vocabulary ownership; generated imports (§6.4) |
| `rule … -> / => / unless`, `A > B`, bands | defeasible theory (strict/defeasible/defeater/superiority) |
| `action … requires … causes` | causal rules for the step function |
| bare `causes` rules | ramifications |

Authoring principles: authors never see primed atoms, inertia, time
indices, or scope imports; the surface keyword `state` declares a fluent
(the semantic term used throughout §5); `unless` sugars to a defeater;
conclusions are typed distinctly from fluents so I1 is a type error, not a
runtime surprise.

**Declarations batch; scopes brace.** Each declaration keyword (`sort`,
`entity`, `state`, `init`, `provider`) takes either a single item or a
Go-style parenthesized group of them (`state ( … )`) — pure lexical batching,
not a new construct. A group is a *list, not an archetype*: its members share
no vocabulary or key, so `()` never implies the grouped fluents belong to one
thing. The two brackets mean different things — `()` is a batch of
declarations, `{}` is reserved for a *scope body* (`scene`, `module`). `rule`
and `action` keep their per-item keyword: they carry bodies and superiority
relationships, and a batch would imply a togetherness that their conflict
edges (§6.2) cut across.

### 6.1 Authoring ergonomics (prioritized)

In a defeasible world, behavior is **emergent, non-local, and negative**: a
rule in one file is defeated by a superiority edge in another, and the author's
hardest question is not "why did this happen" but "why did this *not* happen."
That legibility — of non-local, negative, emergent behavior — is the ergonomic
north star, and it is exactly what a flag system cannot offer. Everything below
is ranked by how much it serves it. This is a language-design commitment, not
a wishlist: the Tier-1 items constrain the grammar and the M1 semantic passes,
so they are decided *before* the syntax freezes.

**Tier 1 — do-or-die.**

1. **`why not?` — negative traces.** `dl_why` must explain `−∂p`, not just
   `+∂p`: which supporting rule was inapplicable (which antecedent sat at
   `−∂`), and/or which attacker won and by which superiority edge. This is the
   question no ad-hoc system can answer; it falls straight out of the statuses
   the fixpoint already computes. It is the product's moat (§9), stated as a
   Tier-1 *requirement*, not a nicety.
2. **Declared vocabulary + orphan/typo detection.** Interning makes a misspelt
   atom (`hodling`) a fresh atom that is silently always false, so the rule
   never fires — the non-monotonic equivalent of a null deref, and Osiris's
   most common modding wound. Mitigation: a closed, declared atom set, plus a
   compile warning for any atom that appears only in rule bodies and is never a
   head or declared fluent ("`hodling` is never concluded — typo for
   `holding`?"). Cheap pass, outsized confidence return.
3. **Priority *bands*, not pairwise superiority.** Pairwise `>` is O(conflicts²)
   of hand-authored, forgettable edges — unmanageable at scale. The domain
   dictates the fix: 5e stacks in named tiers (base → condition → feat →
   immunity) and "specific beats general" is *layering*, not a thousand facts.
   Rules declare a band; higher bands beat lower by default; explicit `>` is
   reserved for intra-band exceptions. This is the largest
   authoring-throughput lever and it is domain-specific, so it had to be
   designed before the grammar freeze — done: §6.2, which also shows bands
   are load-bearing for *modularity*, not only throughput: a declared tier is
   the only defence against an attacker who does not exist yet (§6.4).
4. **Conflictable-pair detection at compile time.** Promote §9's item to Tier
   1: turn the *runtime* "conflict = authoring error" of §5.3 into a *build
   time* one — "rules A and B can both conclude `p` with no priority between
   them; here is a satisfiable state that triggers it" — with a suggested fix
   (assign a band, add `>`, or share a condition). Authoring errors caught at
   edit time, not playtest time.

**Tier 2 — managing the volume at scale.**

5. **Assertable why-traces as tests.** Extend the golden-test culture from
   pinning *verdicts* to pinning *reasons*: "`can_force_door(player)` is
   REFUTED **because** `too_weak` beats `can_force`." A refactor that reaches
   the right verdict by the wrong path then fails loudly. Same discipline that
   keeps the M3 algorithm swap honest, handed to content authors.
6. **Determinism-powered time travel.** I4 already guarantees exact replay from
   base facts + action log; that is a *debugger*, not just a save format. Scrub
   the action log, diff the fact store between any two ticks (§9), reproduce
   any reported bug from its save+log with zero flake. Ship the scrubber early
   — it makes every other bug cheap to corner.
7. **Navigation / LSP-shaped tooling.** At hundreds of rules: go-to-definition
   on an atom, "find all rules that conclude `p`," "find all attackers of `p`,"
   and the superiority / dependency-cone graph (§9). The cone is already
   computed for the scale spine (§5.4); surfacing it as "what could this rule
   affect or be affected by" is the direct antidote to fear of non-local
   breakage.
8. **Hot reload.** Sound *for free* by I1 (conclusions are derived, never
   stored): edit a rule, keep game state, see new verdicts. §9 lists it; the
   tight loop is worth pulling forward.

**Cross-cutting: authors never touch the machinery.** Primed atoms, inertia,
the doubled vocabulary, time indices — none appear in author surface (§6). This
is enforced, not merely intended: `unless` sugars to a defeater, I1 is a type
error (conclusions typed distinctly from fluents), a mis-scoped write is a
partition-checker error. The moment an author must think about `f'`, ergonomics
has failed.

**Milestone pull-in.** Items 1, 2, and 4 are M1 semantic-analysis passes and
land with the parser. Item 3 is settled (§6.2) and lands with the M1 grammar.
Items 5–8 track M2–M5 tooling but their data (traces, action log, dependency
cones) already exists in the scaffold.

### 6.2 Priority bands (decided)

The largest authoring-throughput lever (§6.1 item 3), now designed. Bands are
**pure compile-time sugar over pairwise `>`**: a ladder compiles to
superiority edges between rules that actually conflict (pairs the conflict
analysis already computes), and the engine, the ABGM semantics, `dl_why`, and
the M3 transformation pipeline never learn bands exist. Acyclicity is trivial
within a ladder (total order); ladders and pairwise `>` both feed the single
superiority relation, with one global acyclicity check over the union.

**Sugar, but not optional: bands are how open extension survives** (§6.4).
Defeaters are *literal*-addressed — a rule attacks a conclusion by naming an
atom, needing no handle on, or knowledge of, the rule it kills. Superiority
is *rule*-addressed. So attacking costs nothing and defending costs you the
attacker's name, and no author can name a mod that does not exist yet. Team
defeat sharpens it: an applicable attacker must be beaten by *every*
applicable rule on the other side, so an explicit edge against mod A does
nothing about mod B. **A declared tier is the only defence against an unknown
attacker, and the only thing that can arbitrate two mods that have never
heard of each other** — hence §6.4's rule that `extend` requires a band.
Grosof's courteous LP (§14) hit the same wall building for rule-base merging
and also concluded that priorities must be declared; DeLP's answer (compute
specificity, declare nothing) is the real alternative, and §14 says why it is
refused.

**Rule labels are optional, because superiority is the only rule-addressed
construct.** Everything else names *conclusions* — bodies and heads reference
literals, defeaters attack literals, queries and subscriptions (§11) watch
literals — all declared vocabulary, always named. Only `>` points at a rule,
so an author-given label is needed only where an author writes such a
reference: an explicit `A > B`, its cross-module `overriding` form (hence a
base rule mods may reorder must be a *named, exported* extension point, §6.4 —
extending §6.3's interface artifact, which today exports atoms/heads/actions
but not rule labels), or a why-trace assertion test (§6.1 item 5). Bands — the
primary priority mechanism — generate their edges by head-conflict analysis
over the compiler's internal rule identity, needing no labels, so most rules
are anonymous. Every rule still carries a stable identity (head plus source
span) that `dl_why` renders and tooling references; an explicit label is sugar
that makes it legible (`beaten by fey_ancestry`, not `combat5e.story:81`) and
hand-referenceable. The label is the name superiority needs, nothing more.

The precedent is exact. This is the meta-rule legal reasoning calls **lex
superior** — conflicts resolve by the authority ranking of the source — and
the one it calls **lex posterior** (the later rule wins) is what we refuse:
legislatures are totally ordered in time, so "later" is principled there,
while mod load order is arbitrary, so the same rule is a coin flip wearing a
principle's clothes. "Last one loaded wins" is the attractor every ad-hoc
system drifts into; naming it here is how we stay out.

**The invariant: superiority derives from *declarations*, never from
*bodies*.** Bands look like an exception to "priorities must be declared" —
the compiler emits edges nobody wrote — so the line has to be drawn
precisely, and this is where:

| edge | derived from | stable under body edits? |
|---|---|---|
| band ladder → pairwise `>` | rule's `@band` + the ladder | ✓ both declarations |
| import loses to local (§5.5) | rule's scope (file header) | ✓ declaration |
| causal beats inertia (§5.3) | rule kind | ✓ declaration |
| `A > B` | written by hand | ✓ it *is* the declaration |

Every generated edge is a function of something an author states *about* a
rule, never of what the rule *says*. Declarations are edited deliberately and
rarely; bodies are edited constantly, by people fixing unrelated things. A
band ladder emits edges from two visible declarations, and editing a body
never moves a rule between bands — which is exactly why bands are safe and
"the compiler works out the priorities" in general is not.

The rule this forbids is **lex specialis** — "the more specific rule wins",
inferred from bodies (A beats B when A's body is a strict superset of B's).
It is perennially tempting, and it fails twice over.

*It is not a decision procedure, even in law.* Rules overlap far more often
than they nest: `invisible(X)` and `faerie_fired(X)` (§6.2's own
`outlined > unseen`) are in no subset relation, and neither are most real
conflicts — partial overlap is the common case, and the subset case is the
rare clean one you would have got right anyway. Law has had two thousand
years on this and did not formalize it: lex specialis is not codified in the
Vienna Convention nor "elsewhere as a rule of general application", its
relationship to the other meta-rules "has not been clarified" (it can
contradict lex posterior outright — an older specific law against a newer
general one — with no meta-meta-rule to appeal to), and determining which
norm is more specific "depends on the context of the dispute and the
interpretive methodologies employed by courts": *what appears specific in one
scenario might be deemed general in another*. It is analyzed as a
**reason-giving norm** — an argument a judge weighs, itself defeasible,
whose primacy other reasons can reverse. So the formalizable version (the
syntactic subset test) is not lex specialis; it is a narrow accident that
happens to agree with it sometimes, and the actual principle is exactly the
part that would not compile.

*And the formalizable fragment breaks the invariant anyway,* with the failure
mode this engine exists to kill: given `crowbar ⊃ can_force`, editing
**`can_force`** to add a condition breaks the subset relation, the edge
**silently evaporates**, and `crowbar` starts contesting a rule it used to
beat. Nobody touched `crowbar`; nothing errors at the edit site; the rule
whose behaviour changed is not the rule that was edited. That is Osiris's
stale-fact bug relocated into the rule graph — a derived fact that was true
when written and is a lie now, with nothing to notice. Declared priorities
cannot rot this way.

There is a ladder of badness here, and it is worth stating because each rung
has advocates: **declared** (changes only when a declaration changes) →
**body-derived** (lex specialis: changes when any body is edited, at least at
compile time) → **derivation-derived** (DeLP's generalized specificity:
changes when bodies *or facts* change — its criterion is context-sensitive,
"determined dynamically during the dialectical analysis", so which of two
rules wins can differ **between two game states**, with no edit at all). The
last is unfixable by authoring discipline, and its honest trace reads "A beat
B *this time*" — strictly worse to hand a designer than "beaten by
`@immunity` over `@condition`". §14 records the DeLP fork in full.

```
bands stat_stack: base < condition < feat < immunity

rule dwarf_speed(X):  dwarf(X)                => speed(X) := 25   @base
rule restrained(X):   restrained(X)           => speed(X) := 0    @condition
rule freedom(X):      freedom_of_movement(X)  => ~(speed(X) := 0) @immunity
```

Decisions:

- **Named, not numbered.** Numbered priorities are the z-index disease:
  gap-numbering folklore, meaningless magnitudes. A declared ordered list of
  names forces each tier to mean something, and reading `@condition` on a
  rule states its defeat relationships against the whole family in one
  token — the legibility payoff scattered pairwise edges can never give.
- **Multiple ladders; comparability only within one.** Quest logic, combat
  stats, and social state need not share a ladder. A conflict between rules
  on different ladders, or banded vs. unbanded, is exactly as unresolved as
  before bands existed: explicit `>` or contested. (CSS arrived at the same
  fix in 2022: cascade layers are named, ordered, declared once, and
  specificity competes only within a layer.)
- **Unbanded rules are incomparable, never silently defaulted.** A default
  band would let a rule's defeat behavior change because a ladder was
  declared *elsewhere* — the non-local surprise §6.1 exists to prevent.
  Instead the conflictable-pair pass gains a nudge: "rule X conflicts with
  `@condition` rule Y but has no band; assign one or add `>`." Opt-in and
  backwards compatible with every hand-built world.
- **Explicit `>` is intra-band; contradicting a ladder is an error unless
  annotated.** A silent ladder-inverting edge makes band annotations lies;
  a hard ban forces band inflation or — worse — band *misassignment*, which
  corrupts what tiers mean invisibly. The genuine need is the pair-scoped
  exception-to-the-exception: Boots of Haste (`@base`) must beat Slow
  (`@condition`) while still losing to Restrained (`@condition`) — a shape
  band reassignment cannot express, since bands act uniformly against a
  whole tier. So the sharp tool announces itself (the §5.5/defeater house
  pattern):

  ```
  boots_of_haste > slow  overriding stat_stack
  ```

  Unannotated contradictions stay compile errors, so accidents — the common
  case — are still caught; the annotation distinguishes intent. For the
  annotated pair the ladder edge is suppressed and the pairwise edge
  emitted; the boots still lose to Restrained via the ordinary ladder edge.
  `dl_why` reports it distinctly: "beaten by `boots_of_haste` — explicit
  override of `stat_stack`."
- **The ladder name is mandatory: escape hatches must be claims, not
  permissions.** `overriding stat_stack` is a checkable proposition — the
  compiler errors if the edge doesn't actually contradict that ladder, so a
  refactor that re-bands the rules makes the stale annotation an error
  ("drop it") rather than letting it rot into blanket pre-authorization for
  contradictions the author never saw ("this edge now contradicts
  `speed_rules`, which your annotation doesn't cover"). A bare keyword would
  inherit `!important`'s decay pattern: consent that outlives its reason.
  What it does *not* inherit even bare is the arms race — edges name both
  endpoints and counter-overrides are superiority cycles, already rejected.
- **No override escalator.** There is no `!important` band. Beating
  `@immunity` means declaring a band above it, visible in the ladder for
  every future reader. Punctures that accumulate are a design smell surfaced
  by tooling, not semantics: the compiler warns past a threshold
  ("`stat_stack` is overridden 12 times; restructure the bands") — the
  cardinality-warning philosophy applied to superiority.

Prior art: CSS cascade layers (`@layer`) and `!important` as the
anti-pattern, with the diagnosis above; clingo's weak-constraint priority
levels (`[w@l]`); Grosof's courteous logic programs (prioritized conflict
handling for rules at business scale, though pairwise); and the legal
tradition, from which we take exactly one meta-rule. *Lex superior*
(constitution > statute > regulation) is a ladder, declared, and is what
bands are. *Lex posterior* and *lex specialis* are both refused above —
the first because mod load order is not a legislature's timeline, the
second because it is not a procedure even in law. That the legal system
needs three meta-rules, cannot rank them against each other, and litigates
the boundaries indefinitely is the argument *for* taking only the ladder:
one declared mechanism has no meta-rule conflicts to resolve.

**Semantic-pass tests to pin it (M1):** a ladder generates edges only
between conflicting pairs; the boots/Slow/Restrained triangle resolves as
above; an unannotated ladder-contradicting `>` is an error naming both
declarations; a stale `overriding` annotation is an error after re-banding;
the 5e stack resolves dwarf speed under Restrained to 0 and back to 25
under freedom of movement, with `dl_why` naming the band comparison at each
step.

### 6.3 Compilation model: erasure and provenance

The language is TypeScript-shaped: the C API (`world.h`) is the substrate —
complete and hand-authorable; the golden tests build worlds by hand, and
predate the language the way JS predates TS — and `.story` is optional
tooling above it whose chief product is the *checker*. The failure mode of
a fact database is silent (a misspelled atom is silently always false, like
`undefined` propagating), and the Tier-1 passes are static confidence over
that substrate: orphan detection is `noImplicitAny` for facts,
conclusions-typed-distinctly (I1 as a type error) is `readonly`,
conflictable-pair detection is exhaustiveness checking, cardinality
warnings are the lint.

Compiling the core language produces, beside the theory tables, an
**interface artifact**: the declared vocabulary — entities and their types,
fluents with domains, judgment heads, action signatures. It is the
compile-time twin of `world.h`'s runtime contract, and every client checks
against it.

- **Rules and declarations lower to data** — theory tables, schema, step
  tables. Transpilation: the runtime executing them is the fixed engine.
- **The artifact is the extension point.** Any future front end — a
  narrative language (§2), a quest editor, a third-party tool —
  *fact-checks* against it rather than reaching into the compiler: every
  guard atom resolves against the exported vocabulary (orphan errors
  included), every action reference checks arity and entity types against
  the exported signature. The guard-expression parser is a standalone
  library for the same reason, so guards mean the same thing wherever they
  are written.

**The interface artifact also compiles to a generated C header.** Host
code is a client too, and the intern table gives C the exact silent
failure mode the orphan pass exists to kill:
`world_query(w, lit(intern(syms, "can_atack_goblin")))` interns a fresh,
always-false atom. Codegen closes it, protobuf-style — typed atom/action
constants and arity-typed helpers (`q_can_parley(w, who)`,
`do_unlock(&acts, who)`) — so renaming a fluent in `.story` breaks the
host build instead of silently never firing. A combat loop (initiative,
targeting UI, NPC turns) is then ordinary host code driven by the outer
engine, with full vocabulary checking. **This header is the primary client
surface** — the way a game is expected to be built on the engine, and an
M1/M2 hard deliverable rather than a convenience. Any future front end (§2)
targets the same artifact and gets no privileges the header lacks.

**Erasure is a rule, not an accident:** no surface construct may require
runtime representation beyond engine structures. Bands erase to pairwise
edges (§6.2), thresholds to guard atoms and entailment rules (§5.8), types
and vocabulary closure to nothing; the M3 pipeline erases defeaters and
superiority within the logic itself. **Erasure is total** — no surface
construct has a runtime shadow. The expression VM (§5.8) is not an
exception: it evaluates numeric right-hand sides, which are engine
machinery, not a checked construct's residue. Any future front end (§2)
that wants a bytecode backend re-opens this rule
deliberately, and must argue for it.

**Provenance is the source map, and it is an M1 hard deliverable.** The
debugger is the product, and it will trace through machinery the author
never wrote: generated inertia, band-expanded edges, generated entailments,
strata. A trace that says "beaten by rule
`__gen_sup_417`" forfeits the moat. So every generated rule, atom, and edge
carries its source span and generation reason in the compiled module, and
`dl_why` renders in source terms: "beaten by `@immunity` over `@condition`
(ladder `stat_stack`, combat5e.story:18)"; "inertia on `door` (generated;
declared cellar.story:12)". §6.1's cross-cutting rule says authors never
*write* the machinery; provenance is how they never have to *read* it
either. (§5.4's declared scope interfaces are the `.d.ts` analog, consumed
by M4's module system.)

### 6.4 Two verbs: `extend` and `scene … in`

There are exactly two ways to build on an existing module, and they differ
in one thing — whether you **share its vocabulary**. Everything else about
them follows.

```
// world.story — the base game
module world
state door : { locked, closed, open }
state cursed(actor)
bands stat_stack: base < condition < feat < immunity
rule can_force(X): strength(X) >= 4 & door = closed  =>  can_force_door(X)  @base
```

```
// curse_mod.story — a MOD. Horizontal: joins world's vocabulary.
extend world
rule cursed_cant(X): cursed(X)  =>  ~world:can_force_door(X)   @condition
```

```
// cellar_fight.story — a SCENE. Vertical: own vocabulary, generated imports.
scene cellar_fight in world
rule cursed_cant(X): cursed(X)  =>  ~can_force_door(X)
```

The rule bodies are the same rule. The header picks what it means:

- **`extend M` — open extension, shared vocabulary.** The mod's
  `can_force_door` *is* world's atom. It attacks the base game directly,
  globally, permanently, and — this is the point — **without naming
  `can_force`**, which it neither owns nor can be sure exists. `@condition`
  beats `@base` by the ladder. This is the Emacs-advice posture (§3): any
  module may reach any conclusion, and the `why?` trace is what makes it
  survivable.
- **`scene S in M` — a nested scope (§5.5), private vocabulary.** The
  scene's `can_force_door` is `cellar_fight:can_force_door`, a distinct atom
  fed by a generated defeasible import. Concluding `~can_force_door` beats
  the *import*, not world's rule. World's answer is untouched; only this
  fight sees the curse.

**`extend` requires a band on any rule attacking a foreign atom;
unbanded is a compile error.** Not a style rule — it is exactly the case
where pairwise `>` cannot save you. Within your own module you may write
`A > B` freely: you own both rules. Across an `extend` you cannot name a
mod that does not exist yet, so a declared tier is the *only* defence
against a future attacker, and the ladder is the only thing that can
arbitrate two mods that have never heard of each other. Bands are therefore
load-bearing for modularity, not ergonomic sugar (§6.2). A scene needs no
band: it attacks nobody, it overrides its own import.

**Foreign writes are qualified at the site.** In an `extend`, naming
another module's atom in a head requires `world:can_force_door`, not the
bare name. The header alone is too thin a tell for a rule whose blast radius
is global — this is Python's `global`, Rust's `unsafe`, the discipline of
marking the non-local act where it happens rather than at the top of the
file. It also makes `grep 'world:can_force_door'` a complete census of what
touches that conclusion. Inside a `scene`, the bare name keeps meaning the
local import, and qualification is the escape hatch for asking what the
outer scope thinks:

```
rule surprised(X): world:can_force_door(X) & ~can_force_door(X)  =>  confused(X)
```

**Instantiation.** A `scene` declaration is a *template* (§5.9: spawning is
scope instantiation); each live instance is its own vocabulary —
`cellar_fight#1:wolf_hp` and `cellar_fight#2:wolf_hp` are different atoms
because they are different scope instances. Two wolves from one template get
distinct identity from the same mechanism that generates the imports, with
no separate id scheme (§13).

## 7. Runtime (C)

```
src/
  core/    arena allocator, string interning        (no deps)
  logic/   defeasible engine: theories, solve, why  (deps: core)
  state/   fact store, step function, inertia gen   (deps: core, logic)
  lang/    lexer, recursive-descent parser, compiler (later; deps: all above)
  app/     raylib shell, demo scenes                (the only raylib user)
tests/     golden semantic tests (ctest)
examples/  .story surface-language files
```

- **Memory**: bump arenas per theory/world; strings interned once to `uint32`
  atom ids; solve results are flat arrays indexed by literal id. No hidden
  allocation in the solve loop.
- **API shape**: build theory → `dl_solve` → query verdicts / `dl_why`.
  `world_*` wraps fluent declarations, closed-world fact assembly, judgment
  queries, and steps.
- **Serialization**: write base facts + scalar store; derived state recomputes
  on load — rule patches mid-campaign cannot corrupt saves.

## 8. Performance model

- Scene tier: thousands of ground rule instances, tens of thousands of facts →
  the linear pass is tens of microseconds; turn-based action boundaries give
  ~ms budgets. Full recompute per action, always. Homogeneous rule families
  at RTS-crowd scale may additionally evaluate columnar (§5.8's third
  backing): bitvector fixpoint rounds over entity-indexed arrays, 64
  instances per op.
- Global tier: hundreds of thousands of facts; event-driven wake-ups cost ∝
  changes intersected with the demand cone (§4.1), not database size —
  unwatched judgment cones are never maintained. Never maintain non-monotonic
  conclusions incrementally — scope the recompute instead. (The scene tier
  deliberately skips demand tracking: full recompute is already tens of
  microseconds; bookkeeping would cost more than it saves.)
- Crowds/presentation entities stay renderer-side; promotion into the fact
  store is an explicit design act. The precise boundary: **assets stay out,
  references may come in.** The store never holds pixels or geometry, but
  `sprite(grunk) = 17` is an ordinary integer fact whose interpretation
  lives renderer-side — the same indirection as `at(X) = cell` naming a
  cell whose geometry lives in the provider. Static asset bindings are
  edit-time init facts; *appearance as gameplay state* (disguise,
  polymorph, visible wounds) is a fluent changed only by actions, with the
  sprite index a derived judgment — so NPC rules and the renderer read the
  same fact, and a disguise that fools NPCs necessarily fools the player.
  No visual-desync bug class: there is no second copy of appearance to
  desync.
- Memory model of a shipped game: the world tier always resident; areas
  paged in and out at their interfaces (§5.5 — a scope at rest is its
  interface facts); encounters ephemeral in arenas; pools (§5.9) bounding
  concurrent identity within each. Pay-for-what-you-touch, provably
  invisible to the semantics.
- **Presentation reads the store and judgments; it never writes** (the only
  channel back into state is `do action`). The renderer is a query client
  like any other: host code asks judgments and fires actions; the renderer
  asks judgments and draws. Rendering runs per frame; solves run
  per step — judgments recompute on base-fact change (wake-ups), so frames
  read cached conclusions and pay nothing while nothing changes.

## 9. Tooling (first-class, built early)

- `why <literal>?` — proof/defeat trace: which rules supported, which
  attacked, which superiority decided it. Falls out of the semantics; it is
  the product's moat. A minimal version ships in the scaffold (`dl_why`).
- Compile-time: conflictable-pair detection ("rules A and B can conflict on p
  with no superiority"), safety violations, cardinality warnings, partition
  violations.
- These items are prioritized for authoring impact and mapped to milestones in
  §6.1 (`why not?` traces, orphan-atom detection, priority bands, and
  conflictable-pair detection are the Tier-1 set).
- **Editors emit the surface language.** A map/scope editor's save format
  is a `.story` module — entity declarations plus init facts (placements,
  terrain exceptions, asset bindings); no side-channel binary formats, so
  content is diffable, mergeable, and reviewable. An editor linking
  `infeasible_core` runs the *real* solver against the map as it is edited:
  place a goblin, watch `alerted` derive, ask `why?` — one semantics, no
  second implementation approximating game behavior. Map edits change t=0
  base facts and so invalidate old action logs; saves carry a content hash
  and say so loudly rather than replaying divergence.
- Later: rule hot-reload (sound because conclusions are derived), fact-store
  diff viewer between steps, dependency-graph visualization.

## 10. Parser plan

**Hand-rolled recursive descent** (decided). Rationale: full control over
error messages and recovery (author-facing tool, so "expected `=>` after rule
body, found `->`" quality matters), no generator dependency. Structure:
hand-written lexer → recursive descent with Pratt expression parsing for
guards/arithmetic (the guard-expression parser is a standalone library, so
a future front end can reuse it — §2) → AST in arenas → semantic passes
(types, safety, stratification, conflict pairs, partitions) →
ground/compile to engine structures + interface artifact. Panic-mode
recovery at declaration boundaries so one error doesn't cascade.

## 11. Milestones

1. **M0 — this scaffold**: core (arena/intern), defeasible engine with
   query + why, step function with inertia/ramifications/conflict detection,
   golden tests (Yale shooting, cellar, torch ramification, conflict), raylib
   app shell with an interactive cellar demo.
2. **M1 — language front half**: lexer + recursive-descent parser for
   declarations/rules/actions; semantic checks; fluent syntax implements
   §5.7–5.8 (domains, threshold harvesting, effect operators, guard
   stratification); `cellar.story` compiles and replaces the hand-built test
   worlds; provenance carried on every generated construct, rendered by
   `dl_why` in source terms (§6.3); interface artifact and generated C
   header emitted (§6.3).
3. **M2 — client contract + host API**: define the public client contract —
   the `world_*` API plus the externalized-state pattern (§4.2) — and make
   the generated C header (§6.3) the way games are actually written against
   it: typed atom/action constants, arity-typed query and action helpers,
   a rename in `.story` breaking the host build. **Queries carry a scope**
   (§5.5, §6.4): with private per-scope vocabulary, bare
   `world_query(w, can_force_door)` is ill-formed — there are two atoms.
   Either a scope parameter or a scoped view handle (`world_view_in(w, enc)`)
   lands here, and the generated helpers grow with it. Prefer the view: most
   clients know their scope once, not per call. The client's reactive channel
   is a **unified subscription**, `world_subscribe(view, literal)`: register
   interest in a literal and receive it in each step's delta when its value
   flips. State facts are the free leaf case — the step already computes its
   changeset, and the raw fact-store diff (§9) is subscribe-to-all-base-facts
   — while derived judgments are the cone-recompute case (§4.1 wake-ups); the
   call is identical either way, and a subscribe to a large-cone judgment is a
   cardinality-style warning, not a different primitive. Subscription names
   *conclusions*, so a fluent refactored into a judgment (or back) never
   touches a client call site. Deltas for numeric fluents carry the §5.8
   commit receipt as structured data — each changed value's undefeated
   contributions with ground-rule provenance — so attributed combat text is
   a projection of the delta, never a parsed `why?` string. Playable cellar in raylib
   driven entirely by host code against the generated header. A trivial
   second client in tests pins the no-private-APIs claim (§4.2) the way
   golden tests pin semantics — with no reference client, this test is the
   *only* thing keeping the client boundary honest, so it is a hard
   deliverable, not a nice-to-have.
4. **M3 — engine hardening**: Maher linear algorithm + transformation
   pipeline behind the same API; tick-time join matcher for variables/typed
   entities (until then: ground rules per entity by hand/codegen).
5. **M4 — scale spine**: global tier (subscriptions, dependency cones —
   wake-ups recompute only the reachable ∩ demanded set, per §4.1; the demand
   cone is the attack-closed backward reachability from subscriptions and
   step-rule bodies over the same static graph), scene partitions, **nested
   scopes with lifetime/visibility (§5.5)**, serialization, hot reload.
6. **M5 — proof-of-thesis demo**: one region, ~20 NPCs, a 5e-ish combat slice
   where conditions/feats interact through superiority, one multi-step quest,
   `why?` in the UI — all driven by host code against the generated header
   (§6.3). The quest is the interesting half: a multi-step quest with no
   narrative layer is the honest test of whether rules alone carry story
   state.

## 12. Distribution: web target and content artifacts

The shipped product is a browser platform for a remix community: many authors
making scenarios, subclasses, spells, items, and total conversions on a shared
5e chassis, with nothing to install. This is a distribution and packaging
concern layered on the kernel (§4.2); it adds no engine semantics.

**The web target.** The logic core compiles to WASM as a library. Presentation
is a swappable client (§4.2): native raylib for development, web-native
(Canvas2D or Pixi + DOM) for the shipped product. The core re-solves per
action, not per frame, so the JS↔WASM boundary is crossed rarely and carries a
subscription delta (§11 M2), not per-frame traffic — the inspector's reactive
channel and the WASM marshalling seam are the same `world_subscribe` payload.

**Compiler as a library over an IR.** Parsing and semantic analysis are
separate stages: the parser emits a declaration IR; the analysis and grounding
passes (§5.2, §5.8) consume that IR and never depend on the tokenizer. The
analyzer therefore runs at native build time *or* in-browser at load — the
property that keeps in-browser authoring (no build step) possible.
**Compilation is deterministic** — I4 extends to the compile step — so two
peers grounding the same source obtain the identical theory. That is a
lockstep-multiplayer correctness requirement, not merely cache hygiene.

**Source is authoritative; the compiled theory is a cache.** Every artifact
ships `.story` source, always, so anyone can inspect and adapt it — the remix
community depends on it. The grounded/compiled theory is a regenerable *local*
cache keyed by `hash(source + engine version)` — the `.el`/`.elc` model: a miss
recompiles (slower), never breaks, and content-hash keying is never stale by
construction. Compiled-only artifacts are never distributed.

**Artifacts: reference the shared substrate, embed only at a self-sufficient
leaf.** Three layers:

- **Engine** (WASM): shared, hash-identified, optionally signed for provenance.
- **Game / content pack**: `.story` source + assets + a manifest (id, author,
  version, `requires`), shipped as one container and referencing the engine by
  hash. Mods and total conversions are one artifact at different sizes (§6.4):
  packs `extend` or `scene … in`-import a *layered* 5e (a content-blind core
  plus separate spell/item/subclass files) and compose à la carte, with
  `requires` naming an *interface* (the exported heads) rather than a specific
  file, so alternative implementations substitute. Load order is irrelevant (a
  theory is a set; a genuine clash is a conflictable-pair error, never silent
  last-writer-wins).
- **Save**: `(engine-hash, game-hash, action-log)`, embedding nothing. The save
  is an action log, not a state snapshot (I4) — which yields shareable
  playthroughs, branching, and time travel for free. A base-fact snapshot
  (never judgments — I1) is an optional load-time cache:
  nearest-checkpoint-plus-replay-the-tail.

**Save compatibility: loading old saves is schema migration, and most
patches need none.** A production game patches content under players' feet;
the save's exposure to that splits three ways, in increasing difficulty:

1. *Rule changes are free — and that is most patches.* Nothing derived is
   stored (I1), so a save loads under changed judgments, superiority, bands,
   or a rewritten damage pipeline and the conclusions simply recompute — the
   same property that makes hot reload sound (§9), applied across versions.
   The conventional killer — cached derived state going stale against new
   code — is a bug class that cannot exist here.
2. *Schema changes are checked, declarative migrations.* What can break is
   the EDB schema: fluents added/removed/renamed, pools resized, domains
   changed, scopes restructured. But the save is a relational database with
   a declared, machine-readable schema — the interface artifact (§6.3) — so
   the compiler diffs old against new and classifies every change.
   **Additive is automatic**: a fluent absent from an old save takes its
   declared default/init — closed-world "absent means default" (§5.8)
   wearing migration clothes — and grown pool slots arrive inactive.
   Everything else must be covered by a versioned `migrate` block in
   `.story`: renames, value maps for domain changes, expressions computing
   new fluents from old state (`grudge(X) := old.hostility(X) > 5`) — run
   once at load, a pure data transformation over the old EDB, with
   provenance on every mapped fact. **Exhaustiveness is enforced**: an
   unmapped removal, rename, or domain change is a compile error naming the
   fluent — the partition-violation posture — so silent data loss is
   impossible, which is the difference from every ad-hoc save converter
   ever shipped. Because old content is regenerable from authoritative
   source (above), a migration expression may reference old *judgments*,
   not only old facts: instantiate the old theory once at load and ask it —
   the case where a v1 derived judgment becomes a v2 base fluent. Chains
   compose (v1→v2→v3) and the compiler verifies the composition, not just
   each hop.
3. *Replay never crosses versions — by decision, not limitation.* Replaying
   a v1 action log under v2 rules produces a different story; the player
   chose against v1's judgments (event-sourcing's log upcasters are the
   cautionary tale). Logs are segments tagged `(engine-hash, game-hash)`
   (§9's content hash, made structural); migration operates on the
   base-fact snapshot the save model already keeps as its checkpoint,
   records itself as a lineage event, and opens a fresh segment. I4's
   replay, time travel, and fact-diff hold *within* a segment, and `why?`
   spans the boundary through the migration provenance.

Prior art: Datomic's grow-only schema (why additive-is-free is the design
center), event-sourcing upcasters (what item 3 refuses), Paradox's
cross-version save converters (the ad-hoc practice the exhaustiveness check
replaces). Open riders in §13: pool shrink policy; fluents moving between
scopes.

**Late join and state sync.** Determinism makes multiplayer *lockstep*: peers
broadcast actions (§4.2 driver), everyone replays, and consistency is
structural — no authority or consensus, since every peer holds the bit-identical
state, and any peer can serve a joiner. A joining client reaches the live state
either by replaying the action log from genesis (simplest; sufficient while
sessions are short) or, as a long-session optimization, by importing a
tick-stamped base-fact snapshot (never judgments — I1; re-derived via `dl_solve`)
plus the action tail after it. The live-join dance — subscribe-and-buffer the
action stream *before* requesting the snapshot, discard buffered actions at or
before the snapshot's tick, apply the snapshot, drain the rest through the normal
`world_step` path (I2), then go live — is transport orchestration and lives in
the outer engine, not the core. The core exposes only primitives: export/restore
a base-fact snapshot at a tick, deterministic step-apply, and an optional
per-tick state-hash for desync detection (RTS "sync checks"). The join-snapshot
is the same base-fact checkpoint the save model already needs; it is not a
separate netcode state.

**Server-authoritative deployment: the other trust topology, same kernel.**
Lockstep (above) and server-authoritative are not two engines but two *trust
deployments* of one kernel. Lockstep is free and serverless, and right when
peers are trusted (co-op, hotseat, shareable playthroughs) — but every peer
holds bit-identical full state, so hidden information is structurally
unprotectable (the RTS maphack: the whole map is in the cheater's RAM) and
every §5.10 roll is predictable from the shared seed. When information must
be hidden or peers cannot be trusted, run the driver and kernel server-side
and make each client a §4.2 presentation client whose state *is* its
subscribed cone:

- **A thin client is a subscriber over the wire.** "Which facts does this
  client hold" is a subscription set; "which conclusions can it see" is a
  demand cone (§4.1). The client receives its `world_subscribe` delta
  stream and proposes actions through the do-port — the two-port kernel API
  is already a client-server protocol shape, and the network seam is the
  same payload as the WASM marshalling seam and the inspector channel
  (above): one seam, three consumers. MMO interest management /
  area-of-interest is, in this vocabulary, a demand cone whose visibility
  predicate is fed by a spatial provider.
- **Visibility is judgments, not a replication config.** `visible_to(P, …)`
  is an ordinary derived judgment — fog of war, invisibility, darkness,
  disguise as defeasible rules — and the server filters each client's delta
  stream through it. Payoffs no replication layer gets: `why?` answers "why
  can't I see the goblin" with a proof trace, and cheating is structurally
  impossible rather than policed — the hidden fact never crosses the wire,
  instead of arriving masked. This is §8's disguise principle (one fact,
  every reader) extended to *which peer* is reading.
- **Prediction soundness is statically checkable.** A client may run a
  partial replica over its visible facts for responsiveness, but a
  conclusion over a partial EDB is trustworthy only when the rule's whole
  cone lies inside the visible set — a static property of the dependency
  graph against the visibility interface. The compiler classifies every
  judgment client-predictable (cone ⊆ subscribed interface) or server-only;
  the generated header can type them differently. "What feels instant vs.
  what waits for the server" becomes a compile-time report, not a QA
  discovery. The §5.3 dry-run query is the evaluation primitive prediction
  needs.
- **The seed is server-only.** Under hidden information, §5.10's seed must
  not replicate — clients receive outcomes as facts, or every roll is
  predictable (the roll-hack is the maphack's sibling). The one place
  "server-only" is a hard requirement rather than a subscription choice.
- **Sectors compose** (§5.5 stress test): the server steps sibling sectors
  concurrently; a client's subscription follows its avatar's sector;
  handoff is a resubscribe.

**Trust is containment, not credentials.** Untrusted content runs in a
WASM + sandboxed-iframe cage with a minimal import surface; that is the whole
security posture. The real safety property is that content is declarative data
run through a vetted interpreter, not arbitrary code. Engine signing is
optional provenance/UX (a verified-creator badge, unknown signers warned by
fingerprint), never the containment mechanism, and never gates play.

**Inspector (§9).** A client above `world_*` that fuses fact-store truth with
the host's presentation and spatial state (§5.6) through a binding table
`{logic id ↔ host id}` the host owns; the engine exposes the hooks, the outer
engine builds the GUI. The hooks: a structured `dl_explain` (the `why?`
proof/defeat DAG as data — the text trace is one renderer over it), an
entity→literals reverse index (which requires grounding to retain provenance —
an M1 constraint), and `world_subscribe` deltas (§11 M2). Point-and-click
`why?` over an entity's propositions, and explaining a *transition* (a step's
primed-atom trace) rather than only static state, are the target.

## 13. Open questions

- **Effect-operator set and domain-declaration surface** (§5.8): the numeric
  semantics are fixed, but the exact operator set (`:=`, `+=`, `-=`, …) and
  the domain-declaration syntax are M1 decisions, not yet frozen.
- **Set-quantified effect binder** (`examples/srd_probe*.story`): the one
  effect-side construct the M1 parser must not front-run. An effect that ranges
  over a provider-answered set — `for each T where <guard> [limit n]: <effect>
  [when <cond>]` — is what AoE (Fireball), set-retract (concentration ending),
  variable-count spawning (summon N), and zone painting all reduce to, and it
  is the M3 grounding risk in surface form. Its riders (per-target `when`,
  transient action-scoped inputs like a save outcome, relational effect
  provenance) are decided with it. Declarations, judgment rules, bands, and
  fixed-arity actions can be parsed *before* this is frozen; the effect grammar
  cannot.
- **First-class value domains beyond entity sorts**: `cell`/`point` for
  targeted and area spells (§5.6), and `enum`-style value domains distinct from
  `sort` (which is for entities). Orthogonal to and smaller than the binder,
  but on the same M1 effect-surface critical path.
- **Concurrent non-numeric action interactions**: the numeric pipeline (§5.8)
  and multi-valued defeat (§5.7) settle the mechanics; the author-facing
  rules of thumb for actions that interact through neither still need docs.
- **Ambiguity propagation**: blocked for now, for predictability. Revisit if
  authors want "conflicting rumours" semantics where an undecided premise
  should taint downstream conclusions.
- **Team defeat**: on, matching "several weak reasons jointly outweighed."
  Needs author-facing docs either way.
- **Cross-scope entity identity** (§5.5, §6.4): scope-qualified atoms give a
  spawned instance identity *within* its scope, but escalation fires an
  action onto an outer fluent (§5.5 rule 5), and if that fluent must name a
  specific instance — `dead(wolf)` at world scope, not just a
  `wolves_killed` counter — the identity has to survive a boundary that
  scope-qualification does not obviously carry across. Decide with M4's
  module system. The sector handoff (§5.5's stress test) is the same
  question at MMO scale and suggests the shape: durable id owned by the
  outer tier, sector-local state keyed by pool slot.
- **Escalation merge order under concurrent sibling steps** (§5.5): sibling
  scopes may step in parallel, but I4 needs escalations arriving at the
  shared outer tier to merge in a canonical order (sector id then log
  position — never arrival time). Small, load-bearing; decide with M4.
- **Migration riders** (§12): pool *shrink* needs a policy (reject the
  migration vs. an authored cull predicate choosing survivors), and a
  fluent moving between scopes/tiers is the migration face of the
  cross-scope-identity question above — decide them together.
- **Server-authoritative riders** (§12): reconciliation semantics when a
  client-predicted judgment is contradicted by the authoritative delta
  (presentation-side rollback — but the boundary needs stating);
  whether `visible_to` filtering is per-subscription or per-scope
  (per-scope is coarser but composes with §5.5 for free); and how the
  server-only seed interacts with replay of a client's local log.

## 14. References

- M. Maher, *Propositional Defeasible Logic has Linear Complexity*, TPLP 1(6), 2001.
- G. Antoniou, D. Billington, G. Governatori, M. Maher, *Representation
  Results for Defeasible Logic*, ACM TOCL 2(2), 2001.
- M. Maher, A. Rock, G. Antoniou, D. Billington, T. Miller, *Efficient
  Defeasible Reasoning Systems*, IJAIT 10(4), 2001. (Delores.)
- H.-P. Lam, G. Governatori, *The Making of SPINdle*, RuleML 2009, LNCS 5858,
  pp. 315–322. (the closest implementation to M3's target: the TOCL
  transformations as a theory normalizer in front of a linear propositional
  engine, scaling past a million rules. Propositional by construction — its
  algorithms assume the Herbrand base of the input theory is already built)
- M. Rohaninezhad, S. Mohd Arif, S. A. Mohd Noah, *A grounder for SPINdle
  defeasible logic reasoner*, Expert Systems with Applications 42(20), 2015.
  (grounding retrofitted onto SPINdle for stratified theories using dlv and
  gringo techniques. States §5.2's premise as a complexity result —
  inference is linear, but bottom-up instantiation of the ground predicate
  set is NP-complete — and identifies the fork: backward-chaining defeasible
  reasoners support the first-order form, forward-chaining ones take the
  propositional form. §5.1's choice of forward chaining therefore fixes the
  propositional core and makes §5.2's grounding discipline load-bearing
  rather than hygienic. Its own answer is Herbrand instantiation up front,
  which §5.2 item 4 refuses: a tick loop joins against live facts instead,
  paying per match rather than per possible ground instance)
- C. Martens, *Ceptre: A Language for Modeling Generative Interactive
  Systems*, AIIDE 2015.
- M. Gelfond, V. Lifschitz, *Action Languages*, ETAI 1998.
- E. Giunchiglia, J. Lee, V. Lifschitz, N. McCain, H. Turner, *Nonmonotonic
  Causal Theories* (𝒞+), AIJ 2004.
- M. Shanahan, *Solving the Frame Problem*, MIT Press 1997.
- E. Mueller, *Commonsense Reasoning*, 2nd ed., 2014.
- J. Lee, V. Lifschitz, *Describing Additive Fluents in Action Language C+*,
  IJCAI 2003. (concurrent numeric effects that combine)
- M. Bartholomew, J. Lee, *Stable Models of Formulas with Intensional
  Functions*, KR 2012. (functional stable models / ASPMT)
- J. Lee, Y. Meng, *Answer Set Programming Modulo Theories and Reasoning
  about Continuous Changes*, IJCAI 2013. (𝒞+ over continuous domains via SMT)
- M. Fox, D. Long, *PDDL2.1: An Extension to PDDL for Expressing Temporal
  Planning Domains*, JAIR 20, 2003. (numeric effects as a closed operator set)
- S. Graf, H. Saïdi, *Construction of Abstract State Graphs with PVS*,
  CAV 1997, LNCS 1254. (predicate abstraction — §5.8's landmark guards)
- K. Forbus, *Qualitative Process Theory*, AIJ 24, 1984. (quantity spaces:
  numeric state as ordered landmark intervals)
- R. Evans, E. Short, *Versu — A Simulationist Storytelling System*, IEEE
  TCIAIG 6(2), 2014. (exclusion logic: multi-valued state as the core
  representation of a shipped narrative engine)
- A. Bikakis, G. Antoniou, *Contextual Defeasible Logic and Its Application
  to Ambient Intelligence*, IEEE Trans. SMC-A 41(4), 2011; and *Local and
  Distributed Defeasible Reasoning in Multi-Context Systems*, RuleML 2008.
  (contexts with private vocabulary, defeasible mappings between them, a
  preference ordering over contexts — §5.5's nested scopes are this
  construction; the mapping-is-defeasible rule is what lets a scene override
  locally without contesting the world)
- A. Lindroos, *Addressing Norm Conflicts in a Fragmented Legal System: The
  Doctrine of Lex Specialis*, Nordic J. Int'l Law 74, 2005; M. Koskenniemi,
  *Fragmentation of International Law* (ILC study, 2006); and the
  reason-giving-norm reading (*Lex Specialis as a Reason-Giving Norm*, Int'l
  Community Law Review 27(3), 2025). (§6.2 takes *lex superior* — a declared
  ladder — and refuses the other two. The cautionary value is in how badly
  lex specialis resists formalization in the domain that invented it: not
  codified in the Vienna Convention nor as a rule of general application, its
  relation to the other meta-rules unclarified, capable of contradicting lex
  posterior with no tiebreak above them, and specificity itself
  context-dependent — "what appears specific in one scenario might be deemed
  general in another". It is an argument judges weigh, defeasible in its own
  right, not a function)
- B. Grosof, *Prioritized Conflict Handling for Logic Programs*, ILPS 1997;
  *Representing E-Commerce Rules via Situated Courteous Logic Programs*,
  ECRA 2003. (courteous LP: `overrides(r1, r2)` over rule labels, plus mutex
  declarations, built explicitly for *merging* rule bases from different
  authors — the closest prior art to §6.4's `extend`, and it too resolves
  conflicts by declared priority rather than by any implicit criterion)
- A. García, G. Simari, *Defeasible Logic Programming: An Argumentative
  Approach*, TPLP 4(2), 2004; F. Stolzenburg, A. García, C. Chesñevar,
  G. Simari, *Computing Generalized Specificity*, J. Applied Non-Classical
  Logics 13(1), 2003. (DeLP: no superiority relation at all — argument
  comparison by *generalized specificity*, computed rather than declared.
  It removes §6.4's naming asymmetry entirely — nothing to name, so a core
  rule defends itself against mods that do not exist yet — but is refused on
  three counts. Dialectical trees are not linear, and §5.2's engine choice
  rests on
  Maher-linearity. A computed criterion gives a `why?` that derives rather
  than explains — "A is more specific" is a worse answer to a designer than
  "beaten by `@immunity` over `@condition`". And decisively for §6.2's
  invariant: the criterion is context-sensitive — preference is "determined
  dynamically during the dialectical analysis" — so priority is a function
  of the *fact base*, and which of two rules wins can differ between two game
  states with no edit at all. That is the bottom rung of §6.2's ladder:
  unfixable by authoring discipline, because there is no authoring act that
  pins it)
- E. Oikarinen, T. Janhunen, *Modular Equivalence for Normal Logic Programs*
  / module theorem; V. Lifschitz, H. Turner, *Splitting a Logic Program*,
  ICLP 1994. (compositionality of answer sets under **disjoint output
  signatures** — the formal version of §5.4's invisibility claim, and the
  proof that open extension and a composability theorem are exclusive: §6.4's
  `extend` is precisely two modules defining one atom, which these forbid)
- M. Tofte, J.-P. Talpin, *Region-Based Memory Management*, Information
  and Computation 132(2), 1997. (arena-per-scope + dependency-closure
  checking is region typing for facts, §5.5)
- B. Grosof, *Prioritized Conflict Handling for Logic Programs*, ILPS 1997.
  (courteous logic programs — rule priorities at business-rules scale)
- W3C, *CSS Cascading and Inheritance Level 5*. (cascade layers: named
  ordered tiers over an unmanageable pairwise system; `!important` is the
  escape-hatch anti-pattern §6.2 refuses)
- M. Gebser, R. Kaminski, B. Kaufmann, T. Schaub, *Answer Set Solving in
  Practice*, Morgan & Claypool 2012. (weak-constraint priority levels)
- Wizards of the Coast, *Magic: The Gathering Comprehensive Rules*, §613
  ("Interaction of Continuous Effects"). (fixed set-before-add pipeline for
  simultaneous effects; its timestamp system is the cautionary half)
- Larian's Osiris: DOS2/BG3 modding documentation (community wiki).
- E. Ruskin, *AI-driven Dynamic Dialog through Fuzzy Pattern Matching*,
  GDC 2012. (Left 4 Dead response rules: most-specific-match dialogue
  selection — "specific beats general" as ad-hoc scoring; §4.2's
  `pending_scene` gets the same behavior from superiority, with traces)
- inkle, *ink* — https://github.com/inkle/ink (surface model for a narrative
  layer should one be built as a client (§2); its growth of variables,
  functions, and arithmetic is the cautionary half — computation belongs in
  rules or providers, not a dialogue layer)
