# Infeasible — Engine Design

*A narrative game engine in C where the world is a logic database: defeasible
rules for judgments, defeasible inertia for change, and host code driving it
through a generated, vocabulary-checked API. raylib for presentation, CMake
for builds.*

*A narrative/dialogue layer is deliberately **out of scope** — see §12.1 for
the seam it reattaches at. Everything below is the rules engine.*

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
  declarations + rules, and actions. The language is complete without a
  narrative layer; §12.1 records the seam one would attach at.
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
  prose. A 5e-ish combat slice is a milestone acceptance test.

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
  2. *Typed variables* (`X : actor`) bound every domain.
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

1. **Read down the stack freely.** An inner rule may read facts and
   conclusions from any enclosing scope. Encounter rules see area and world
   facts; the reverse is forbidden.
2. **Outer facts are pinned during an inner pass.** While an encounter's
   defeasible pass runs, every fact it reads from an enclosing scope is an
   immutable input — nothing outer changes mid-fixpoint. The inner cone reaches
   outward but only across a frozen boundary, so the scoped recompute is sound
   for exactly the §5.4 reason: dependency is monotonic, and a pinned input
   cannot be part of the reachable-change cone of the inner step.
3. **Write only your own scope.** A step commits `+∂`-primed literals only for
   fluents declared in the scope it runs in.
4. **Escalate outward only through a declared action.** The single way an
   inner scope affects an outer one is by firing an action whose effect lands
   on an outer fluent — a declared interface, which is what §5.4 already calls
   the module system. This keeps I1/I2 intact: cross-scope influence still
   travels as base facts through the step function, never as a stored
   conclusion and never as a silent write.
5. **Outer changes wake inner subscribers.** The global-tier subscription
   mechanism of §4.1 points *downward*: a world-fact change wakes the inner
   scopes whose static cones include it, and they recompute defeasibly.

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
open, §12.

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

**Scope-depth superiority (opt-in).** When an inner rule and an outer rule
conflict on the same head, the natural default is that the *more local* rule
wins: `encounter > area > world`. This is the logical form of D&D's "specific
beats general" (§3) — the same thing `too_weak > can_force` writes by hand in
`cellar.story`, but induced by structure. It is powerful and dangerous:
ambient scope-superiority can silently defeat an outer rule an author forgot
was in scope, which is the opposite of the `why?` transparency that is the
product's point. Therefore it is **opt-in per rule, never ambient**, and
`dl_why` always names the deciding scope explicitly ("beaten by encounter-scope
rule R"). Tracked as an open question (§12).

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
visible stack (points 1–2) and scope-depth superiority is expressible;
physically (A)'s arena-per-scope, so an encounter tears down in one free and
solve stays allocation-free (§7). The scope id is a small tag on the atom;
grounding and wake-up respect it.

**Golden test to pin the meaning** (mirroring how Yale-shooting pins inertia):
an encounter fluent set true does **not** survive the encounter's teardown,
while a world fluent set true from inside the encounter (via a declared
escalating action) **does** persist after teardown — and an inner rule reading
an outer fact produces the same verdict whether evaluated scoped or whole.

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
  (`fluent hp : int in 0..20` — the schema is the outermost clamp, so
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
   resolver for same-stage `:=` collisions — `fluent speed : int combine min`
   gives 5e's "two effects set your speed: the most restrictive applies".
   Static, declared in one place, admissibility-checked, named in the trace.
4. *Bespoke pipelines are modeled, not configured*: a real damage pipeline
   (base → resistances → vulnerability → clamp) is written as derived
   judgment values (`incoming_damage(X)` through the ordinary defeasible
   layer, bands arbitrating modifiers) committed by a *single* effect — the
   MTG move of writing CR 613 as rules. Costs no engine feature; `why?`
   traces every stage because every stage is a rule.

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
**default** (`fluent terrain(cell) : tile default floor`), backing it with
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
   despawn is arena teardown. Template identity is the §12 question.
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

### Invariants (compiler/engine enforced)

- **I1 — No write-back.** Derived conclusions are never stored as base facts.
  Storing one recreates Osiris's stale-fact problem and breaks purity.
- **I2 — Actions are the only mutation.** Clients and all gameplay code
  change facts exclusively via the step function.
- **I3 — Providers are dependencies.** Index-backed guards invalidate their
  cones when their underlying index changes.
- **I4 — Determinism.** No wall-clock, no unseeded randomness inside the
  logic core; a save is base facts + action log; replay is exact.

## 6. Language sketch

See `examples/cellar.story` for the running example. One language: the
**core language** (declarations, rules, actions) compiles to engine
structures plus an **interface artifact** (§6.3), which is the contract
every client and any future front end checks against.

| Construct | Compiles to |
|---|---|
| `entity`, `fluent`, `scene` | fact-store schema; partition declarations |
| `rule … -> / => / unless`, `A > B`, bands | defeasible theory (strict/defeasible/defeater/superiority) |
| `action … requires … causes` | causal rules for the step function |
| bare `causes` rules | ramifications |

Authoring principles: authors never see primed atoms, inertia, or time
indices; `unless` sugars to a defeater; conclusions are typed distinctly
from fluents so I1 is a type error, not a runtime surprise.

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
   reserved for intra-band exceptions. Scope-depth superiority (§5.5) is one
   instance of the same principle. This is the largest authoring-throughput
   lever and it is domain-specific, so it had to be designed before the
   grammar freeze — done: §6.2.
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
within a ladder (total order); ladders, scope edges, and pairwise `>` all
feed the single superiority relation, with one global acyclicity check over
the union.

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
- **Scope-depth superiority (§5.5) slots in below bands**, as a tiebreak
  between conflicting rules that bands did not decide (same band or both
  unbanded), for rules that opted in. Explicit band assignment is louder
  intent than lexical position. The opt-in form stays open until M4 (§12);
  its precedence slot is reserved now, which is all the grammar needs.

Prior art: CSS cascade layers (`@layer`) and `!important` as the
anti-pattern, with the diagnosis above; clingo's weak-constraint priority
levels (`[w@l]`); Grosof's courteous logic programs (prioritized conflict
handling for rules at business scale, though pairwise); and the legal
tradition — *lex superior* (constitution > statute > regulation) is a
ladder, *lex specialis* is the scope tiebreak, and centuries of case law
suggest the two-mechanism structure is stable.

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
  narrative language (§12.1), a quest editor, a third-party tool —
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
M1/M2 hard deliverable rather than a convenience. Any future front end
(§12.1) targets the same artifact and gets no privileges the header lacks.

**Erasure is a rule, not an accident:** no surface construct may require
runtime representation beyond engine structures. Bands erase to pairwise
edges (§6.2), thresholds to guard atoms and entailment rules (§5.8), types
and vocabulary closure to nothing; the M3 pipeline erases defeaters and
superiority within the logic itself. **Erasure is now total** — with the
weave gone (§12.1), no surface construct has a runtime shadow. The
expression VM (§5.8) is not an exception: it evaluates numeric right-hand
sides, which are engine machinery, not a checked construct's residue. Any
future front end that wants a bytecode backend re-opens this rule
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
  ~ms budgets. Full recompute per action, always.
- Global tier: hundreds of thousands of facts; event-driven wake-ups cost ∝
  changes, not database size. Never maintain non-monotonic conclusions
  incrementally — scope the recompute instead.
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
a future front end can reuse it — §12.1) → AST in arenas → semantic passes
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
   a rename in `.story` breaking the host build. Playable cellar in raylib
   driven entirely by host code against the generated header. A trivial
   second client in tests pins the no-private-APIs claim (§4.2) the way
   golden tests pin semantics — with no reference client, this test is the
   *only* thing keeping the client boundary honest, so it is a hard
   deliverable, not a nice-to-have.
4. **M3 — engine hardening**: Maher linear algorithm + transformation
   pipeline behind the same API; tick-time join matcher for variables/typed
   entities (until then: ground rules per entity by hand/codegen).
5. **M4 — scale spine**: global tier (subscriptions, dependency cones),
   scene partitions, **nested scopes with lifetime/visibility (§5.5)**,
   serialization, hot reload.
6. **M5 — proof-of-thesis demo**: one region, ~20 NPCs, a 5e-ish combat slice
   where conditions/feats interact through superiority, one multi-step quest,
   `why?` in the UI — all driven by host code against the generated header
   (§6.3). The quest is the interesting half: a multi-step quest with no
   narrative layer is the honest test of whether rules alone carry story
   state, and of what §12.1 will actually need to add.

## 12. Open questions

### 12.1 Deferred: the narrative layer

An Ink-style weave (knots, choices, diverts; a bytecode VM as a client) was
designed into earlier revisions and **removed to keep the project focused on
the rules engine** — a scope decision, not a repudiation. It was cheap to
remove because it was already a client (§4.2) and a separate front end
(§6.3), holding no privileges and no primitive status in the core language;
see git history for the removed text.

**The seam.** A narrative layer re-enters as a front end consuming the
interface artifact (§6.3) and a client on the public `world_*` surface
(§4.2) — the same terms any third-party tool gets. It should externalize
its state (§4.2), or knowingly accept that conversation state sits outside
the save. Two constraints bind it:

- **Erasure (§6.3) is now total.** Weave bytecode was its one deliberate
  exception. A narrative backend re-opens that rule and must argue for it
  rather than inherit it.
- **It must not become a scripting language.** Ink grew variables,
  functions, and arithmetic, and that is the muddiest part of Ink.
  Computation belongs in rules (truth) or providers (services), where it is
  declared, checked, and traced — the same §2 posture that refused
  logic-side Turing-completeness.

### 12.2 Live questions

- ~~Scalar and functional fluents~~ — **resolved**: multi-valued fluents with
  strict-team defeat across values (§5.7); numerics via value store + landmark
  abstraction + closed effect operators, integer-only, stratified primed
  guards (§5.8). Remaining M1 syntax details: the exact effect-operator set
  and domain-declaration surface.
- ~~Concurrent numeric effects~~ — **resolved** (§5.8): combine by operator
  class through a fixed pipeline (base → Σ deltas → declared-range clamp);
  `:=` competes as a value conclusion under §5.7; commutative-associative
  admissibility gates the operator set; four static escape hatches, no
  timestamps. Remaining rules of thumb for concurrent *actions* generally
  (non-numeric interactions) still owed author-facing docs.
- Ambiguity propagation variant: blocked for now (predictability); revisit if
  authors want "conflicting rumors" semantics.
- Team defeat: currently on (matches intuition for "several weak reasons
  jointly outweighed"); needs author-facing docs either way.
- ~~Defeated-family withdrawal~~ — **resolved** (§5.7): assignments are
  reified as `fires_R` atoms, so a defeated value conclusion withdraws its
  whole family and "sealed blocks open, inertia keeps locked" works as
  prose intended. Withdrawal is per *assignment*, not per rule; conflict
  and superiority stay at the value level; booleans never reify. It erases
  to a fresh atom and ordinary defeasible rules — no engine change.
  Remaining M1 work is mechanical: emit the layer in the compiler, render
  `fires_R` in `dl_why` as the source assignment (§6.3 provenance — a trace
  naming `fires_R` directly would forfeit the moat), and measure the
  grounding cost.
- Template-scope identity (§5.5): spawning is scope instantiation, and
  instantiating a template more than once needs distinct entity ids per
  instance (two summoned wolves from one template). Decide the id scheme
  (scope-qualified atoms?) with M4's module system.
- Scope-depth superiority (§5.5): should `encounter > area > world` be an
  opt-in a rule requests, a per-scope default an author can flip, or always
  explicit? Leaning opt-in-per-rule for `why?` transparency; revisit once M4
  has real nested worlds to author against. Its precedence slot is now fixed
  — a tiebreak *below* bands (§6.2); only the opt-in form remains open.

## 13. References

- M. Maher, *Propositional Defeasible Logic has Linear Complexity*, TPLP 1(6), 2001.
- G. Antoniou, D. Billington, G. Governatori, M. Maher, *Representation
  Results for Defeasible Logic*, ACM TOCL 2(2), 2001.
- M. Maher, A. Rock, G. Antoniou, D. Billington, T. Miller, *Efficient
  Defeasible Reasoning Systems*, IJAIT 10(4), 2001. (Delores.)
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
- inkle, *ink* — https://github.com/inkle/ink (surface model for the
  deferred narrative layer, and its cautionary half: §12.1)
