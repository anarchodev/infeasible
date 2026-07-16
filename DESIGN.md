# Infeasible — Engine Design

*A narrative game engine in C where the world is a logic database: defeasible
rules for judgments, defeasible inertia for change, and an Ink-style weave on
top. raylib for presentation, CMake for builds.*

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

- One custom language (working name: `.story` files) spanning three layers:
  declarations + rules, actions, and Ink-style weave.
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

- **Ink (inkle)** — surface model for the weave: knots, choices, diverts, gathers.
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

Three tiers, one semantics:

```
┌────────────────────────────────────────────────────────────┐
│  Weave (Ink-like VM)                                       │
│  knots / choices / diverts; text alternatives              │
│  guards = defeasible QUERIES     choices = fire ACTIONS    │
├────────────────────────────────────────────────────────────┤
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
  through evaluated guard literals (`hp(X) < 10`).
- **Inference** runs in two modes over the same rule format: *query* mode
  (what is defeasibly provable now?) and *step* mode (given occurring actions,
  what are the next base facts?).
- **Weave** compiles to a bytecode VM (later milestone). It cannot mutate
  facts except via `do <action>`.

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

**Functional fluents are the one new primitive this forces**, and they are the
scalar-fluent open question (§12) in another guise. Closed-world (§5.3)
generalizes from "`f` or `~f`" to "exactly one `at(X)=c` per tick"; inertia
generalizes to "position persists unless a move fires" — Yale-shooting for
coordinates, correct by the same construction. Deciding functional fluents is
owed to the design regardless; space is its first heavy consumer, so pin it
before M1 fixes the fluent syntax.

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

### Invariants (compiler/engine enforced)

- **I1 — No write-back.** Derived conclusions are never stored as base facts.
  Storing one recreates Osiris's stale-fact problem and breaks purity.
- **I2 — Actions are the only mutation.** The weave and all gameplay code
  change facts exclusively via the step function.
- **I3 — Providers are dependencies.** Index-backed guards invalidate their
  cones when their underlying index changes.
- **I4 — Determinism.** No wall-clock, no unseeded randomness inside the
  logic core; a save is base facts + action log; replay is exact.

## 6. Language sketch

See `examples/cellar.story` for the running example. Construct → tier mapping:

| Construct | Compiles to |
|---|---|
| `entity`, `fluent`, `scene` | fact-store schema; partition declarations |
| `rule … -> / => / unless`, `A > B` | defeasible theory (strict/defeasible/defeater/superiority) |
| `action … requires … causes` | causal rules for the step function |
| bare `causes` rules | ramifications |
| `=== knot ===`, `+`/`*` choices, `->` diverts, `{ … }` guards | weave VM; guards are defeasible queries |
| `do action(...)` | fire the step function |

Authoring principles: authors never see primed atoms, inertia, or time
indices; `unless` sugars to a defeater; the same guard syntax works in rules
and weave; conclusions are typed distinctly from fluents so I1 is a type
error, not a runtime surprise.

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
   lever and it is domain-specific, so it must be designed into the rule
   grammar from the start.
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
land with the parser. Item 3 is a rule-grammar decision and must be settled
*before* the M1 grammar freeze. Items 5–8 track M2–M5 tooling but their data
(traces, action log, dependency cones) already exists in the scaffold.

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
  store is an explicit design act.

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
- Later: rule hot-reload (sound because conclusions are derived), fact-store
  diff viewer between steps, dependency-graph visualization.

## 10. Parser plan

**Hand-rolled recursive descent** (decided). Rationale: full control over
error messages and recovery (author-facing tool, so "expected `=>` after rule
body, found `->`" quality matters), no generator dependency, trivial to embed
significant-indentation weave blocks alongside declaration syntax. Structure:
hand-written lexer → recursive descent with Pratt expression parsing for
guards/arithmetic → AST in arenas → semantic passes (types, safety,
stratification, conflict pairs, partitions) → ground/compile to engine
structures. Panic-mode recovery at declaration and knot boundaries so one
error doesn't cascade.

## 11. Milestones

1. **M0 — this scaffold**: core (arena/intern), defeasible engine with
   query + why, step function with inertia/ramifications/conflict detection,
   golden tests (Yale shooting, cellar, torch ramification, conflict), raylib
   app shell with an interactive cellar demo.
2. **M1 — language front half**: lexer + recursive-descent parser for
   declarations/rules/actions; semantic checks; `cellar.story` compiles and
   replaces the hand-built test worlds.
3. **M2 — weave**: knot/choice/divert VM, text alternatives, guards, `do`;
   playable text cellar in raylib.
4. **M3 — engine hardening**: Maher linear algorithm + transformation
   pipeline behind the same API; tick-time join matcher for variables/typed
   entities (until then: ground rules per entity by hand/codegen).
5. **M4 — scale spine**: global tier (subscriptions, dependency cones),
   scene partitions, **nested scopes with lifetime/visibility (§5.5)**,
   serialization, hot reload.
6. **M5 — proof-of-thesis demo**: one region, ~20 NPCs, a 5e-ish combat slice
   where conditions/feats interact through superiority, one multi-step quest,
   `why?` in the UI.

## 12. Open questions

- Scalar fluents: how much arithmetic lives in guards vs providers; syntax for
  assignment effects (`causes hp(X) -= damage`). Includes **functional
  fluents** (exactly-one-value, e.g. `at(X) : cell`, §5.6): closed-world and
  inertia must generalize from `f`/`~f` to a single-valued domain, and space is
  the first heavy consumer, so settle this before the M1 fluent syntax.
- Concurrent actions in one step: allowed (the step function already takes a
  set) but needs author-facing conflict rules of thumb.
- Ambiguity propagation variant: blocked for now (predictability); revisit if
  authors want "conflicting rumors" semantics.
- Team defeat: currently on (matches intuition for "several weak reasons
  jointly outweighed"); needs author-facing docs either way.
- Scope-depth superiority (§5.5): should `encounter > area > world` be an
  opt-in a rule requests, a per-scope default an author can flip, or always
  explicit? Leaning opt-in-per-rule for `why?` transparency; revisit once M4
  has real nested worlds to author against.

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
- Larian's Osiris: DOS2/BG3 modding documentation (community wiki).
- inkle, *ink* — https://github.com/inkle/ink
