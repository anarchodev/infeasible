# Infeasible — Engine Design

*A narrative game engine in C where the world is a logic database: defeasible
rules for judgments, defeasible inertia for change, and host code driving it
through a generated, vocabulary-checked API. A hand-written Canvas2D web
renderer for presentation, CMake + WASM for builds.*

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
- C11 + CMake for the engine; the shipped presentation is JS + a hand-written
  Canvas2D renderer over a WASM build of the core. Renderer-agnostic logic core
  (pure functions over fact stores; the presentation client touches nothing
  below the frozen presentation interface, §12).

**Non-goals**

- AAA presentation. Canvas2D at the StarCraft-1 ceiling caps us at indie
  visuals; the logic core doesn't care.
- General-purpose logic programming. The language is game-shaped; escape
  hatches go through C providers, not logic-side Turing-completeness.
- GOAP-style planning (defeasible action theories make planning research-grade;
  revisit only if NPC planning becomes core).
- A narrative/dialogue layer. Games are built as host code against the
  interface artifact and its typed binding (§6.3); a knot/choice/divert front end would be a client
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
- **Set-at-a-time evaluation (Datalog)** — semi-naive evaluation, bddbddb's
  BDD-encoded relations, Soufflé's specialized relation structures: operate
  on the set of tuples, not the tuple. §5.8's columnar backing is this move
  applied to defeasible proof statuses. The defeasible implementation line
  (Delores, SPINdle) never took it — tuple-at-a-time throughout — because
  its workloads (law, business rules) are single heterogeneous theories with
  no homogeneous family to vectorize over; that shape is what an
  entity-populated game world produces and a statute book does not. Both
  ingredients are prior art; the composition (set-at-a-time × team
  defeat/superiority × `why?`) appears to be ours.
- **Demand-driven evaluation** — one recurring idea across five literatures:
  magic sets (demand as program transformation), tabling/XSB (the top-down
  memoized dual; Deimos is defeasible logic's own query-driven half), lazy
  ASP grounding (rules ungrounded until relevant — the grounding bottleneck
  is §5.2's premise), demand-driven incremental computation (Adapton's
  dirty ∩ demanded is §4.1's wake-up rule as a calculus), and
  subscription-maintained views (differential dataflow; LARS for the
  nonmonotonic-streams case). §4.1's demand cone stands on all of these.
  Two of their lessons are load-bearing: demand under negation must close
  over attackers (naive magic-sets-with-negation was unsound — the reason
  §4.1's cone drags in complements and superiority competitors), and demand
  bookkeeping is not always a win (the reason §8 makes sweep-vs-track a
  per-family compiler choice rather than a global strategy).
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

**The surface is one primitive, and it is already there.** `world_subscribe`
takes a literal — base fact or derived judgment, no distinction at the call —
and each step reports which subscribed literals flipped, alongside the level
each one now holds. The pair is what a loop wants: decide from the level, react
to the edge (§12's `btn`/`btnp` shape). Naming a *conclusion* rather than a
storage location is what makes a fluent refactored into a judgment, or back,
invisible to every client; the two cases differ in what they cost, which is a
warning, not a second call shape. What the cone above buys is that cost — today
a subscribed step re-solves the judgment family, the same solve the client's
first query would have paid; the demanded-and-reachable narrowing is M4 and
changes no answer.

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
  the proof. Because there is no blessed client, the test is carried
  entirely by having a *second* one, in tests, pinning the claim the way
  golden tests pin semantics (§11).

  A second client is evidence only if it is genuinely a second one, so it
  is built to be unlike the browser cart in every dimension that could
  hide a private dependency: a different language (C against `world_*`,
  not JS against the §6.3 binding), a different presentation (a text
  frame), and — the load-bearing one — a different *architecture*. The
  cart is **polled**: it re-asks the world everything it draws, every
  frame. The second client is **reactive**: it polls once at startup and
  after that maintains its picture only from what a step reports, the
  subscription edges plus the numeric changeset, never re-reading a fluent
  it already knows about.

  That difference is what turns the test from "it compiles" into a
  check with a failure mode. A reactive client's cached picture must equal
  a freshly polled one at *every* tick, and the test asserts exactly that
  after every step — so a subscription channel that misses an edge, or
  reports one no step caused, separates the two pictures and fails. It is
  the only check that the reactive channel is **complete** rather than
  merely correct about what it does report.

  The two clients meet on one artifact: an action log (§12's save). The
  cart plays the world by clicking and asserts the log it produced is the
  committed fixture; the second client replays that fixture and asserts it
  lands in the same world, offering the same commands. Neither client can
  drift into being the special one, because the save is all they share.

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

**Status.** The engine implements the standard semantics with a tri-valued
fixpoint over pre-ground rules. The linear-time path (M3) is factored into a
build-time half and a run-time half, both behind the same API, with the golden
tests pinning the semantics so each swap is safe. **The run-time half has
landed** (item 2 below): the columnar engine — the one that actually runs a
tick, `dlcol_solve` under every `world_step`, judgment family and lane —
evaluates in SCC-scheduled order, and the schedule is compiled once with the
rest of the schema, so a tick pays only the walk. The scalar reference
(`dl.c`, the `.story` kind stratum's build-time solver) keeps the plain sweep
as its default and carries the ordered driver as `dl_solve_scc` alongside the
order-independent worklist `dl_solve_wl`; both are differential oracles
(`test_drivers_agree`), and neither wins there because that path recompiles
per solve, so a schedule it cannot amortize costs more than the rescans it
saves. **The build-time half — the Antoniou transformation and the
stratifying compiler of item 1 — is what remains.**

1. *Transformation + stratification (build time).* The size-preserving
   Antoniou transformation (TOCL 2001) eliminates defeaters and compiles away
   the superiority relation, reducing the theory to *basic* form (strict +
   defeasible rules only); the compiler condenses the rule-dependency graph into
   its strongly-connected components and applies the decided cycle rule below
   (bands and scope-depth priorities have already erased to `>` upstream, §6.2,
   so the transformation never sees them). This is Maher's own normal form — the
   load-bearing half, shared by *any* linear evaluator.

   **Cycle rule (decided): recursion is Datalog; defeat does not recurse.**
   Boolean cycles cannot oscillate — the proof tags are monotone, so a cyclic
   support graph converges; it can only *stall* loop-locked literals at
   UNDECIDED. (This is why the answer here differs from the primed-numeric
   cycles §5.8 rejects: those genuinely oscillate at runtime; these are an
   ergonomics decision, not a soundness one.) The stall is a pothole: a
   failing rule body yields REFUTED on an acyclic graph and UNDECIDED on a
   cyclic one — the verdict changes with the graph's shape, not the world's
   meaning, and UNDECIDED elsewhere means "contested". The decided semantics:

   - **A support-SCC may be cyclic iff none of its literals is attacked
     anywhere in the theory** — no rule concludes a member's complement, no
     defeater targets a member. Such an SCC is plain Datalog by construction:
     least fixpoint inside (supported literals already prove through cycles
     today), then every underived member **completes to REFUTED**. The
     completion is trivially sound there because no conflict-flavored
     UNDECIDED can exist in an unattacked SCC; the why-trace for the refuted
     side is "no support: every derivation re-enters the cycle", rendered
     with the loop's members. This is what makes rule-authored recursive
     relations — command chains, contagion, ally-of-ally, crafting trees —
     first-class. (Spatial reachability stays provider territory, §5.6;
     the recursion legalized here is exactly the kind a provider would wall
     off from mods.)
   - **A cycle containing an attacked or defeater-targeted literal is a
     located compile error** naming the loop and the attacker — the same
     diagnostic posture as §5.8's oscillator rejection. Defeasibility and
     recursion both exist, in disjoint zones, exactly as stratified negation
     separates recursion from negation in Datalog; the naive completion
     would otherwise silently resolve a genuine conflict (UNDECIDED has two
     causes — loop-starvation and tied attack — and only the first may
     complete). Well-founded defeasible logic (Maher & Governatori) is the
     documented escalation path if defeat-through-recursion ever earns its
     way in; nothing currently points that way — conflicts resolve at the
     definition level (layers, bands), not through recursive entanglement.
   - **Module interaction.** A `scene` overriding a recursive relation
     attacks its own *import*, not the source atom (§5.5 private
     vocabulary), so scene-local overrides of recursive relations stay
     legal. Only an `extend` attacking a recursive head directly hits the
     error, and deliberately so: defeat cannot reach through a cycle, and a
     loud compile error beats a silently wrong completion.
   - **Conservativity.** Acyclic theories are untouched — the existing
     golden suite lives entirely in the acyclic zone and pins it. New
     goldens: transitive closure over base facts (reachable pairs PROVED,
     unreachable pairs REFUTED with the loop trace); an attacked cycle
     rejected naming loop and attacker; an acyclic differential asserting
     the completion pass changes nothing.

   Neither the check nor the completion needs to wait for the M3 evaluator —
   the SCC analysis runs at grounding time and the completion is one pass
   over the solved statuses; the matcher's derived-body widening (#44)
   inherits recursion as ordinary semi-naive iteration on the extension
   index, a scope item, not a semantics question.
2. *SCC-ordered sweep (run time; landed).* Rather than Maher's
   counter-and-worklist algorithm (Delores, TPLP 2001), the evaluator is a single
   *weak-topologically ordered* sweep: solve the SCC condensation in topological
   order, iterating only within a genuine component to a local fixpoint. On a
   transformed, near-acyclic theory the components collapse toward singletons and
   one ordered pass decides everything in O(N) — the same linear bound, reached
   by *scheduling* the sweep rather than by dynamic counters. Chosen for
   engineering fit, not theory: it keeps the branchless sequential scan that
   already wins the dense/shallow scene-tier workload and the entity-indexed
   bitvector lift of the columnar backing (both §8), pays none of the worklist's
   random-access dependency-chasing, and confines residual iteration to the SCCs
   where cycles actually remain. It runs over the *untransformed* theory today —
   which is already enough to remove the sweep's sole weakness, an O(n²)
   scan-order cliff on adversarial deep chains — and the transformation above
   will only shrink the components it walks.

   **The schedule is condensed from the dependency graph, not the support
   graph** the cycle rule uses. The distinction is load-bearing and easy to get
   wrong: deciding a literal reads the bodies of the rules that *attack* it as
   well as those that support it, so the edge set is `body → head` **and**
   `body → head^1`, defeaters included. A schedule condensed from support edges
   alone would evaluate a literal before the attackers that refute it. That
   double edge also buys the columnar engine its per-rule applicability refresh:
   a rule's bodies are predecessors of both its head and its head's complement,
   so whichever is visited first, they have already settled — the evaluator
   refreshes the rules it is about to read instead of every rule once per pass.
   The two condensations coexist because they answer different questions; the
   cycle rule's is about *meaning* (which loops may complete to REFUTED, §5.2
   above), the schedule's is about *order*.

**Precedent and honest status.** SCC-decomposed evaluation of a monotone
fixpoint is a standard, independently-rediscovered technique in the fields
adjacent to this one: *SCC-recursiveness* for abstract-argumentation semantics
(Baroni, Giacomin & Guida, AIJ 2005 — Dung's semantics computed
component-by-component, on the principle that an argument's status depends only
on its ancestors), *weak topological ordering* for abstract-interpretation
fixpoints (Bourdoncle 1993, the de-facto iteration strategy), and stratified/SCC
evaluation in bottom-up Datalog (semi-naive per component, topological over the
condensation). The DL-specific *linear* implementations of record (Maher's
Delores; SPINdle) instead take the counter route, so wiring the SCC schedule to
defeasible logic's proof conditions is our synthesis, not a published DL
result — it is validated by the differential golden tests, not by an external
theorem, and it had to be *measured* to earn the swap (Delores is the cautionary
case: its transformation, linear in theory, did not behave linearly in the
shipped system).

**What the measurement said.** Columnar solve is **1.7–1.9× faster** across the
scale range — `bench_col` 0.30 → 0.18 ms at N=100k, `bench_5e` 0.42 → 0.22 ms
at N=100k, `bench_slice`'s judgment phase 0.030 → 0.017 ms mean — with the
replay hash unchanged, which is the differential stated in the units that
matter. Two results carry more weight than the ratio. First, the win is *not*
the cliff: these are the dense, shallow, forward-ordered theories the plain
sweep was already best at, and the schedule beats it there anyway, because
visiting a literal once with settled inputs beats rescanning it, and because
refreshing only the rules about to be read beats refreshing every rule once per
pass. Second, the cliff goes too — `bench_dl`'s `rev` mode reverses the atom
numbering of an otherwise identical theory, and the plain sweep runs 0.43 → 2.74
ms across that relabelling while the ordered one holds at 0.52. Scan order stops
being a performance variable, which is the robustness half of the claim:
reordering declarations in a `.story` file cannot silently cost 5×.

The scalar reference is the honest exception. There the schedule build (~0.59 ms
of a 0.74 ms solve at 4000 atoms) is paid per call, because that path recompiles
the theory on every solve, so it does not pay for itself on forward-ordered
theories even though the ordered *evaluation* alone is ~3× cheaper. The columnar
engine has the opposite shape — schema and schedule compile once per rule-set
change (§4.1's two-tier split; the incremental surface of `dlcol_truncate_rules`)
and are reused every tick — which is why the swap lands there and stays a driver
choice here. Consistent with §8 this remains a conditional optimization: the
counter form's real prize is incremental cone re-solve (M4), and the compiler
stays free to pick per workload.

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
  time, and now statically: the conflictable-pair pass (#98, §5.13) warns on
  any effect pair that could contest — a zero-warning story cannot take this
  path.
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

**A provider must be able to explain itself, or the trace has a hole in it.**
The more faithfully a game takes the advice above — dense computed relations
are host territory, the logic joins only sparse results — the more of its
interesting reasoning ends up behind a callback that answers yes or no and
accounts for nothing. `los(a,b) [PROVED]` is the whole story the trace can
tell, while *what* the ray hit is sitting in the host's answer and is
discarded. That is the exact failure `why?` exists to prevent: the author sees
*that* the engine believed something and goes reading host code to find out
why.

So both provider kinds carry an optional account, and both are strictly
**trace-time**, because an explanation must not be able to change what it
explains. A boolean relation takes a render callback phrasing one ground
answer, appended where the trace already prints the atom
(`near(g,i) [manhattan 4 > 2]`); registering none leaves traces byte-identical,
which is what makes it additive. A value-returning function takes an opt-in
call log — the calls a step made, with their arguments and results — because a
receipt can say a number changed by 3 but not that `neighbor(at(x), north)`
answered it. The log is off by default: the provider boundary is the hot path
(§8.1), and it is a side-channel that cannot alter a call's answer, so a run
with it on and one with it off are the same run (I4).

**Golden test to pin it:** a `move` action changes exactly one actor's cell and
leaves every other position inert across the step (spatial Yale-shooting); a
proximity rule fires iff the provider reports the actors within range, and
recomputes when a move changes that range (I3); a rule with no spatial
anchor over a populated grid raises the cardinality warning rather than
grounding the cross product; and the proximity rule's trace carries the host's
own account of the distance it measured.

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
  rule. (This closed-world promise is scoped to guards over *stored*
  numerics: a guard over a partial derived value is genuinely tri-valued —
  when no definition applies it asserts neither fact, §5.13/#116.) On a value change the provider re-buckets (one binary search over the
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
  renderer side of the I4 wall, which is the same line as the presentation-client wall.
  Integer division is **floored** — the quotient rounds toward −∞ (`-7 / 2 = -4`),
  matching 5e's "round down", *not* C's truncation toward zero; the semantics are
  pinned by golden test, not inherited from the host. Division by zero is defined
  as 0 at runtime; a divisor that constant-folds to 0 is a compile error.
  Round-up sites — 5e's explicit per-feature exceptions ("half your level,
  rounded up") — are `divup(a, b)`, a named builtin beside `min`/`max` that
  desugars to `-((-a)/b)`, so ceiling can never drift from floor. Rounding is
  never a mode or a property of `/` itself: which rounding applies is always
  visible at the expression.
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
numeric guards is the degenerate one-stratum case. How strata compose with
the commit pipeline, operator classes, and layered definitions — and the
cost model for the composition — is consolidated in *"One tick, in
order"* below.

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
  = contested step, statically detected by the conflictable-pair pass (#98,
  §5.13). No new semantics.
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
   *Status (decided, 2026-07-30)*: the modeled form is the ONLY form. The
   first shipped form was configured — #83/#84's per-type commit stage
   hardcoded the 5e response algebra (immune → 0, resistant XOR vulnerable
   → floored halve / double, both cancel, PHB p.197) in engine code while
   the modeled form waited on stratification (#87). Once strata, `test()`
   guards, and primed reads landed, the equivalence gate ran (modeled ≡
   configured pinned golden; 1.88–1.96× at 2k/10k/100k N=1, the gap being
   the second stratum's extra solve) and the #84 decision REMOVED the
   configured stage and its `as <type>` surface: the measured 2× lives
   only in the N=1 regime the M3 SCC sweep is built to change, the lane
   cost model below prices the modeled form at near-parity, and keeping
   the stage would have required the §6.1 invisible-backing promotion
   (shape recognizer + byte-identical trace unification) — compiler work
   needed by nothing else, to preserve a non-durable win. The modeled
   trajectories are pinned as absolute goldens (`test_modeled`, values
   carried verbatim from the retired stage); re-run `bench_dtype` when
   the M3 sweep lands. One caution the port confirmed: floored halving
   and doubling do not commute (7 → 3 → 6 one way, 7 → 14 → 7 the
   other), so resistance and vulnerability are *not* two stackable
   multiplicative layers — 5e's both-cancel is a third rule defeating
   the other two, authored and traced; #94's commutation check rightly
   refuses the two-undefeated-layers encoding.

   **Two forms, and the choice is the author's (decided).** The paragraph
   above describes the pipeline as layered derived values; `test_modeled`
   pins it as transient accumulators with a response over the total. Both are
   real, both are authorable today, and they are not the same semantics —
   so the surface offers both and the author picks by what a *step* means in
   their game.

   - **Per instance** — `value fire_dmg(unit) : int` with a `prior /2` layer
     for resistance, ordered against vulnerability and immunity, and each
     attack committing `hp(T) -= fire_dmg(T)`. Mitigation happens per
     contribution and the deltas sum order-free. No accumulator, no primed
     read, no stratum.
   - **Per total** — a transient accumulator (`inc_fire`) summing the tick's
     contributions, and a response reading `inc_fire(X)'` and subtracting
     once. This is what a *simultaneous batch* means: several contributions
     that resolve as one instance.

   They differ in exactly one place: floored division does not distribute
   over addition. Two 3s against resistance are `floor(6/2) = 3` per total
   and `1 + 1 = 2` per instance; doubling agrees either way (it distributes),
   immunity agrees, and unmitigated damage agrees. So the divergence is a
   rounding boundary, not a different model of damage.

   Which is right depends on what a step is, which is the *host's* choice of
   tick granularity and therefore not the engine's to make. A step that
   resolves one attack never distinguishes them. A step that batches
   simultaneous effects does, and then per-total is the claim that the batch
   is one instance.

   **Per instance is the recommended default**: it is the cheaper shape — no
   accumulator to reset, no primed read, hence no stratum, and every stage is
   a marker judgment the trace can interrogate rather than arithmetic inside
   one effect expression. Reach for the accumulator when a step really is a
   batch, or when a rule needs the aggregate itself (*"if total damage this
   round exceeds 50, stagger"*), which per-instance cannot express at all —
   that is the shape §5.8's primed read exists for, and no reformulation
   removes it (the additive-fluent result below).

   The cost of the per-instance form is the ordering #94 demands: resistance,
   vulnerability, immunity and both-cancel are mutually applicable and
   non-commuting, so they need a total order — a `bands` ladder (§6.2) rather
   than the pairwise edges, which is what bands are for.

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

The receipt therefore reports **both ends of the pipeline**, not the committed
number alone: `base` (the winning `:=`, else the carried value), `raw` (base
plus the undefeated deltas) and `applied` (what the declared range retracted
that to). A 12-damage hit on a 5 HP target is raw −7, applied 0 — the overkill
is the difference, and absorption at a floor reads the same way. Recovering
that from the committed value alone is impossible, and a client that
subtracts to guess it is wrong the moment a dynamic bound moves.

Provenance is **structured, never a formatted name**: a contribution carries
the authored rule's predicate and its binding as ids (`fireball`, `C=vera`,
`T=grik`), so a client composes its own sentence. Handing back only the ground
instance name `fireball[C=vera,T=grik]` would make a rendering choice the
interchange format and force every host to parse it back apart.

A **defeated contribution is a row too**. An effect whose action was submitted
but whose rule failed its guards contributes nothing and would vanish from a
log of winners — yet "Immune — 0" is exactly the line a player needs, and its
absence is indistinguishable from the attack never happening. It is recorded
with what it would have contributed, flagged. Ramifications are excluded on
purpose: a ramification that did not fire is not a thwarted attempt, it is
every other rule in the world.

Alongside the receipts a step reports its **changeset** — the base facts that
actually moved, enumerated rather than polled. It is the leaf case of the §11
M2 subscription delta, available now because the commit already compares old
against new to write it; a client asking "what happened?" should not have to
know which atoms to ask about first.
And the tempting alternative is deliberately rejected: an accumulating
damage *buffer* that rules append to and a later phase drains is mutable
intermediate state with an ordering — the Osiris disease wearing a queue
costume. The "list of damage to apply" is a projection of one fixpoint's
winners, never a store.

Deliberately not offered: per-rule stage reordering (recreates the conflict
one level up), timestamps in any costume, and a content-configurable global
pipeline (fixed stage order is why the system is learnable; MTG's pain is
the timestamps, not the fixedness).

**One tick, in order — strata, stages, classes, layers (decided).** The
numeric tier now carries four ordering mechanisms, each introduced for its
own reason: strata across values (the primed-guard layering above), the
fixed commit pipeline within one fluent (base → Σ deltas → clamp; the
per-type response stage is modeled content, not a pipeline stage, per the
#84 decision above), operator classes within one accumulator (set before
add; admissible merges), and layered `prior` definitions within one
derived value (#82/#94). They compose because their jurisdictions are
disjoint, and the composition is the decided semantics of a tick:

1. *Strata order values.* The compiler condenses the dependency graph —
   numeric fluents connected through primed guards; boolean and
   multi-valued primed reads stay free, the fixpoint being their
   evaluator — and assigns every numeric fluent to the stratum where its
   writers settle; a primed-numeric cycle is a located compile error. A
   tick runs strata in order; the state write is atomic at the end and
   the action log records one step (I4 — replay never sees a half-tick).
2. *Within a stratum*, the sandwich above is unchanged: evaluate (guard
   atoms from the value store plus every settled lower stratum) →
   propagate (pure fixpoint, no arithmetic) → commit-compute the pipeline
   for the fluents this stratum owns, then mint their primed guard atoms
   as strict inputs to the strata above.
3. *Within one fluent's pipeline*, operator classes order contributions;
   *within one derived value*, `prior` chains order definitions by
   superiority, commutation-checked by class (#94). Nothing anywhere
   orders by declaration, commit time, or timestamp — the prohibition at
   the top of this passage holds at every level.

**The two accumulator kinds, and why the response stage reads primed
(decided).** The additive-fluent literature separates exactly the two shapes
this tier uses. *Additive-inertial* — new value = old value + Σ contributions
— is an ordinary numeric fluent under `+=`/`-=`, carried across ticks by
inertia. *Additive-default-zero* — new value = Σ contributions, and 0 when no
action contributes — is the per-tick transient accumulator, spelled here as a
`:= 0` base plus `+=` deltas so that the reset is an ordinary operator-class
commit rather than a second lifetime rule.

The consequence worth pinning is a *lower bound*, not a technique: an effect
that depends on the **total** of concurrent contributions must read that total
after the contributions land. C+ reaches the same place from planning — a
constraint on a total is written over an auxiliary additive-default-zero
fluent, read in the resulting state (`caused false if departed(G,L) eq M after
num(G,L) eq N && M > N`). So the response stage's primed read is inherent to
aggregate-then-respond and no remodeling removes it: a formulation that
mitigates each contribution as it lands is a *different semantics*
(per-contribution rather than per-total), not the same one written better. The
lane frontier's answer is therefore to widen the evaluator, never to rewrite
the pipeline.

One difference from that prior art is ours to close: there the kind is
**declared**, here it is **inferred** from the presence of a primed guard.
Inference is why a single accumulator anywhere puts a whole world on the
general stratified path — a compiler told that a fluent is default-zero knows
its commit pipeline's **base stage is 0 rather than the carried value**, and
that the shape is a fixed two-phase commit, which is a pattern to lane rather
than a hazard to retreat from. (Stated as the base stage because that is where
it lives: a numeric fluent's persistence is the carried value entering
`num_commit`, not a generated inertia rule — those are emitted per *boolean*
fluent. The `:= 0` ramification an author writes today is exactly a base of 0,
restated as content.)

One corollary is decided with it — **commit-time visibility**: a judgment
consulted from the commit (a verdict tested in an effect expression)
reads the fixpoint as settled at its own stratum — the latest settled
state, never a partially-propagated one. The retired #84 stage's
behaviour (responses solved over the pre-step state) was exactly this
rule in the degenerate one-stratum world, so landing strata changed no
existing world's replay; golden tests pin each side (pre-step in a
one-stratum world; a resistance concluded in a lower stratum of the same
tick applies to damage committed above it).

*Cost model* — why the modeled pipeline fits the lane budget:

- **Strata do not multiply the fixpoint.** The committed M3 evaluator is
  an SCC-condensed weak-topological sweep (§5.2) — dependency order
  already *is* the evaluation order. Strata add arithmetic hooks at the
  boundaries where a primed-numeric arc crosses, not extra sweeps:
  propagation stays ~one pass (the decided-skip freezes everything
  settled below), and each boundary costs one pipeline pass over that
  stratum's fluents plus a threshold re-bucket per written value (one
  binary search; bit-column compares on lanes). The stratum count is
  static and small — 5e's damage → `hp'` → `dead'` → on-death cascade is
  three. No primed numeric guards ⇒ zero boundaries ⇒ today's tick.
- **Everything event-shaped is event-priced.** Typed buckets are commit
  scratch keyed by fired effects — O(events), never O(N × types) storage
  — and a `prior` chain walks only where a definition actually fired
  (no-reference-no-walk is static, #82). A 100k-unit battle in which 5k
  units take damage prices the pipeline at 5k.
- **Layer programs compile like expressions.** A value head's definitions
  sort once at build (superiority is static) into a fixed sequence of
  (guard, class-op, expr) triples — bytecode, like the effect VM. N=1
  evaluation is a short chain walk; on lanes it is evaluate-all-and-mask:
  per layer the guard is an already-solved bit column and the update is
  `v = select(mask, f(v), v)` — branch-free, 64-wide masks, vectorizable
  arithmetic. This is the same loop shape as the retired configured
  response stage (three mask reads and a scale), generalized from a
  hardcoded 3 to a static K, with K ≈ 2–5 in 5e material — a bounded
  constant factor, not a new asymptotic.
- **Anchors and the adoption gate (ran; decided).** `bench_slice` steps a
  100k-unit battle in 2.5 ms/tick (judge+step ≈ 1.2 ms of it); the
  numeric commit is a fraction of the step share, so even 5× on commit
  arithmetic leaves order-of-magnitude headroom against a 16 ms frame.
  The gate was the prototype-before-adopt playbook: `bench_dtype`
  measured the modeled pipeline at 1.88–1.96× of the configured stage
  (2k/10k/100k, both N=1), inside the ~2× bound — and the #84 decision
  removed the stage rather than keeping it as a fast path (see the
  *Status* note under escape hatch 4 above). The modeled form is both
  the semantics and the implementation; the surviving lane work targets
  it alone.
- **Space is unchanged.** Layer programs mint no atoms (numbers never
  become atoms); guard atoms exist only for authored comparisons;
  accumulators and layer scratch die at commit. The save gains nothing.

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
lift the M3 SCC-ordered sweep to vectors indexed by entity id, pack boolean fluents
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
The trace is a backing too: `dlcol_why` reads verdicts, fact bits,
applicability, and superiority straight off the solved columns — no scalar
re-derivation — and the same test pins its output byte-for-byte against
`dl_why` on the grounded instance (atoms and rules render as
`name[entity]`).
`bench_col` (Release, per-unit AI family of 10 rules): full-family
recompute at 10k units 0.04 ms, 100k units 0.4 ms, 1M units 5.8 ms —
vs 5 ms / 65 ms / 660 ms for the same workload grounded into the scalar
solver (~85–165×), putting RTS-crowd judgment eval well inside a frame
budget. The sweeps carry a decided-skip (a literal or rule-applicability
row decided for every entity is monotone-frozen and skipped), which is
what makes the backing competitive at *small* W too — that plus the
cached schema is how `world_step` now runs: the step theory (judgments,
generated inertia, causal rules beating inertia) compiles once into an
N=1 family, and each step rewrites fact columns and re-solves — no
per-step theory rebuild, no allocation, modestly faster than the scalar
path at every world size (2.4 vs 2.7 µs/step at 10 fluents, 218 vs
276 µs at 1000) with the world golden tests (Yale, ramification,
conflict rejection) pinning the swap. `bench_slice` closes the loop end-to-end: a deterministic two-army
battle (toy spatial provider filling fact columns → columnar judge →
hosts acting on verdict columns via `dlcol_proved_row` → column-wise step
commit with summed damage, threshold guards, and word-wide inertia
`alive' = alive & ~died`), replay-hash-verified (I4), at 10k units
0.3 ms/tick median and 100k units 2.5 ms/tick median — the judge+step
share is ~0.1 ms and ~1.2 ms of that; the toy provider dominates, which
is the intended shape (spatial cost lives behind the §5.6 wall in any
engine). Heterogeneous partitioning, store-fluent guards, and the per-entity
family world tier (action columns, per-entity conflict reporting — the
M1 sorts/keyed-fluent API) remain open.

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
over stored numerics as closed-world strict inputs, never UNDECIDED — the
partial-derived-value exception is §5.13's) is pinned in
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
(Roll *kinds* — one modifier whose die lands on every roll of a category —
were first shipped as a bespoke keyword and are now the functor-position
modifiers of §5.12: the per-(member, binding) site cloning survives
unchanged underneath; only the selection surface moved.)
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

### 5.11 Turn phases and reactions (decided)

Combat moves through stages — declare, react, resolve, clean up — and
reactions (Shield, opportunity attacks) interrupt mid-resolution. Neither gets
a language construct. The whole area is a **pattern over existing machinery**,
proven end-to-end by `examples/reaction5e.story` (the #118 probe) and pinned
by `test_reaction`.

**Terminology ruling.** §5.8 spent *stages* on intra-tick order (strata ×
stages × classes × layers). These are **inter-tick**: always say *phases*.
The two never smear — a stage orders rules within one step's fixpoint; a
phase gates which rules are live across a run of steps.

**The pattern: a phase is a fluent.** `phase : { declare, react, resolve,
cleanup }` is an ordinary arity-0 multi-valued fluent (§5.7). Action legality
is a guard (`requires phase = declare & …`); phase **advancement is causal
rules and ramifications, never a host write** — the host submits actions and
empty steps, and the clock ticks itself:

```
rule to_react(A: actor, T: actor):
    phase = declare & pending(A, T)' & has_shield(T)   causes phase = react
rule skip_react(A: actor, T: actor):
    phase = declare & pending(A, T)' & ~has_shield(T)  causes phase = resolve
```

I2 holds (all mutation flows through steps) and the turn structure replays
from the action log alone (I4). Precedent: boardgame.io declares phases and
allowed-moves-per-phase as data with auto-advance conditions, and MTG turn
engines reify the turn as state; we take the semantics-by-convention half —
turn order *is* rules — and decline the construct, because guards and
ramifications already spell it and a construct would only re-state them.

**The reaction protocol (the named idiom).** A round decomposes into many
steps; the step boundary rule is: **a step is the largest unit nothing can
legally interject into**. Anything a reaction may interrupt must therefore
end a step, and the intermediate moments become ordinary fluents — MTG's
stack, reified one level deep:

1. **Declare** commits a step. The attack-in-flight is stored state
   (`pending(A,T)`), and the die and attacker's modifier **lock by
   snapshot** (`atk_die(A) := roll(20) & atk_mod(A) := atk(A)`). Rolls are
   per-tick lookups (§5.10), so a value that must survive its own multi-step
   resolution is written to the store *by design* — the locked roll is part
   of the attack's state, so it saves and replays for free (#129 tracks
   whether this earns `latch` sugar; the snapshot is the blessed spelling).
2. **The window is a judgment.** `can_react(T)` derives from the phase, the
   incoming hit, spell knowledge, and reaction economy. The host *reads* it
   and asks the player; it never decides it. The trigger judgment re-reads
   the locked roll against **live** derived values each tick — casting
   Shield layers +5 onto the `ac` value (#82), and the same locked d20 that
   hit in `react` misses in `resolve`. Retroactivity is a judgment flipping,
   not rollback machinery.
3. **The I4 logging split.** A *declined open window* is a choice and MUST
   be a logged action (`pass(T)`). A window that *cannot open* is skipped by
   rule off closed-world base facts (`~has_shield`, above) with **no log
   entry** — auto-advance is derivable, so logging it would be noise.
   (Gating window *entry* on the derived hit itself needs a primed judgment
   read, and closing an entered-but-empty window needs NAF over a derived
   predicate — both out today, tracked as #131; entry keyed on base
   capability facts is the workaround the probe ships.)
4. **Resolve and cleanup** are empty-step ramifications: damage commits
   where the (re-judged) hit still holds, death cascades in the same step
   via `hp'` (§5.8 strata), durations tick down and expire by rule.

One die, two tests falls out: the locked `atk_die` is compared against live
AC (hit) and against 20 (crit), and the crit doubles the damage die
branch-free with `test(crit(A))` (#86).

**The `split` hint (landed — the N=1 slice).** Knowing only a subset of
rules is live per phase is a *compiler* fact, and it is static reachability
— excluded rules are provably dead from declared structure before any state
is read (stratification's sibling: #87 orders rules within the tick, phases
stratify them across the turn). It is NOT idle-skipping, so it passes the
honest-path rule (§8): the bench that gated it (#120) churns every phase.
The surface is one annotation with **zero semantic content**, same species
as `merge`:

```
state phase : { declare, react, resolve, cleanup } split
```

The engine caches one step schema per value, selected by the **pre-step**
value: rules guarded on another value are omitted; inertia exists only for
the value's write-set (what live rules write or read primed); excluded
fluents commit by copy (dynamic-bounded numerics still re-clamp). Deleting
`split` changes time, never meaning — `test_split` pins split-on/off
byte-identical, and the probe story carries the annotation with unchanged
goldens. It also completes the loud-no-op ladder (#119): an action all of
whose rules are dead under the current value is a located step error naming
both ("no live step rule while `phase=declare`"). A primed read of a split
fluent is rejected (selection is by the pre-step value). Nothing about the
hint says "phase": any mode fluent qualifies (overworld/combat, day/night).
Measured headroom at 100k entities: 65% of the full-schema round statically
dead, 2.89× on the hand-narrowed simulation. The per-value **lane** schemas
landed with the mixed lane/N=1 route (#121, both slices): under each value
the erased split guard makes the per-actor residue lane-eligible, so one
lane family per value (holding only the fluents its live rules touch)
solves bit-parallel, while the leftover — the phase's own advance rules,
numerics, binders — steps on a *sparse* N=1 residue schema fed the lane
half's next-state as strict primed facts (the lane half is a stratum under
the residue, the same mechanism as §5.8's primed-guard mints). A residue
rule writing a per-actor lane fluent would straddle one fluent's writers
across the halves, so that world declines the families and steps pure N=1
— always sound, never silently wrong. Through the real path, the same
100k workload runs the full round at **2.11×** over the single-schema lane
baseline (the hand-narrowed ideal's 2.85× minus the measured residue
orchestration), equivalence pinned byte-for-byte against both the unsplit
twin and the simulation.

### 5.12 Kinds are rules (decided)

5e modifies by *category*: Bless touches "attack rolls and saving throws,"
Halfling Luck every d20 roll, Rage melee weapon attacks, Bracers of Archery
ranged weapon attacks — and the taxonomy those sentences quantify over is
layered, overlapping, and a **product, not a tree** ("melee weapon attack"
cross-cuts any nesting you pick). The decided construct
(`examples/taxonomy5e.story` is the worked probe):

**A kind is the boolean case of `value`, over the built-in meta-sort `value`
whose elements are the declared value symbols.** Membership is `fact`s;
hierarchy and unions are derived predicates; the taxonomy is defeasible with
ordinary superiority; and a modifier written once selects with ordinary body
atoms, its value variable standing in **functor position** — the single
genuine novelty, and the HiLog move: `V(A) = prior + roll(4)` looks
higher-order and grounds first-order, one layer per member, through the same
expansion machinery (dotted `L.value` labels, per-(member, binding) roll
sites, the commuting-layer class).

```
value ( save(value, ability)  attack(value, reach, source)  d20(value) )
fact  ( save(spell_save, wis)  attack(sword_atk, melee, weapon) )

rule saves_roll_d20(V: value, A: ability): save(V, A)      => d20(V)
rule attacks_roll_d20(V: value):           attack(V, _, _) => d20(V)

rule halfling_luck(A: actor, V: value): d20(V) & lucky(A) => V(A) = max(prior, 2)
```

**The slogan: the kind stratum is a world with no step function.** Base facts
plus judgment rules over a closed domain, sealed at world-build, evaluated by
the *same* DL engine with the same verdicts and the same `why`
(`story_compile_kinds_why` renders build-time traces in the runtime format —
the thrown dagger's `thrown_not_melee > melee_by_weapon` defeat reads exactly
like any runtime defeat). Its only consumer, the grounder, is two-valued, so
an undecidable membership is a **located authoring error**: an UNDECIDED
verdict means cyclic support (§5.2's cycle rule applies verbatim); a
CONTESTED membership — this logic is ambiguity-blocking, so an unresolved
conflict refutes *both* polarities — is detected as applicable support on
both sides with neither proved, and the error names the competing rules and
the superiority to add. Closed-world negation (`~magical(V)` — the sentence
that makes "nonmagical bludgeoning, piercing, or slashing" one named
predicate) is admitted only over facts-only kinds, because only those close
at build; negating a derived kind is the same located error shape.

**Staging is inferred from sorts, never declared.** A rule concluding a kind
runs at world-build and may read only the stratum; a rule reading kinds as
*selectors* while concluding runtime values is the legal direction — #95's
"static filter, never in the fixpoint" grown up from membership lists to a
solved theory. The boundary is enforced both ways with located errors ("kind
rules run at world-build and cannot read fluents — guard the modifier
instead"), and the LSP's symbol detail says **build-time** out loud.

**The vocabulary rule** that keeps open/closed coherent: *instances arrive at
load boundaries; vocabulary is sealed at world-build.* A save can introduce a
new goblin (§5.9's pools already say how), never a new damage type. Program
union is the composition model — a module contributes facts and rules, sealed
when the grounder takes the fixpoint; `taxonomy5e.story`'s bottom-of-file
"initiative is a dex check" (one membership fact, and Luck — written far
above, knowing nothing of initiative — covers it) is the single-file
degenerate case of exactly what a module system will do.

**The two-primitives statement** (the answer to construct proliferation):
sorts and kinds are the **membership** primitive — a named finite set you
quantify over at ground time, over entities and over value symbols
respectively; bands and layer chains are the **ordering** primitive; and the
grounder erases all of them — the engine sees none of this. Enum and MV
domains are neither: they classify a fluent's *range*, not things rules
quantify over.

**Retractions** (kept, per house style — each closes a plausible path):
- *Tag trees* (the GAS shape): a tree forces one decomposition order and the
  taxonomy is a product; GameplayTags' dotted paths hit exactly this, with
  wildcard conventions as the workaround. Derived predicates give as many
  overlapping taxonomies as the material needs.
- *An ADT surface* (`kind roll = attack(…) | save(…)` + patterns): right
  semantics, wrong surface — predicate arity, body atoms, and variables
  already are the sum, the pattern, and the wildcard; the grammar would be
  imported weight.
- *`kindset` / `open` / `+=`*: a named union is a derived predicate; module
  extension is program union — no keywords. Anonymity at the set level, never
  at the name level (every name still resolves to a declaration).
- *Membership lists*: became N facts; multi-membership is free.
- *The `kind` keyword itself* (PR #115's surface, removed): a kind is the
  boolean case of `value` and the stratum is inferred from sorts; loudness
  lives in diagnostics and hover, not the grammar.

**The probe's three gap findings, all landed.** Kind-level superiority
(#145): `halfling_luck > bless` between two modifiers desugars to the
per-member dotted pairs over their shared members; an explicit dotted sup
overrides the blanket per member (most-specific wins). Cross-value links
(#143): a kind predicate may carry several `value` positions — a LINK
predicate — and a modifier binds extra `value` parameters through it, so
Rage selects over attacks and lands on the linked *damage* roll
(`attack(V, melee, weapon) & dmg_of(V, W) & raging(A) => W(A) = prior + 2`);
selection is a build-time join, existential over the linked parameters, one
expansion per head member, and derived links come free because links are
ordinary kinds. Non-commuting modifiers (#144): any `prior` shape is
admitted — resistance *halves* (`prior / 2`), penalties subtract — with
#94's per-member check demanding total ordering, which one #145 blanket
usually supplies; only the prior-less override stays out. The probe now
states Rage, Bless, Luck, the thrown dagger, brutality, and resistance
PHB-faithfully, with no approximations left.

**Deferred, stated**: kind-indexed value families, and the module system
(single-file program union is the shipped degenerate case).

### 5.13 Regaining totality (decided)

The bar is Elm's: **a program that compiles does not fail at runtime** —
here, a clean compile plus a typed host never observes a `world_step` error.
The engine was close enough to this bar to claim it deliberately: the logic
layer is tri-valued (undecidedness is a *verdict*, never an in-band datum),
division is total by decided semantics (`x/0 = 0`, floored — the precedent:
totalize where a sensible answer exists), and the compiler already refused
programs it could not evaluate soundly (nested test guards, primed-judgment
reads, the kind stratum's build-time sweep, §5.8's oscillators). What
remained was a finite, enumerable list of `world_step` `-1` paths, each of
exactly one of two kinds, with one principled answer each:

1. **an authoring error a stronger compiler can catch** — moved to a located
   compile diagnostic;
2. **a host-protocol error a typed boundary can make unconstructible** —
   moved behind the §6.3 interface artifact (M2).

The runtime checks stay in the code as *assertions* (defense in depth
against compiler bugs) but leave the language contract. The loud-failures
rule is strengthened, not weakened: the loudest possible failure is the one
at compile time. And totality does **not** retire tri-valuedness — an
UNDECIDED verdict on a *judgment* is an honest answer ("the question does
not apply") and stays; totality is the guarantee that the **two-valued
consumers** — arithmetic, the step commit, the grounder — are never handed
one.

**The inventory, and where each row landed.**

| runtime failure | kind | static answer |
|---|---|---|
| contested boolean / multi-valued fluent | authoring | **conflictable pairs (#98, ERROR severity since #160)**: complementary-effect writers that could co-fire refuse to compile unless their conditions provably exclude it or a #159 `exclusive` group covers the collision |
| conflicting merge-less `:=` on one numeric | authoring | same pass, numeric side — including a writer colliding with *itself* when a scope variable is missing from the target and the value varies with the binding |
| arithmetic over a partial derived value | authoring | **the static safety rule (#116)**: an arithmetic read of a partial value is a located error unless the same rule's condition also reads it |
| solver UNDECIDED from cyclic rule graphs | authoring | **the §5.2 cycle rule (#109)**: unattacked support-SCCs complete to REFUTED via the Datalog fixpoint; attacked cycles are a located error |
| unknown action atom (#119's loud no-op) | host protocol | the §6.3 artifact's generated action constructors (M2): a bound host cannot spell an unknown atom |
| co-submission of protocol-exclusive actions | host protocol | **`exclusive` groups (#159)**: the declared, checked form of "the host never co-submits these" — rejected at step entry today, unconstructible through the §6.3 binding (M2) |
| action dead under the current `split` value (#121) | host protocol | per-value action liveness in the artifact + the `can_act` judgment idiom (M2): a bound host *asks* instead of tripping |
| internal tripwires (undeclared effect heads, …) | compiler bug | already unreachable from a clean compile by the vocabulary checks; kept as assertions |

**Partiality is inferred (#116).** A derived value with zero unconditional,
`prior`-free base definitions is *partial* — no keyword. The decision was
argued both ways (`split` set a make-the-weight-visible precedent for an
explicit marker); inferred won because the safety rule is the loudness
backstop: a forgotten base changes the value's contract, and the first
unguarded arithmetic consumer errors at compile, which is where the author
is actually looking. The machinery: `defined v(X)` is a first-class body
atom — the disjunction of the value's *prior-free* layer markers, ordinary
defeasible rules sharing a head, grounded at every partial read site so
hosts and `why?` can always ask it; a guard over an undefined value asserts
**neither fact** (an *open literal* in the family — a fact-less located
literal would otherwise close to REFUTED, since −∂ is vacuous with no
rules); `prior` over nothing *propagates* undefinedness (Bless on a save
that does not exist — still does not exist) rather than trapping. The
safety rule's soundness is syntactic, no entailment needed: an absent value
makes the guarding condition UNDECIDED, the rule cannot fire, and the RHS
never evaluates. False positives (definedness implied indirectly) cost one
self-documenting conjunct — the Elm trade, the same one #98's exclusivity
conditions ask for below. On the M2 boundary a partial value reads as the
option pair `(defined, value)`, never an exception.

**The conflictable-pair contract (#98, hardened by #160).** Two severities,
principled: **step-effect pairs are errors** — a contested step has no
meaning to ship (the `-1` is per-step, with no principled recovery), and
with #159 every safe construction has a checkable spelling, so the flip
costs no expressiveness. **Judgment pairs stay warnings** — a contested
judgment is defined, sometimes intended semantics (REFUTED both polarities
is ambiguity blocking's answer, pinned by the unresolved-conflict golden),
and its hazard, the silent null, is answered by visibility. Resolved and
covered pairs stay quiet in both cases: the clean state must be
*reachable*, or the contract below is vacuous. What fires is exactly what
can go wrong: step-effect pairs (complementary booleans, different
multi-valued values, merge-less `:=`) whose conditions do not provably
exclude co-firing and no `exclusive` group covers, judgment pairs nothing
orders (no covering `>` — a *teammate's* edge counts, team defeat's static
shadow; band edges count, having desugared to `>`), and strict-vs-strict
conflicts, which superiority can never order. Exclusivity is judged under
the collision's unifier — complementary literal, distinct multi-valued
constants, disjoint comparison intervals, disjoint membership lists — and
is conservative in the direction the diagnostic needs: exclusivity the
compiler cannot see fires, and one more condition (or one `exclusive`
declaration) is the fix — Elm's "handle the case you know is impossible". The *report* on resolved pairs — band edge / explicit `>` /
which team member beats which attacker — is tooling surface (§6.1 hover),
not a diagnostic. One reading note the warnings rely on: under ambiguity
blocking a contested literal reads **REFUTED on both polarities** — an
applicable unbeaten attacker refutes; contested is not undecided.

**Declared exclusivity (#159).** "The host never co-submits these" is not a
defense — it is a safety argument living in unverifiable host code. The
`exclusive` declaration moves it into the language:
`exclusive east(G: actor), west(G: actor)` names a group whose named
variables form its *key* (`_` positions never constrain), and a step admits
at most one member instance per key tuple — checked at `world_step` entry,
pre-solve, state untouched. Taxonomically this *reclassifies* the covered
conflictable pairs from authoring to host-protocol: the #98 pass treats a
pair whose collision forces key equality as exclusive (a collision that
leaves a key free — an arity-0 fluent — is deliberately not covered and
still refuses to compile), and the §6.3 binding retires the runtime check for bound
hosts by refusing to construct a violating action set. The construct obeys
this section's own rule — a new `-1` path may enter the language only with
its static-or-typed-boundary answer attached, and this one arrives with the
M2 row above. Self-exclusivity (`exclusive strike(A, _)`: one strike per
attacker per step, whoever the target) covers the self-collision shape the
same way; an uncovered *binder* variable stays warned — it collides within
one submitted action, which no protocol can forbid.

**The cycle rule's gate (#109; semantics in §5.2).** The completion runs
only in cyclic SCCs no rule attacks, and only for entities where every
*out-of-SCC* input the SCC reads is decided — a tied attack elsewhere, or a
#116 open guard, keeps the honest stall. Loop-starvation and conflict never
blur; that gate is what lets #116's tri-valuedness and #109's completion
coexist in one solve.

**The contract.** A story that **compiles** cannot take the contested or
undefined step paths (#160 made the step-side pairs errors, so the
zero-warning rider is gone from this half), and — with #109 — cannot be
handed a solver UNDECIDED by a two-valued consumer. Zero warnings still
buys the judgment-side guarantee (no silently-REFUTED nulls nothing
orders). The remaining `-1`s are host-protocol — an unknown or split-dead
action atom, an `exclusive`-group violation — and the M2 typed binding
retires those for bound hosts: *a compile plus a bound host never observes
`world_step -1`*. Honest boundaries, stated: a host that bypasses
the binding with raw atoms keeps today's loud `-1`s (deliberately — the
check is the protocol); provider callbacks are a trust boundary (host-
answered facts are inputs, not proofs) and sit outside the claim; and the
claim is per-story, carried by the author's zero-warning obligation, not a
global theorem about the language.

*Status*: the compile-side rows are landed and pinned (`test_partial`,
`test_conflict`, `test_cycle`, `test_excl`; #116/#98/#109/#159/#160 — the
step-side severity flip swept every example and test story clean, and
fixed a real latent bug in reaction5e's open-vs-skip window on the way);
the host-protocol rows wait on the §6.3 artifact (M2), and the contract's
full sentence should be re-stated there when the binding ships.

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

**The interface artifact is the primary client surface, and it compiles to
a typed host binding.** Host code is a client too, and the intern table
gives every host the silent failure mode the orphan pass exists to kill:
`world_query(w, lit(intern(syms, "can_atack_goblin")))` interns a fresh,
always-false atom. Codegen closes it, protobuf-style — typed atom/action
constants and arity-typed helpers (`q_can_parley(view, who)`,
`do_unlock(acts, who)`) — so renaming a fluent in `.story` breaks the host
build instead of silently never firing. A combat loop (initiative,
targeting UI, NPC turns) is then ordinary host code driven by the outer
engine, with full vocabulary checking.

**The shipped host is JavaScript over the WASM core (§12), so the binding
is plain ES-module JS, typed buildlessly.** The generated artifact is a
`.js` wrapper over the WASM exports that runs as-is; its types ship as
JSDoc annotations (or a `.d.ts` sidecar) that `tsc --checkJs` and any
editor consume with **no build step** — so a rename in `.story` breaks the
author's typecheck while the shipped module still runs untouched. This is
the §12 buildless rule applied to the host layer: TypeScript-the-language
would reintroduce the build step §12 forbids, so the type-checking is a
dev-time view over the same JS that ships, never a compile of a separate
source. There is **no generated C header** (decided 2026-07-21): the only C clients
are the golden tests and the eventual native player (§13), and they build
against `world.h` + the intern table by hand — the hand-built test worlds
already *are* that client, and §4.2's second-client test pins the boundary
without needing codegen. Vocabulary-checking is a property of the interface
artifact, not of any one language projection; the typed JS binding is the first
(and, for now, only) backend, and any future front end (§2) or additional
host language targets the same artifact and gets no privileges the binding
lacks. The binding is a **dev-time view** (§12): typed authoring convenience,
never required to run or remix — the shipped, authoritative host form is
plain ES modules against `world_*`.

**The binding's totality boundary (decided 2026-07-30 — §5.13's last
rung, stated now so the artifact is not retrofitted later).** The binding
is where the host-protocol `-1`s die, and the interface decision that
makes them die *correctly* is: **action sets are assembled through an
incremental builder, never passed as raw arrays.** The builder is the
protocol checks, moved to construction time and made attributable:

- generated **action constructors** — an unknown or misspelled atom is
  unconstructible (retires #119's row);
- **per-value liveness** and the `can_act` idiom — an action dead under
  the current `split` value is refused at `add` (retires #121's row);
- **#159 `exclusive`-group enforcement per `add`** — a violating order
  fails *at insertion*, naming the group and the conflicting order already
  in the set. This is the load-bearing choice: "unconstructible" must not
  be a static type claim alone, because the aggregated-untrusted-input
  host — a networked simultaneous-turn game collecting orders from remote
  clients — receives violating orders with **no local bug**, and needs
  per-order, per-source rejection (drop that client's order, keep the
  tick), never a whole-set failure and never a crash a remote player can
  trigger.

With the builder consuming the whole protocol class, the host-side panic
policy becomes mechanical: an `internal:` error always asserts; an
authoring error is unreachable from a compiled story (#160); a protocol
error is consumed at construction — so **a `world_step -1` reaching a
bound host is unconditionally assert-worthy**, completing the runtime
check's demotion to defense in depth. The engine itself never aborts,
deliberately: it is a library (the rule-editor feeds it half-broken live
sessions; a WASM `abort()` kills the page), the reject is atomic (no
soundness pressure), and replay has a legitimate non-bug path to the
protocol `-1` — an action log replayed against a story whose `exclusive`
groups have since changed must surface as *save incompatibility*, a
detectable, reportable condition, not a crash.

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
tests/     golden semantic tests (ctest)
examples/  .story surface-language files
```

There is **no renderer tier in the C tree**: presentation is a JS + Canvas2D
web client over a WASM build of `core`/`logic`/`state` (§12), so nothing native
draws. The C source builds native only for tests, benchmarks, and the optional
preservation-time native player (§13).

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

### 8.1 Mean, tail, and who decides

The bullets above say *what* each tier costs. This says *who chooses* when a
subsystem trades average cost for worst-case cost — and the answer is the game
programmer, not the engine. Optimizing the mean where you have slack and the
tail where you have a deadline is the ordinary discipline of systems
engineering; most engines fail it not by choosing wrong but by *hiding* the
choice — a garbage collector, a query planner, an allocator each picks
mean-vs-tail for you and won't say. In a game the frame budget is a hard wall
and different subsystems have opposite needs — per-frame movement/collision
must be flat-tail (one spike blows the 16 ms wall); an off-critical-path AI
judgment can run low-mean/high-tail. The engine cannot know which is which:
that knowledge is the frame budget and the critical path, both the
programmer's. So the tradeoff is made *local, explicit, and visible*.

- **Two classes of decision; only one gets a lever.** *Semantic invariants* —
  the defeasible semantics itself, determinism (I4), no-write-back (I1),
  actions-only-mutation (I2), and the extensional equivalence of all evaluation
  strategies — the engine fixes these, with no author lever, because one would
  permit incorrectness and silently break the lockstep/replay guarantees the
  netcode stands on (§12). *Operational policy* — eager-vs-matcher grounding
  (§5.2), full-recompute-vs-demand-cone (§4.1), lane-vs-scalar (§5.8),
  pinned-vs-adaptive plans, budget/ceiling — the programmer controls these,
  because the right answer is a function of *their* frame budget and variance
  tolerance in *this* subsystem, which the engine structurally cannot have.

- **Equivalence is the license.** A performance lever is safe to hand over only
  if turning it cannot produce a wrong answer. Every evaluation backing —
  scalar N=1, columnar lanes, eager grounding, the tick-time matcher — is
  pinned extensionally identical by the differential golden tests (same
  verdicts, byte-identical why-traces; §5.4, `test_col` / `test_drivers_agree`).
  So an operational lever selects among provably-equal-answer strategies: **a
  performance choice can never become a correctness bug.** The multi-backing
  architecture is not only how the engine goes fast — it is what makes the cost
  surface author-facing *without risk*. The equivalence discipline, sold
  internally as "so the M3 algorithm swap stays safe," earns its real keep here.

- **The same axis recurs at every layer,** controllable at the granularity it
  lives at: *grounding* — materialize (flat nᵏ) vs matcher (∝ matches), per
  rule; *state density* — every instance of a fluent declared up front vs
  declared on touch, per fluent (below); *recompute scope* — scene-tier full
  recompute vs demand-cone wake-up
  (M4), per scope/fluent; *family evaluation* — lanes (pay the full width each
  tick) vs scalar sparse wake, per family; *plan stability* — pinned vs adaptive
  re-plan, per rule. Scenes (§5.5/§6.4) scope the policy to the state it governs
  — materialize the threat rules *inside* the combat scene where density is high
  and the frame is tight, match them in the overworld where they are sparse.

- **Hints advise; overrides direct.** A *hint* is a claim about the data
  ("usually sparse") that feeds the measured cost model — wrong costs a
  suboptimal plan, nothing worse. An *override* is a claim about the
  *performance policy required* ("materialize always; do not trade my flat tail
  for a spiky mean") that removes the decision — wrong means the author eats
  exactly the cost they asked for. They are distinct in kind: a hint the router
  may overrule cannot deliver the guarantee an override exists to make. Keep
  both. Overrides move a *soft routing threshold*; they never silently defeat
  the *hard absolute ceiling* (the §5.2 cardinality cap, a memory backstop),
  which an override may only raise with an explicit stated budget — preserving
  loud-failures-no-silent-caps.

- **The flat path has a *provable* tail.** Because entities are declared up
  front (§5.2), the eager/materialize cost is a compile-time constant — the
  cross product of sort sizes, paid every tick, density-independent. That is a
  *guaranteed* worst case, not a profiled p99: the compiler can state a rule's
  materialized cost before the game runs, and re-checks it when a sort grows (so
  an override cannot silently rot into an out-of-budget blow-up — it trips a
  build error instead). Choosing the flat path deliberately requires that
  number, so the tooling obligation follows (§9): surface, per rule/family, the
  static worst case (free, from declared domains) *and* the measured per-tick
  distribution. Exposing a lever without its cost only relocates the guessing.

- **State density is a policy, never a consequence of size (decided).** A
  plain boolean fluent's ground universe is **sparse by default** — instances
  exist when touched (by an init, a rule, an action, or a host write), and
  everything else answers closed-world through the schema hook. **Dense is the
  override**: an author who needs the flat tail asks for it, and gets the
  guarantee this section is about — every slot allocated before the first
  tick, so no declaration, no atom-map growth and no family rebuild can land
  inside a frame. Both directions are principled, and which is the default is
  decided by what each costs when the author is wrong.

  Sparse-by-default, because dense is not merely N^k of *store*: the step
  theory generates inertia **per declared fluent**, two rules each, so a dense
  binary fluent is 2N² rules re-solved every tick. That is the term the
  grounding analysis found dominant — closed-world negative inertia over n-ary
  fluents was 79% of a measured 5e-shaped ground set, and eliding it cut a
  1000-actor world from 541k ground rules to 43k. An author who never thinks
  about density should not be paying that.

  Dense-as-override rather than dense-on-overflow, because a predicate that
  silently changes its tail behaviour when someone adds the 1025th entity is
  the *opposite* of a provable worst case. Spilling into a different cost
  model at a size threshold would make density an accident of content, and
  content is exactly what a mod changes. The cardinality ceiling stays what
  §5.2 makes it — a memory backstop, and for rules a routing threshold — not a
  silent switch between tail guarantees.

  Two kinds are dense **by nature, not by policy**, and carry no lever:
  *multi-valued* fluents (an effect setting one value negates every sibling,
  so all sibling atoms must exist) and *numeric* fluents (value-store slots,
  which are O(instances) because that is what the author declared). The lever
  exists only where the choice is real.

- **Default auto; the lever is a scalpel.** The measured cost model carries the
  common case. Overrides are for the parts the author has reasoned about, where
  they know something about policy — variance tolerance, critical-path
  membership, rollback re-sim budget — that no measurement conveys. Mandatory
  hand-tuning would recreate the DBA-does-everything world the provider
  mechanism (§5.2, "the logic consumes the broadphase, it is never the
  broadphase") already refuses: the provider is itself the first author-supplied
  physical-design decision; these levers are the second.

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
   golden tests (Yale shooting, cellar, torch ramification, conflict). (No
   native renderer: the interactive cellar demo lands as the Canvas2D web shell
   in M2 — the earlier native raylib demo was dropped when Canvas2D became the
   single renderer, 2026-07-21.)
2. **M1 — language front half**: lexer + recursive-descent parser for
   declarations/rules/actions; semantic checks; fluent syntax implements
   §5.7–5.8 (domains, threshold harvesting, effect operators, guard
   stratification); `cellar.story` compiles and replaces the hand-built test
   worlds; provenance carried on every generated construct, rendered by
   `dl_why` in source terms (§6.3); interface artifact emitted (§6.3; the
   typed JS host binding is an M2 concern, once there is a JS host).
3. **M2 — client contract + host API**: define the public client contract —
   the `world_*` API plus the externalized-state pattern (§4.2) — and make
   the generated typed JS binding (§6.3) the way games are actually written against
   it from the JS host: typed atom/action constants, arity-typed query and
   action helpers over the WASM exports, a rename in `.story` breaking the
   host typecheck. **Queries carry a scope**
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
   a projection of the delta, never a parsed `why?` string. Playable cellar in the
   browser (WASM core + Canvas2D) driven entirely by host code (JS) against the
   generated bindings. A trivial
   second client in tests pins the no-private-APIs claim (§4.2) the way
   golden tests pin semantics — with no reference client, this test is the
   *only* thing keeping the client boundary honest, so it is a hard
   deliverable, not a nice-to-have.
4. **M3 — engine hardening**: linear evaluation behind the same API — the
   Antoniou transformation + a stratifying/SCC-condensing compiler, then an
   SCC-ordered ("weak-topological") sweep as the evaluator, not Maher's
   counter/worklist route (see §5.2); tick-time join matcher for variables/typed
   entities (until then: ground rules per entity by hand/codegen).
5. **M4 — scale spine**: global tier (subscriptions, dependency cones —
   wake-ups recompute only the reachable ∩ demanded set, per §4.1; the demand
   cone is the attack-closed backward reachability from subscriptions and
   step-rule bodies over the same static graph), scene partitions, **nested
   scopes with lifetime/visibility (§5.5)**, serialization, hot reload.
6. **M5 — proof-of-thesis demo**: one region, ~20 NPCs, a 5e-ish combat slice
   where conditions/feats interact through superiority, one multi-step quest,
   `why?` in the UI — all driven by JS host code against the generated typed
   JS binding (§6.3). The quest is the interesting half: a multi-step quest with no
   narrative layer is the honest test of whether rules alone carry story
   state.

**The customer is a D&D 5e game** (decided 2026-07-20; `examples/combat5e.story`
and `srd_probe*.story` are its seed). Customer in the strict sense: the game
whose needs *settle design arguments*. Where a tradeoff is genuinely contested,
resolve it in 5e's favour; where 5e does not need a feature, that is evidence
against building it. It earns the role because it is the engine's origin case
and close to an ideal showcase — advantage/disadvantage, conditions,
resistances, immunities and exceptions-to-exceptions are exactly what a
superiority relation is for, and rules arguments at the table are `why?` demand
in its natural habitat.

Distinguish that from the three supporting roles, which have **no design
authority**: the RTS vertical slice is the **performance canary** (it may
falsify — "this is too slow", "this corrupts" — but it may not request
features), the fighting game is the **flex** (§12), a post-M3 stress test
that must never shape the kernel, and the SMAC-like 4X is the **showcase**
(below).

**The showcase is a Sid-Meier's-Alpha-Centauri-like 4X** (decided
2026-07-21). Its job is to *demonstrate* the killer feature to an audience,
not to settle design arguments — so like the canary and the flex it has **no
design authority** (the 5e customer still wins contested tradeoffs). It earns
the role because a 4X's feel is deeply-stacked, famously opaque modifier
stacks — morale × terrain × facilities × faction × social-engineering all
silently compounding — which is `why?` demand at its most acute: the trace
(§5.1, §9) turns the genre's signature frustration ("why did I *lose* that
combat?") into the flagship feature. It is turn-based, so it exercises the
same conditional-step semantics the 5e customer does (§12's update → resolve
→ draw) rather than the canary's real-time perf regime, and its complexity
lives in **rules the engine generates and explains, not authored prose** — so
one person can finish it without the writing-team cost a CRPG showcase would
carry. It ships on the frozen web stack (Canvas2D renderer + WASM engine
coprocessor + JS host loop, all decided 2026-07-21, §12); the 5e *remix
platform* remains the product vision and runs alongside it.

**Peer-engine context.** `infeasible` is one of three peers (see
`../stiff/DESIGN.md` §11 and `../cfl/DESIGN.md` §11). Their customers form a
strictly increasing chain through the dependency lattice — D&D 5e uses
infeasible alone; Contra adds `stiff`; Scorched Earth adds `cfl` on top of
both. Each game introduces exactly one engine, so a regression localizes to the
engine just added. This is the first link in that chain and therefore the one
with no cross-engine concerns at all: **build no cross-engine glue here.**

## 12. Distribution: web target and content artifacts

The shipped product is a browser platform for a remix community: many authors
making scenarios, subclasses, spells, items, and total conversions on a shared
5e chassis, with nothing to install. This is a distribution and packaging
concern layered on the kernel (§4.2); it adds no engine semantics.

**The web target.** The logic core compiles to WASM as a library. Presentation
is a swappable client (§4.2): a single **hand-written Canvas2D renderer** for
both development and the shipped product, behind the frozen PICO-8-sized
presentation interface (below). No native (raylib) backend and no WebGL (Pixi)
backend — Canvas2D alone, which the arithmetic below shows is ample at the
StarCraft-1 ceiling (decided 2026-07-21). The core re-solves per
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

**The durability line.** The target artifact property is the 90s one: a
shipped game that still runs and is still remixable in ten years. What
actually survived that era — Doom WADs, Z-machine story files, the SCUMM
catalog — survived as *data plus a small re-implementable interpreter*:
every original runtime died, and communities rewrote the interpreter
against the data format (source ports, Frotz, ScummVM). The kernel already
has this shape: content is declarative data through a vetted interpreter
(above), the save is `(engine-hash, game-hash, action-log)`, and I4 replay
is the Doom-demo property — a playthrough reproduces exactly on any
conforming engine. What rots on the web, ranked worst-first:

1. **Anything fetched at runtime** — CDNs, servers, load-time packages —
   dies in years. The one-container artifact already forbids it: vendor
   everything, no exceptions.
2. **Build toolchains** — the remixability killer; a ten-year-old
   dependency tree does not install. Discipline: **nothing in the artifact
   requires a build step**. Content is already buildless (in-browser
   analysis, above); host code's shipped, authoritative form is plain ES
   modules that run as-is. Typed generated bindings (§6.3) and any
   transpiled authoring layer are dev-time *views*, never required to run
   or remix — the source-authoritative rule applied to the host layer.
3. **Browser API deprecation** — low; the web's compat promise is the
   strongest in computing. Canvas2D is effectively frozen; WebGL is
   low-but-nonzero (the WebGPU transition); WASM is a small spec'd ISA
   with multiple independent implementations, and the engine's C source
   ships regardless, so the interpreter is rebuildable — the ScummVM
   escape hatch, held open on purpose.
4. **Library abandonment** — smallest, and commonly misranked first. A
   vendored library is inert; it cannot rot on its own, only break when a
   platform API shifts beneath it (item 3). Abandonment prices the
   eventual port, nothing else. (Pixi v8 dropping its Canvas2D backend is
   the in-genre exhibit: a backend is the upstream's to remove, and what
   you vendored is what you keep.)

The line itself: **below** it, durable by construction — the WASM engine
(plus its C source), `.story` source and the interface artifact with their
versioned serializations, the save format, buildless host JS against
`world_*`, and the presentation interface (next). **Above** it,
acknowledged mortal and cheaply replaceable — renderer implementations,
DOM chrome, the platform site. The rule: the mortal layer reaches the
durable one only through spec'd surfaces, so a platform break is a port of
one thin implementation, never per-game surgery. Both disciplines are
cheap now and unretrofittable in year eight.

**The presentation interface: PICO-8-sized, and frozen.** §4.2 makes
presentation a swappable client; this fixes the *size* of the swap
surface. PICO-8's lesson is that its utility comes not from rendering
power but from a tiny frozen API (`spr`, `map`, `print`, `pal`, `rect` —
and that is the whole world). The counterpart here: a fixed internal
resolution chosen **per game in the manifest** from the blessed
1080-divisor set — 320×180, 480×270, 640×360, 960×540 — so every choice
integer-scales cleanly to 1080p and 4K forever. 640×360 is the reference
default: SC1's density in 16:9, ×3 to 1080p (and roughly SC1's own map
viewport — its console ate the bottom quarter of 480; widescreen UIs
corner-cluster instead of spanning). Everything else is frozen
engine-wide: nearest-neighbor integer upscale with letterboxing (a wider
window must never reveal more map — under lockstep that is a fairness
rule, not taste), and roughly a dozen ops — atlas tile blit, sprite draw
(flip + alpha), text, primitives, one composite op for fog/vision —
frozen the way the kernel's two ports are frozen. Genre breadth costs
the durability line nothing here: resolution is a per-game *choice
within* the frozen contract, never a widening of it. UI text
renders at internal resolution like everything else, or the aesthetic
splits into game pixels and suspiciously sharp text. The
graphics ceiling is **StarCraft 1** (decided): 640×480-class palettized
sprites at RTS scale. Nothing in the surface may assume more.

- **The reference renderer is hand-written Canvas2D** — a few hundred
  dependency-free lines, below the line. The arithmetic is ample even at
  the ceiling: SC1 is a tile layer plus a few hundred animated sprites per
  frame, software-rendered on 1998 CPUs; GPU-backed Canvas2D has orders of
  magnitude more headroom. The sprite layer repaints per frame (movement
  interpolation is presentation state); the tile layer pre-renders to an
  offscreen canvas and repaints only when a subscription delta touches it
  — for the static world, the delta stream is the repaint schedule. SC1's
  signature effects need no shaders: player colors are pre-baked recolored
  atlas variants (an asset-pipeline product, not a renderer op — the op
  set stays small), cloaking is the alpha op, fog of war is the composite
  op over dithered overlay tiles. What Canvas2D cannot do (per-pixel
  lighting, shaders, thousands of blended particles) is above the ceiling
  by decision: the constraint and the art direction agree.
- **Canvas2D is the only renderer** (decided 2026-07-21) — no WebGL/Pixi
  backend and no native raylib backend. Pixi would be a *vendored dependency
  above* the durability line bought to gain performance the arithmetic here
  says we do not need at the SC1 ceiling; a native renderer would be a second
  platform to keep alive. One dependency-free renderer below the line is both
  the more durable choice and the smaller surface to freeze.
- **What keeps the interface honest without a second renderer.** The old
  argument was that two live renderers prove the interface is small (the role
  M2's trivial second client plays for `world_*`, §4.2). With one renderer that
  proof falls to two other things: the op-set is frozen small *by construction*
  (a dozen ops), and the eventual optional **native player** (§13) — a
  preservation/offline runtime implementing those same ops over the same cart —
  is the second backend that demonstrates portability if and when it is built.
  Day-one honesty is the discipline of the frozen op-set, and the
  weekend-rewritability of the Canvas2D renderer is still the evidence that the
  op-set is small enough to port.
- **Learnability is the same property.** A PICO-8-sized draw model is
  learnable in an afternoon, which is what the remix community needs from
  presentation — the utility target and the durability target are one
  decision.
- **Input, audio and persistence are frozen here too, and each is frozen
  for a determinism reason rather than an aesthetic one.** Freezing the draw
  ops alone does not make a cart portable: the moment content reaches for
  `addEventListener`, Web Audio or `localStorage`, the optional native player
  (§13) stops implementing a dozen ops and starts reimplementing a browser,
  and the weekend-sized claim above is gone. All three sit *inside* the
  presentation interface, below the durability line, and nothing outside it is
  reachable from a cart.
  - **Input is polled, never delivered as events.** `pointer()` in internal
    resolution coordinates, `button(i)`/`pressed(i)` over three pointer
    buttons, `key(k)`/`keyp(k)` over a frozen named key set. The polling shape
    is PICO-8's, adopted for I4 and not for nostalgia: the save is an action
    log, so a callback firing between ticks lets a cart branch on input the
    replay never observes. Input is sampled once per tick at the tick
    boundary, and the only path from input into the world is *becoming an
    action* — nothing reads input during a solve. Coordinates arrive already
    mapped back through the letterbox and the integer upscale, because that
    inverse belongs below the line: every cart computing it independently is
    every cart getting the letterbox edges wrong. Keys are a frozen named set
    and never raw codes — browser `KeyboardEvent.code` and native scancodes
    disagree, and a raw code is the platform leaking into the cart.
  - **Audio is write-only.** `sound(id, gain)`, `stop(id)`, `music(id, gain)`,
    `music_stop()`. A cart may start and stop sound; it may never ask whether
    something is playing, how far in it is, or whether it finished. Each of
    those readbacks is wall-clock in disguise, and a rule that branches on one
    is nondeterministic by construction (I4). Audio is a projection of the
    delta and emit streams exactly as the renderer is.
  - **Persistence is not storage.** The save is `(engine-hash, game-hash,
    action-log)` and the *platform* owns it; a cart gets no general key-value
    store. State written outside the action log is state replay cannot
    reproduce, which forfeits shareable playthroughs, branching and time
    travel in one move — the three properties the action-log save exists to
    buy. What a cart does get is PICO-8's `cartdata` shape: one small
    fixed-size numeric blob for cross-run **non-game** state (settings,
    cosmetic unlocks), explicitly outside the save and explicitly not
    readable by rules. A fluent may never be initialised from it, or a
    world's history stops being a function of its log.

  Twenty-odd ops across all four surfaces — still an afternoon to learn, still
  a weekend to port, and now the whole cart-facing platform rather than only
  its drawing half.

**The freeze is a list, and the list is checked.** The op names, the blessed
resolution set, the sixteen-entry palette, the frozen key set and the text cell
are one module (`web/platform/spec.mjs`), and every backend and every cart
reads them from there. What makes "frozen" a property of the code rather than a
promise in a document is the *correspondence check*: `web/platform_check.mjs`
asserts that the cart-facing surfaces expose exactly those names and that each
backend implements exactly those names, in both directions. A thirteenth draw
op cannot be added to a renderer and quietly depended on by a cart — it fails
until it is added to the spec deliberately, and then every backend must grow
it. That is the same discipline the golden tests apply to semantics, pointed at
a surface instead of a meaning.

Two consequences of the shape are worth stating because they are easy to
mis-implement. The text cell is frozen as **metrics, not glyphs** — `print`
advances a fixed cell width per character on every backend, and the letter
shapes are the backend's business — so an offline player may substitute its own
font without any cart's UI shifting by a pixel. And the letterbox *inverse*
(display point → internal pixel) lives below the line beside the letterbox
itself: every cart computing it independently is every cart getting the edges
wrong.

There are **two** frozen text cells, and the arithmetic rather than taste is
why. A game picks its internal resolution from the blessed set, but the cell is
in internal pixels, so at 640×360 a 4×6 cell puts 160 columns on screen and a
capital is 1.4% of screen height — against PICO-8's 3.9%. That is a terminal,
not a game UI. The reflex is to grow the cell, but the dense one earns its keep
precisely where columns matter: a `why?` trace reads unwrapped at 160 columns
and wraps at 100. One size is wrong for one of the two jobs, so `print` takes a
size the way `spr` takes flip and alpha — a second cell on an existing op,
never a thirteenth op. A backend implements two bitmap fonts, which is still a
weekend to port.

**Burst cues: the one output of a step that is not state.** Those four
surfaces are what a cart *calls*; what a step *hands back* is the other half of
the interface, and it is three streams — the fluent delta (§11 M2), the §5.8
commit receipts, and the **emission buffer**. Presentation cues split in two,
and only one half needs a construct. A *persistent* cue — an aura while a
condition holds, a burning-ground loop — is a function of a fluent being set,
so the client subscribes to that fluent's delta and starts/stops the loop on
the transition; nothing new is required. A *burst* cue — a hit spark, a
floating "Resisted!", a death cry — is over before the next tick and has no
fluent to watch. Reifying one as state would mean a fact set to fire the
renderer and immediately retracted, which is exactly the write-back I1 exists
to forbid.

So a burst cue is declared `emit spark(actor)` and fired from a `causes`
clause like any effect, and the step reads it off the same solve the next state
comes from. An emission is the **transient twin of an action**: an action is a
transient input to a step, an emission a transient output of one; neither is
ever a fact. The consequences are the whole contract:

- *Write-only.* No rule body, guard, `requires` or `init` may name a cue —
  a located compile error. Reading one back would make it state (I1) and would
  give the renderer's channel a path into the logic.
- *Positive only.* `~spark(X)` is an error. A cue fires when its rule fires, so
  a suppression condition belongs in the rule's body — which is where
  defeasibility already lives, since that body reads defeated judgments like
  any other. Emissions therefore carry no defeat structure of their own.
- *A proposition, not a count.* Two rules firing one cue in a step emit it
  once; distinguish sources by parameterizing the atom. This is what makes the
  stream a set of atom ids rather than an event log with multiplicity.
- *Deterministic (I4).* The stream is a pure function of (state, actions), in
  declaration order, so a replay reproduces it exactly — a requirement for
  lockstep, rollback and the replay debugger, not a nicety.
- *Flat, one crossing.* `world_emits` is a per-tick buffer of ground atom ids
  the client reads through a typed-array view over WASM memory, exactly like
  the delta — per step, never per atom.

A cue is momentary, so its *condition* must be too: a ramification gated on
`~alive'` alone fires every step thereafter, while one gated on
`alive & ~alive'` fires on the transition. That is an authoring property of
edges vs. levels, not a construct — the same distinction `btnp` draws against
`btn` on the input side.

**The infeasible cart: presentation as a projection of the log.** The three
streams above are not merely *sufficient* for a client — they are the complete
input to one, and that makes a stronger target reachable: a cart written
entirely in `.story`, with no host code at all. The engine's loop reads the
streams and drives video and audio from what they describe. Host JS becomes the
escape hatch rather than the default, which is the right polarity for three
separate reasons — a cart with no host code cannot rot, the offline player
(§13) can run it without embedding a JS engine, and the remix on-ramp becomes
"edit rules" instead of "edit rules and also learn the host API".

The architecture that makes it factorable is the signalling split, and it is
worth stating as a table because getting it wrong is the classic game-audio
bug — an aura loop that restarts every tick, or a hit sound implemented as a
loop:

| stream | answers | drives |
| --- | --- | --- |
| emission buffer | *what happened* | one-shots |
| subscription edges | *what changed* | loops starting and stopping, tweens beginning |
| changeset + receipts | *by how much* | damage numbers, bar animation, easing targets |

**Levels drive loops; edges drive one-shots** — `btn` against `btnp` again,
and the engine hands a client both separately so the two cannot be conflated by
accident. The third row is why a cue carries only *who* and *what* and never a
magnitude: the number already exists in the commit receipt, with provenance a
cue could not carry. A floating damage number is a **join** across two streams
in one tick, not a second copy of a number that can drift from the first.

Two constraints on any table built over this, both consequences of decisions
made elsewhere. Emissions are a **set per tick**, so two hits in one tick are
one cue unless the atom is parameterized — an author who wants two sounds
distinguishes them in the vocabulary, because counting is not available (I4).
And audio is **write-only**, so a loop's lifecycle rides its level's
enter/exit edges; "is it already playing" is not a question the interface
answers, and a table that wanted to ask it is mis-factored.

**Animation is CSS's factoring, and it belongs in the loop.** The reason
motion does not need to be in the database is that the useful part of a
transition model is not declarativeness but the factoring: state is discrete,
interpolation is a pure function of (previous state, next state, elapsed wall
time), and nothing flows back. I4 forbids the wall clock in the *logic core*;
a renderer reading it to produce pixels can never touch a fact. So the platform
records, per subscribed literal, *when* it last changed, and the cart eases its
own values between two positions it computed itself — the database says
`at(hero)` flipped, never where the sprite is mid-flight. Two details are worth
stealing whole: a transition interrupted mid-flight must restart from the
current *interpolated* value or fast flips snap, and layout changes want
FLIP — compute the new layout, then play the difference. One way the analogy
runs in our favour: a browser's intermediate state is unobservable and
unrecoverable, whereas tick states here are exact, so scrubbing a replay lands
on a known state and the tween simply restarts. Declarative animation and time
travel do not fight, which they would if motion were stored.

**Layers, because call order was doing three jobs at once.** In imperative
drawing the sequence of calls silently provides z-order, repaint granularity,
and transform scope. Derive the scene from a set of facts and all three are
gone, so a declarative renderer names them: which layer is on top, which layer
pre-renders and repaints only when a delta touches it (§12's tile-vs-sprite
split, already assumed), and which layer is in world space rather than screen
space (`camera` and `clip` stop being order-dependent ops and become layer
attributes). Layers are an ordered list of **names**, not numeric z — an open
numeric space invites `z: 999` and arbitrary gaps, and the ethos everywhere
else here is a small enumerated set. Order *within* a layer is a determinism
problem rather than a taste one: it must be total and stable, tie-broken by
declaration order exactly as the emission stream is, or two peers render
differently and the second-client differential (§4.2) quietly stops meaning
anything.

**Three things stay outside the database, each for a different reason.**
*Per-viewer state* — selection, camera, which panel is open — is local, and a
fluent that records it desyncs lockstep the moment two players look at
different things; §5.5's private-vocabulary scopes are its home, which makes
per-viewer presentation an M4 dependency of this whole direction. *Layout
arithmetic* is expressible and miserable: "the i-th offered command" is ordinal
reasoning over a dynamic set, which costs pairwise rules to author and is the
Haskell-of-defeasible-logic failure the surface exists to avoid — so layout is
a blessed built-in (list, grid, anchor) that assigns positions to a set, not a
thing authors derive. *Sub-tick motion* has no representation because the tick
is the unit of truth; it lives in the loop, per the tween clock above.

What is *not* an obstacle is worth recording, because the intuition runs the
wrong way. Positions are numeric fluents — store slots with an expression VM,
not ground atoms — so a coordinate costs nothing to represent; what would
explode is grounding a relation over coordinate pairs, which is precisely why
§5.6 makes space a provider. And cost is not the constraint either: at
columnar rates, a few hundred draw *decisions* re-derived per tick is noise.
The database is good at deciding and bad at arithmetic, and the factoring above
is that sentence made structural.

**What the engine must grow for this to be reachable**: a presentation
ontology (blessed vocabulary for sprite, layer, sound, so a renderer knows what
a conclusion means), the layout built-ins above, and the per-viewer scopes.
One constraint falls out of the engine having no string type, and it is more
interesting than it looks: **the atom is the label**. `rusty_key` renders as
"RUSTY KEY" by a renderer-side convention, so UI copy is vocabulary — which
keeps a single source of truth and costs punctuation, sentences, and
per-language text, pushing localization to a table keyed by atom.

**Capabilities are gated by engine version, not by cart-carried code.** The
alternative — a cart shipping its own compiled module, cartridge-with-a-chip —
is declined. It solves a problem we do not have: cartridge silicon existed
because the console was already in millions of homes and could not change,
whereas the engine here travels with the artifact or is served by a platform
that updates. Versioning also collapses four mechanisms into one that already
exists, since the save is already keyed on an engine hash: no chip registry, no
content-hash identity, no import-list audit, no redistributability rule, and no
combinatorial support matrix — a runtime at version N supports everything up to
N, a total order rather than a feature set.

Two axes must not be conflated inside that, though. **Capability version** is
monotone and additive, and a cart declares a minimum. **Semantics version**
must be pinned exactly: if a release changes how a defeasible cycle resolves or
how a clamp rounds, an older cart must still evaluate under the older
behaviour, or every saved action log for it silently diverges. Only the second
belongs in the save's engine hash, and the cost of this design — stated plainly
because it is permanent — is that every evaluation behaviour ever shipped is
shipped forever. The golden-test discipline already pushes hard against
semantic churn; this is the reason it must.

**The test has been run, and it passes.** `examples/cellar_pure.story` is the
cellar with its presentation, played end to end — frame, labels, placement,
sprites, fog, menu, refusals, cues — with **zero game code**. The renderer
(`web/platform/scene.mjs`) knows the blessed vocabulary and nothing else; the
driver (`web/platform/purecart.mjs`) contains no game at all. Together they are
248 lines of generic JS standing in for 318 lines of game-specific cart, and
pointing them at a different `.story` draws a different game with no edit. The
price on the authoring side is 43 presentation rules, 24 enum members of
vocabulary, and 14 rows of geometry in `init`. Nothing in the compiler had to
change: the ontology is ordinary judgments over enum constants, and coordinates
are ordinary numeric fluents.

**The menu is the engine's answer, not the story's.** The first version of this
cost a `cmd` enum mirroring the actions and twenty rules restating each
action's `requires` as an `offers`/`blocked` judgment — the rule written twice,
free to drift from the original, and reconstructing something the solver had
already computed and thrown away. So the engine answers it: `world_actions`
enumerates the ground action atoms, `world_action_status_of` says whether one
applies right now, and `world_action_blockers` returns the unsatisfied
conditions of the rule that came CLOSEST to firing — ordinary literals, so
`world_why` on one prints the argument that refused it. That deleted the
vocabulary and the twenty rules outright, and it is worth every client, not
only a pure one: any UI that greys a button was writing that mirror judgment.
The status distinguishes *blocked* from *unknown*, which is the difference
between a refusal and a typo, and from *speculative* — a rule whose remaining
conditions are about the next state, which cannot be decided without taking the
step and so is reported rather than silently judged.

**A second game says which of that was universal.** `examples/duel_pure.story`
is a card duel drawn by the same renderer with no edit between them — chosen to
be as far from the cellar as a game can be and still be drawn at all: no space,
so things live in zones rather than rooms; you act ON a target, so a command
needs a subject *and* an object; numbers are the game rather than scenery; and
the menu is a hand that changes every turn. `web/skins_check.mjs` measures the
overlap rather than asserting it.

Nine of the fourteen blessed predicates were needed by both — `panel`,
`caption`, `shows`/`prop_shows`, `in_anchor`/`prop_in`, `gauge`, `picked`,
`cue_sound`, and the geometry table. That is the shared core, and it is most of
the vocabulary rather than a coincidence. Five turned out to be one game's
furniture: `held`, `shaded`, `here` and `cue_word` are the cellar's, `aimed` is
the duel's.

Three things the *renderer* believed turned out to be the first game's shape,
and all three were invisible until the second:

- **Filtering a menu by "the term mentions the subject"** worked only because
  every cellar action carried its actor as an argument. A duel's does not —
  `strike(edge_a, gnoll)` never names who is striking — and that filter hid the
  entire game. The rule that serves both is about SORT: hide a row naming
  something of the subject's own kind that is neither the subject nor the
  object.
- **An action with no arguments was unreachable**, because a filter keyed on
  the subject can never match one. `end_turn` is nobody's and everybody's.
- **Subject and object are two selections, not one.** The cellar needed only
  the first, so one game could not have told us.

And three things *neither* could say, which is the more useful list because an
item demanded by two independent games is no longer a matter of taste:

1. **A number that is not the blessed one** — *closed*. The engine had derived
   BOOLEANS (judgments) and stored NUMBERS (fluents, never derived — I1), and
   no derived number a client could read: a value's chain is inlined into
   whatever reads it, so a value nothing in the world reads had nowhere to be
   read from — which is exactly the presentation case, since a bar's width is
   computed for a client and for nobody else. `world_get_value` registers each
   ground instance and evaluates it against current state, so the arithmetic
   stays in the story. A gauge no longer knows `hp`: it reads `gauge_value` and
   `gauge_max`, which the story defines, and its colour is an ordinary
   judgment. Naming a fluent *as an argument* is still open and is the general
   form (#124's `value` meta-sort is the mechanism).
2. **A label on a THING rather than on a region.** `caption` takes an anchor;
   cards and fighters have no anchor of their own, so they cannot be named.
3. **"Select one, clear the rest."** Both games paid one concrete action per
   entity for it — `pick_hero`/`pick_guard`, `aim_gnoll`/`aim_imp`/`aim_you` —
   because a parameterized action cannot say "the others". Until §5.5's scopes
   exist this is also why selection is world state at all, and the renderer
   routes clicks by a `pick_`/`aim_` NAMING CONVENTION that should die with it.

Five frictions remain from the first experiment, and they are the shape of the
work left (a sixth — an
action's extra parameters needing recovery by convention — went away with the
menu, since the engine names a GROUND action and the term carries its own
arguments):

1. *A rule needs a body*, so the parts of a frame that are simply always there
   need something to hang on. A `showing` fluent is the honest answer and turns
   out to be the seam a title screen would use anyway.
2. *Values are functions, not lookup tables* — **closed**. A head argument may
   now be a constant, and then the definition is a ROW speaking for that ground
   instance alone (`sx(sh_bar) = 8`), with a catch-all as the default beneath
   the rows. #94's "exactly one unconditional base" becomes one *per instance*:
   rows never collide with each other, and a table with no catch-all is partial
   (#116) exactly where no row speaks. Without this a value is only ever a
   function — one formula for every instance — which suits `damage(W)` and is
   useless for per-shape geometry, where each shape's numbers are its own and no
   formula relates them.
3. *A predicate is monomorphic in its argument sorts*, first rule wins,
   silently — so `shows(actor, …)` and `shows(item, …)` cannot be one
   predicate and the ontology duplicates per sort. This is the ontology's
   loudest argument for **sort union**, since "everything drawable" is a cover,
   not a coincidence.
4. *A judgment can carry an entity but not a quantity*, so a bar's source has to
   be a fluent blessed **by name** — the one world word the renderer knows.
   Naming a fluent as an argument is what would close it.
5. *Per-viewer state has no home*, exactly as predicted. Selection is a fluent,
   so it sits in the shared world and in the action log, and "select one, clear
   the rest" needs one concrete action per actor because a parameterized action
   cannot say it. §5.5's scopes remain the answer, and this is the concrete bill
   for not having them yet.

None of the five is a reason to keep host code as the default. The residue after
the experiment is exactly what the factoring predicted it would be — the loop,
the layout arithmetic inside the renderer, and the **assets**, which are pixels
and are no more expressible as rules than a `.png` is.

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

**Rollback is a third recomposition of the same primitives — and the
fighting game is the stress test.** A fighting game looks continuous but
is the most discrete of genres: named states, frame-data tables,
fixed-point positions, one input per player per 60Hz tick — inside the
thesis boundary (discrete facts changed by discrete actions), unlike the
physics platformer, whose truth is per-frame continuous simulation.
GGPO-style rollback is snapshot-at-confirmed-frame, restore on a late
remote input, deterministic resimulation — exactly the late-join
primitives above, composed by a different driver; fighting-game replays
have always been input logs, which is to say this save format. And hit
arbitration is defeasible logic natively: "the hit lands — unless
blocking — unless a throw, which beats block — unless airborne — unless
armor — unless an armor-breaker" is a stacked superiority diamond that
shipping games resolve with hand-ordered if-chains, and `why?` in
training mode ("your invincibility ended frame 5; the hit landed frame
6") is a feature the genre has never had. This is a *post-M3* stress
test, not a design target: 60 solves/sec with rollback resimulating ~8
frames inside one means ~1ms worst-case solves and snapshot/restore on
the hot path — maximal exercise of exactly what the other genres
exercise least (§8's worst-case budget; I4 as netcode) — while most of
the content lives in providers (per-frame hitbox tables are asset data,
like tilemaps; the rules arbitrate interactions). The kernel must not
bend toward it. It is the flex, not the customer.

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
  the generated typed JS binding can type them differently. "What feels instant vs.
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

**Coprocessors, carts, and the client surface (provisional sketch, 2026-07-21).**
A consolidation of a design session, recorded provisionally; it refines the
artifact model above and the client model of §4.2 without changing kernel
semantics. Names are working names.

- *The coprocessor reading.* The three peers (§11) are native, optimized
  **coprocessors** — fixed "silicon" the platform embeds; a **cart** is *data*:
  `.story` world source (§6) compiled under frozen semantics, plus buildless
  host glue, assets, and a manifest. The durability line above already argues
  this shape (data + a small re-implementable interpreter); the coprocessor
  framing just names the interpreters and makes explicit that the RTS/family
  work (§4.1) is one coprocessor reaching N-entity scale *inside itself*, while
  cross-coprocessor composition stays the deferred, host-owned concern (§11:
  build no cross-engine glue here). infeasible-alone (the D&D 5e customer) is a
  complete single-coprocessor console; a multi-coprocessor console is a later,
  conscious commitment.
- *Save is a per-coprocessor dump the host frames.* Extends the save model above
  to the multi-engine case, honoring the dependency direction (no OS below
  `app/`): each coprocessor serializes *only its own* mutable state to a
  caller-provided buffer — for infeasible that is exactly the base facts (I1/I2
  make derived state non-state; load restores facts and re-solves, never
  trusting a stored judgment). The dump is **name-keyed** (interned ids are
  unstable across builds) and carries a **schema fingerprint** (validated
  against the cart vocabulary, §6.3); `world_dump`/`world_restore` touch a
  buffer, never a file. The *host* owns the envelope — `{cart id+version,
  per-coprocessor blobs, optional action log, meta}` — and all disk/compression/
  sync. Two shapes, both live: the per-coprocessor **snapshot** (decoupled load,
  no replay) and the host-level **action log** (exact I4 replay, undo,
  time-travel, reproducible bug reports; replay is cross-coprocessor, hence
  host-owned). The host dumps every coprocessor at a **tick boundary** (after
  resolve, before the next update) so the combined save is consistent.
- *The client reads reactive tables, not queries.* Fleshes out the
  `world_subscribe` channel (§11 M2) into an author-facing surface. Judgments
  are exposed to host code as **reactive tables** the engine keeps current: a
  **level** table (`judgment[entity]`, current verdict) and **edge** tables
  (`rose`/`fell`, entities whose verdict changed this tick — the `btnp` to
  level's `btn`). The loop **decides from these cached tables, never by
  re-querying** — safe because state is constant within a tick (I2); `query`/
  `why` demote to one-shot reads and diagnostics. Across the WASM/JS split the
  engine cannot poke a JS object in place, so a judgment table is a **typed-array
  view over WASM linear memory**: the engine solves and writes its own memory, JS
  reads through a zero-copy view (genuinely in place — one buffer), and edges
  arrive as a compact delta buffer the engine writes and JS reads once, crossing
  the boundary **once per step**, not per entry (the `world_subscribe` delta
  seam). Internals stay packed bitvectors for the solve; exposed columns unpack
  to byte columns for cheap reads (`memory.grow` detaches views, so JS rebuilds
  them after a grow). The frame is
  **update → resolve → draw**: read tables and submit actions in update, resolve
  (a step) yields the next tick, draw renders it — so the player sees the
  consequence the same frame, and the displayed tick is exactly what the next
  update decides against. Turn-based games step **conditionally** (no action →
  no step → same tick); real-time steps every frame. Tables are read-only views;
  the only write path is an action — command/query separation made physical.
- *Authoring language ≠ loop language.* The world is authored in `.story` (§6),
  a language built for rules; the loop is plain buildless host code. Deliberately
  **not** a fluent DSL embedded in the host language: an internal DSL borrows
  host syntax but not host semantics (a comparison that is not a boolean, a
  "rule" that declares rather than runs), misleading exactly the junior/remix
  audience the durability+learnability target serves. Keeping authoring in
  `.story` also keeps the host surface small — the same size argument as the
  presentation interface above. The host language is **JS/ES modules** (decided
  2026-07-21 with browser-primary, below); the session's PICO-8/Lua framing was
  inspiration for the *shape* (small API, buildless, learnable), which is
  language-neutral and ported unchanged. JS is not self-limiting the way Lua
  was, so the minimalism is imposed by a deliberately small host API, not the
  language.
- *Non-goal: speculative solve as a decision mechanism.* A "what-if future"
  query (run a step without committing, to decide from a predicted state) is
  rejected as a core primitive: it invites authors to move behavior out of rules
  (declarative, inspectable, `why?`-able) into procedural lookahead, undercutting
  the thesis. Behavior that reacts to a situation is a rule. Legitimate residues:
  the **present counterfactual** (evaluate current judgments with a fact patched
  — the §5.3 dry-run, no time step) and **exact undo via the action log**.
  Genuine adversarial lookahead is out-of-scope host-side planning on a minimal
  pure-step primitive (the narrative-front-end rule: substrate, not kernel), not
  needed by the customer.
- *Runtime and distribution (decided 2026-07-21).* **Browser-primary.** The
  platform is a website — zero-friction discovery, instant play, in-place fork,
  share-by-URL — which is what a remix community (§12) runs on; the web is also
  the strongest durability substrate (§12). A **cart is a self-contained,
  downloadable, forkable artifact** (the §12 one-container HTML), so the
  retro-collectible identity survives *inside* the browser model with no install.
  The engine ships as **WASM with SIMD128** (Baseline in every major browser
  since Safari 16.4, 2023); the solver's integer `v128.and/or/xor` are
  deterministic across engines, so browser SIMD never threatens I4 — WASM-SIMD,
  scalar, and native-AVX builds compute bit-identical results (pinned by the
  golden tests and the `test_col` differential fuzz oracle). The core speed is
  SWAR (64 entities per `i64` word), fully preserved; SIMD128 recovers 128-bit
  vectorization; only the *wider* native AVX-256/512 is browser-unavailable — a
  bounded 2–4× on the vectorizable loops that matters only at large N. A **native
  player is an optional later runtime over the same cart format** (the
  ScummVM/source-port shape §12 blesses), where AVX returns for the RTS ceiling,
  preservation, or offline. Same cart, two runtimes, two perf envelopes; results
  identical by construction.
- *Presentation is a frozen op-set, not Canvas.* Carts draw against the
  **PICO-8-sized frozen presentation interface** (§12: ~a dozen ops — atlas/tile
  blit, sprite with flip+alpha, text, primitives, one composite for fog/vision),
  **never raw Canvas2D**. Canvas2D is the reference (and only shipped) *backend*;
  the eventual optional native player (below) would be a second backend with
  its own renderer — not raylib, and not a second shipped web renderer. A native app therefore does **not** reimplement the Canvas API — it
  implements the dozen ops, which §12 already banks on being weekend-sized (the
  smallness *is* the durability proof). Corollary of the JS host: the native
  player must **embed a JS engine** (e.g. QuickJS) to run cart glue — the mirror
  of "a Lua cart would need a Lua VM in the browser"; deferred with the native
  player, cheap for an embeddable engine.

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
- **Team defeat**: **decided — keep it** (2026-07-28; a removal was drafted on
  a since-dropped branch and reconsidered). The distinguishing case is the
  criss-cross — supporters `r1, r2`, attackers `s1, s2`, `r1>s1`, `r2>s2`, no
  single champion — and team defeat decides it the way layered content needs:
  independent reasons, each carrying its own trump over its own exception,
  compose *across authors* — `r > s` keeps its local meaning ("my rule
  overrides that exception") no matter what else exists. Single-champion
  defeat does not remove the global-knowledge problem, it relocates it: a
  support is impotent unless it dominates *every* attacker, so pairwise `>`
  edges silently stop meaning what they say, and the failure class it
  manufactures is the *silently-REFUTED* conclusion — exactly §6.2 Tier-1
  #2's non-monotonic null deref. Two language facts seal it: band-derived
  superiority is uniform across teams, so the two semantics coincide wherever
  bands resolve the conflict; and automatic specificity (below) *organically
  generates* criss-crosses — two specific supports each trumping its own
  general exception — which must compose, not refuse. The original objection
  (an emergent team win is unpredictable without global knowledge) is
  epistemic and is answered by visibility, not semantics: the `why?` trace
  already renders which team member beat which attacker, and the
  conflictable-pair compile check has landed (#98, §5.13) — unordered
  complementary pairs warn at build time (a teammate's `>` edge counts as
  team defeat's static shadow); the per-pair *resolution report* for
  resolved pairs (band edge / `>` / which member beats which attacker) is
  the remaining tooling surface (§6.1 hover). Pinned by the criss-cross golden in `test_dl`
  (verdict-level, not just the dl↔dl_col differential). Still needs
  author-facing docs.
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
- **The presentation ontology** (§12's infeasible cart): the vocabulary that
  experiment used is a working existence proof and probably the wrong
  primitives. Two things are suspect. It declares the DRAWING rather than the
  world — `panel(anchor, style)` is a widget constructor with `rule` in front
  of it, and most such rules carry no inference at all, which is the sign that
  nothing about them wanted to be a rule. And the vocabulary is PER-GAME: an
  `anchor`/`word`/`style` set re-declared by every story is a callback
  interface with extra steps, not a language. The shape that would fix both is
  a world model with no presentation in it and a separate declarative *skin*
  quantifying over relations that are actually universal — kind → appearance,
  containment → layout, state → modifier — with layout stated relationally
  (`inside`, `below`, `stack`) rather than as absolute anchors, since the
  database is good at relations and bad at arithmetic. But one game cannot
  say what is universal: the intersection of a rooms-and-menu game, a tactics
  grid and a card game is what deserves blessing, and only the first exists.
  Undecided with it: where a transition's *declaration* lives. A cart-side table keeps presentation
  above the durability line; an inert annotation in `.story`, riding the §6.3
  artifact and never read by a rule, makes a cart's look travel with the world
  so a second client animates it identically. Decide with the layout built-ins,
  since both answer "what does a renderer read that is not a fact".
- **Sound and music assets** (§12): the audio *ops* are frozen, but the
  format a cart ships its sounds in — and whether a cart may synthesize
  rather than sample — is open. Sampled assets are one more thing an
  offline player must decode; a tiny synth spec is more to freeze but
  decodes to nothing. Decide with the asset pipeline.
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
  Defeasible Reasoning Systems*, IJAIT 10(4), 2001. (Delores and Deimos:
  the forward-chaining counter/occurrence-list engine and the query-driven
  memoized engine — the field's own instance of the sweep-vs-demand fork
  that §4.1 and §8 navigate)
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
  IJCAI 2003, pp. 1079–1084; earlier as *Additive Fluents*, AAAI Spring
  Symposium 2001. (concurrent numeric effects that combine — and the source of
  §5.8's two accumulator kinds. Also the independent confirmation that a law
  over a *total* of concurrent contributions must read that total in the
  resulting state: `caused false if departed(G,L) eq M after num(G,L) eq N &&
  M > N`, where the auxiliary `departed` is additive-default-zero and the `if`
  part is evaluated after the contributions land)
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
- F. Bancilhon, D. Maier, Y. Sagiv, J. Ullman, *Magic Sets and Other Strange
  Ways to Implement Logic Programs*, PODS 1986; C. Beeri, R. Ramakrishnan,
  *On the Power of Magic*, J. Logic Programming 10, 1991; K. T. Tekle,
  Y. A. Liu, *More Efficient Datalog Queries: Subsumptive Tabling Beats
  Magic Sets*, SIGMOD 2011. (demand as program transformation: bottom-up
  evaluation restricted to the query's cone — §4.1's demand cone in
  database clothing. Its two hard-won lessons transfer: extending demand
  through negation was unsound until done carefully — cf. D. Kemp,
  D. Srivastava, P. Stuckey on well-founded bottom-up evaluation, TCS 146,
  1995 — which in defeasible terms is why the cone must close over
  attackers and superiority competitors; and the transformation is not
  always a win, the bookkeeping-vs-sweep trade §8 resolves per family)
- W. Chen, D. S. Warren, *Tabled Evaluation with Delaying for General Logic
  Programs*, JACM 43(1), 1996. (SLG resolution / XSB: the top-down dual —
  compute what the query demands, memoize subgoals, terminate. Deimos
  (IJAIT 2001, above) is this strategy inside defeasible logic; what the
  line lacks is standing demand — subscriptions — and incremental
  maintenance between queries)
- C. Lefèvre, P. Nicolas, *A First Order Forward Chaining Approach for
  Answer Set Computing*, LPNMR 2009 (ASPeRiX); A. Weinzierl, *Blending Lazy
  Grounding and CDNL Search for Stable-Model Solving*, LPNMR 2017 (Alpha).
  (lazy grounding: rule instances created only when their bodies become
  relevant, against ASP's ground-everything-first bottleneck — the
  literature that keeps judgments *ungrounded*, not merely unevaluated,
  outside the demanded set)
- M. Hammer, K. Phang, M. Hicks, J. Foster, *Adapton: Composable,
  Demand-Driven Incremental Computation*, PLDI 2014; A. Mokhov,
  N. Mitchell, S. Peyton Jones, *Build Systems à la Carte*, ICFP 2018.
  (dirty ∩ demanded as a formal calculus, and the design-space taxonomy —
  eager vs. suspending, dirty-bit vs. trace — that locates §4.1's wake-up
  rule and §8's choices as points in a mapped space)
- F. McSherry, D. Murray, R. Isaacs, M. Isard, *Differential Dataflow*,
  CIDR 2013. (subscription-maintained incremental views: deltas propagate
  only through demanded dataflows — the systems-world shape of
  `world_subscribe`'s delta stream, §4.1/§12)
- H. Beck, M. Dao-Tran, T. Eiter, *LARS: A Logic-based Framework for
  Analytic Reasoning over Streams*, AIJ 261, 2018. (standing continuous
  queries over a changing fact base with nonmonotonic semantics, evaluated
  incrementally — the community that treats subscriptions + nonmonotonicity
  as one problem, though ASP-flavored rather than defeasible)
- J. Whaley, M. Lam, *Cloning-Based Context-Sensitive Pointer Alias
  Analysis Using Binary Decision Diagrams*, PLDI 2004 (bddbddb); H. Jordan,
  B. Scholz, P. Subotić, *Soufflé: On Synthesis of Program Analyzers*,
  CAV 2016. (set-at-a-time Datalog engineering: relations as bulk data
  structures, evaluation as whole-relation operations — the lineage §5.8's
  columnar backing extends to defeasible proof statuses; see §3 for why the
  defeasible implementation line never met it)
- Larian's Osiris: DOS2/BG3 modding documentation (community wiki).
- E. Ruskin, *AI-driven Dynamic Dialog through Fuzzy Pattern Matching*,
  GDC 2012. (Left 4 Dead response rules: most-specific-match dialogue
  selection — "specific beats general" as ad-hoc scoring; §4.2's
  `pending_scene` gets the same behavior from superiority, with traces)
- inkle, *ink* — https://github.com/inkle/ink (surface model for a narrative
  layer should one be built as a client (§2); its growth of variables,
  functions, and arithmetic is the cautionary half — computation belongs in
  rules or providers, not a dialogue layer)
- Lexaloffle, *PICO-8* — https://www.pico-8.com (fantasy console: its
  utility comes from a tiny frozen API, not rendering power — the size
  model for §12's presentation interface)
- ScummVM — https://www.scummvm.org; Infocom's Z-machine and its
  interpreters (Frotz); the Doom source-port lineage. (what game
  preservation actually looks like: data plus a small re-implementable
  interpreter outlives every original runtime — §12's durability line)
