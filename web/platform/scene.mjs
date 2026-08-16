// scene.mjs — a renderer that has never heard of the game it is drawing.
//
// DESIGN.md §12's "infeasible cart": a cart written entirely in `.story`, with
// no host code, drawn by a generic loop that reads what the world concluded.
// This is that loop. It knows the BLESSED VOCABULARY below and nothing else —
// no cellar, no door, no torch — so pointing it at a different story draws a
// different game with no edit here.
//
//   ax/ay/aw/ah(anchor)      geometry, as derived VALUES (not stored facts)
//   panel(anchor, style)     a box
//   caption(anchor, word)    text; the ATOM IS THE LABEL (`w_the_cellar`)
//   shows(drawable, sprite)  anything's sprite — ONE predicate over a declared
//                            cover (`sort drawable union actor, item`, #231),
//                            not one per sort with the renderer taking the
//                            union at read time
//   in_anchor(actor, anchor) actors packed into a region
//   prop_in(item, anchor)    props packed into a region
//   held(item, actor)        ...or carried beside a holder
//   shaded(anchor)           the composite op over a region
//   gauge(anchor, E)         a bar; the STORY supplies gauge_value/gauge_max
//                            (derived values) and gauge_low (a judgment)
//   picked(E)                the SUBJECT — whose menu this is
//   aimed(E)                 the OBJECT — what a command is aimed at
//   surfaced(judgment)       a guard worth showing a refused command for
//   cue_sound(cue, sound)    an emission plays a sound
//   cue_word(cue, word)      ...and floats a word
//
// Two conventions carry meaning that would otherwise need syntax. **Enum order
// is meaning**: a `sprite` member's position is its atlas index, and a `cmd`
// member's position is its place in the menu — declaration order is already the
// engine's tie-break for emissions (I4), so inheriting it means two clients
// cannot disagree about what is drawn on top or listed first. And **the atom is
// the label**: `w_the_cellar` prints as "THE CELLAR", which is what a world
// with no string type buys — one source of truth for UI copy, at the cost of
// punctuation and of any second language.
//
// The scene is rebuilt once per TICK, not per frame. Enumerating a predicate
// means crossing its argument domains and asking, which is a few hundred
// queries — noise at a tick, waste at 60fps. Frames only replay the model.

const BIG = { big: true };

/** `w_the_cellar` -> "THE CELLAR". Strips the vocabulary prefix an author uses
 *  to keep enum members from colliding, then reads the atom as English. */
const say = (atom) => String(atom).replace(/^[a-z]_/, '').replace(/_/g, ' ').toUpperCase();

/** `take_torch(hero,hall)` -> `take_torch`; the label a menu row shows. */
const verb = (term) => term.replace(/\(.*/, '');
/** A ground term's arguments — they are in its name, which is the spelling the
 *  §6.3 artifact publishes. */
const args = (term) =>
  (term.includes('(') ? term.slice(term.indexOf('(') + 1, -1).split(',') : []);

/** Cross an argument list's domains into every ground tuple. */
function tuples(domains) {
  return domains.reduce((acc, d) => acc.flatMap((t) => d.map((v) => [...t, v])), [[]]);
}

/**
 * @param {object} w      the generated binding session
 * @param {object} iface  its IFACE export (predicate arities and sorts)
 * @param {object} doms   { ...SORTS, ...ENUMS } — every argument domain by name
 */
export function createScene(w, iface, doms) {
  const arity = new Map(iface.judgments.map((j) => [j.name, j.args]));
  const actionParams = new Map(iface.actions.map((a) => [a.name, a.params]));

  /** Every proved ground instance of a judgment, as argument tuples. */
  const proved = (name) => {
    const args = arity.get(name);
    if (!args || !w.q[name]) return [];
    return tuples(args.map((s) => doms[s] ?? []))
      .filter((t) => w.q[name](...t) === 'proved');
  };
  const isProved = (name, ...t) => !!w.q[name] && w.q[name](...t) === 'proved';

  const spriteIndex = (s) => (doms.sprite ?? []).indexOf(s);

  /** Which declared domain a ground argument belongs to. Sorts hold entities;
   *  enums hold values. Telling them apart is what lets a menu reason about
   *  "something of the subject's own kind" without knowing any kind's name. */
  /* entity -> its BASE sort. A declared cover (#231) is an argument domain and
   * so appears in `doms`, listing its members' entities again under its own
   * name; a flat inversion would answer "drawable" for a fighter and the kin
   * test below would stop telling a card from a person. The artifact marks a
   * cover so this can skip it — the sort of a thing is where it was declared,
   * never a set that merely admits it. */
  const covers = new Set(Object.keys(iface.unions ?? {}));
  const sortMap = new Map(Object.entries(doms)
    .filter(([k]) => !covers.has(k))
    .flatMap(([k, v]) => v.map((e) => [e, k])));
  const sortOf = (e) => sortMap.get(e) ?? null;
  const isEntity = (a) => sortMap.has(a);

  /** What clicking a thing does.
   *
   *  Selection is per-viewer state, and per-viewer state has no home until
   *  §5.5's scopes exist — so for now it lives in the shared world, which means
   *  it has to be an ACTION. `pick_<entity>` sets the subject and
   *  `aim_<entity>` sets the object; both games arrived at that shape
   *  independently, for the same reason ("select one, clear the rest" is not
   *  something a parameterized action can say).
   *
   *  So this is a NAMING CONVENTION standing in for a language feature, and it
   *  should die when scopes arrive. It is written out rather than inferred
   *  because inferring it — "any applicable action with one entity argument" —
   *  swallowed `go_hall(hero)` and hid half the cellar's menu. A convention
   *  that is wrong is worse than one that is merely temporary. */
  const CLICK = ['pick_', 'aim_'];
  const pickTerm = (e) => {
    for (const item of w.menu()) {
      if (item.status !== 'applies' || args(item.term).length) continue;
      if (CLICK.some((p) => verb(item.term) === p + e)) return item.term;
    }
    return null;
  };

  /** An anchor's geometry, read as DERIVED VALUES rather than stored facts:
   *  a layout is constants, and constants in the fact store are configuration
   *  carried in every save. */
  const box = (a) => ({ x: w.value.ax(a) ?? 0, y: w.value.ay(a) ?? 0,
                        w: w.value.aw(a) ?? 0, h: w.value.ah(a) ?? 0 });

  /** The scene model: what a frame draws, derived once per tick. */
  let model = null;

  function rebuild() {
    const anchors = doms.anchor ?? [];
    const geom = Object.fromEntries(anchors.map((a) => [a, box(a)]));

    // occupants of a region, in declaration order — the layout built-in the
    // story is spared: "which slot" is ordinal reasoning, and asking rules to
    // do it is the failure mode §12 names.
    const slots = {};
    const place = (list, kind) => {
      for (const [e, a] of list) (slots[a] ??= []).push({ e, kind });
    };
    place(proved('in_anchor'), 'actor');
    place(proved('prop_in'), 'prop');

    const held = proved('held');                       // [item, actor]
    const picked = proved('picked').map((t) => t[0])[0] ?? null;
    const aimed = proved('aimed').map((t) => t[0])[0] ?? null;

    // THE MENU IS THE ENGINE'S ANSWER. It used to be an `offers`/`blocked`
    // pair of judgments the story wrote beside every action — the `requires`
    // clause restated, free to drift from the original. `w.menu()` asks the
    // world instead: every ground action it has a rule for, whether it applies
    // now, and the guards that refused it. Filtered to the picked actor by the
    // one thing a client legitimately knows — the actor is in the term.
    // THE MENU IS THE ENGINE'S ANSWER. It used to be an `offers`/`blocked`
    // pair of judgments the story wrote beside every action — the `requires`
    // clause restated, free to drift from the original. `w.menu()` asks the
    // world instead: every ground action it has a rule for, whether it applies
    // now, and the guards that refused it. Filtered to the picked actor by the
    // one thing a client legitimately knows — the actor is in the term.
    //
    // A verb grounds once per binding, so `drop_torch` is three ground actions
    // for three rooms and a flat list shows DROP TORCH three times. The rule
    // that fixes it without losing anything: every APPLICABLE instance is a
    // real choice and all of them are listed (attacking two different goblins
    // is two rows), but a verb with no applicable instance contributes at most
    // ONE refused row — the reason you cannot drop the torch is not three
    // different reasons.
    // WHICH ROWS BELONG TO THIS MENU. The first version filtered by "the term
    // mentions the subject", which was the first game's shape talking: every
    // cellar action carried its actor as an argument. A duel's does not —
    // `strike(bolt_a, gnoll)` never names who is striking — so that filter hid
    // the entire game.
    //
    // The rule that serves both is about SORT, not position: hide a row that
    // names something of the SUBJECT'S OWN KIND which is neither the subject
    // nor the object. `go_hall(guard)` is the guard's business,
    // `strike(bolt_a, imp)` is aimed at someone you are not aiming at, and an
    // action naming nothing of that kind (`end_turn`) is nobody's and
    // everybody's, so it is always offered.
    const clickable = new Set([...sortMap.keys()].map(pickTerm).filter(Boolean));
    const kin = picked ? sortOf(picked) : null;
    const rows = picked
      ? w.menu().filter((item) => !clickable.has(item.term) &&
          args(item.term).filter((a) => sortOf(a) === kin)
                         .every((a) => a === picked || a === aimed))
      : [];
    const byVerb = new Map();
    for (const r of rows) {
      const g = byVerb.get(verb(r.term)) ?? { ok: [], blocked: [] };
      (r.status === 'applies' ? g.ok : g.blocked).push(r);
      byVerb.set(verb(r.term), g);
    }
    const visible = [];
    for (const g of byVerb.values()) {
      if (g.ok.length) { visible.push(...g.ok); continue; }
      const best = g.blocked.find(offerable);
      if (best) visible.push(best);
    }

    // ...and where one verb DOES leave several rows, the label has to say which
    // is which — but only by what actually DIFFERS between them. Appending
    // every argument gives "STRIKE EDGE A GNOLL" when the target is the same
    // in both, and the noise is the part a player has to read past.
    const menu = [];
    if (picked) {
      const m = geom.a_menu ?? { x: 8, y: 244, w: 176, h: 12 };
      const group = new Map();
      for (const r of visible) {
        const g = group.get(verb(r.term)) ?? [];
        g.push(r);
        group.set(verb(r.term), g);
      }
      visible.forEach((item, i) => {
        const peers = group.get(verb(item.term));
        const mine = args(item.term);
        const differs = peers.length < 2 ? []
          : mine.filter((a, k) => peers.some((p) => args(p.term)[k] !== a));
        menu.push({
          term: item.term, ok: item.ok, blockers: item.blockers ?? [],
          label: say(verb(item.term)) + (differs.length ? ' ' + differs.map(say).join(' ') : ''),
          x: m.x, y: m.y + i * (m.h + 3), w: m.w, h: m.h,
        });
      });
    }

    model = {
      geom,
      panels: proved('panel'),
      captions: proved('caption'),
      shaded: proved('shaded').map((t) => t[0]),
      gauges: proved('gauge'),
      slots, held, picked, aimed, menu,
      sprite: (e) => spriteIndex((proved('shows').find((t) => t[0] === e) ?? [])[1]),
    };
    // sprite lookup is per-entity and would re-enumerate; flatten it once.
    // ONE read: a story declares `sort drawable union actor, item` and every
    // drawable is one predicate (#231). This used to spread `shows` and
    // `prop_shows` together, which was a game's ONTOLOGY SHAPE leaking into
    // the renderer — worse than the game word §12 forbids, and it grew a term
    // per drawable sort.
    const spr = new Map([...proved('shows')].map(([e, s]) => [e, spriteIndex(s)]));
    model.sprite = (e) => (spr.has(e) ? spr.get(e) : -1);
    return model;
  }

  const anchorOf = (name) => ((doms.anchor ?? []).includes(name) ? name : null);

  // ---- drawing ---------------------------------------------------------

  const STYLE = {          // the frozen look of each declared style
    st_title:      (d, b) => { d.rectfill(b.x, b.y, b.w, b.h, 2); },
    st_room:       (d, b) => { d.rectfill(b.x, b.y, b.w, b.h, 1);
                               d.rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, 3); },
    st_bar:        (d, b) => { d.line(b.x, b.y, b.x + b.w - 1, b.y, 2); },
    st_button:     (d, b) => { d.rect(b.x, b.y, b.w, b.h, 3); },
    st_button_off: (d, b) => { d.rect(b.x, b.y, b.w, b.h, 2); },
  };

  function draw(ctx, sheet) {
    const d = ctx.draw;
    const m = model ?? rebuild();
    d.cls(0);

    for (const [a, style] of m.panels) {
      const b = m.geom[a];
      if (b && STYLE[style]) STYLE[style](d, b);
    }

    // occupants: a region packs its contents into a row of cells
    for (const [a, list] of Object.entries(m.slots)) {
      const b = m.geom[a];
      if (!b) continue;
      list.forEach((o, i) => {
        const x = b.x + 24 + (i % 4) * 40, y = b.y + 28 + Math.floor(i / 4) * 44;
        const idx = m.sprite(o.e);
        if (idx >= 0) d.spr(sheet, idx, x, y);
        if (o.e === m.picked) d.rect(x - 3, y - 3, 22, 22, 6);
        if (o.e === m.aimed && o.e !== m.picked) d.rect(x - 3, y - 3, 22, 22, 10);
        o.at = { x, y, w: 16, h: 16 };
        // whatever this occupant carries rides beside it
        const mine = m.held.filter(([, by]) => by === o.e);
        mine.forEach(([it], k) => {
          const s = m.sprite(it);
          if (s >= 0) d.spr(sheet, s, x + 8 - mine.length * 7 + k * 14, y + 20, { alpha: 0.9 });
        });
      });
    }

    for (const a of m.shaded) {
      const b = m.geom[a];
      if (!b) continue;
      for (let ty = 0; ty < b.h / 8; ty++)
        for (let tx = 0; tx < b.w / 8; tx++)
          d.shade(sheet + '_fog', 0, b.x + tx * 8, b.y + ty * 8);
    }

    for (const [a, word] of m.captions) {
      const b = m.geom[a];
      if (!b) continue;
      // a caption on a region sits above it; one on a bar sits inside it
      const inside = b.h >= 12;
      d.print(say(word), b.x + (inside ? 8 : 0), inside ? b.y + 3 : b.y - 11,
              inside ? 5 : 3, BIG);
    }

    m.gauges.forEach(([a, who], i) => {
      const b = m.geom[a];
      if (!b) return;
      // The numbers are the STORY's. `gauge_value`/`gauge_max` are derived
      // values (#82) it defines and the engine evaluates, so nothing here knows
      // what a bar is measuring — and the colour is a judgment, not a threshold
      // frozen in this file.
      const v = w.value?.gauge_value?.(who) ?? 0;
      const max = w.value?.gauge_max?.(who) || 1;
      const low = isProved('gauge_low', who);
      const y = b.y + i * 11;
      d.print(say(who), b.x, y, who === m.picked ? 5 : 3, BIG);
      d.rectfill(b.x + 44, y + 1, 48, 6, 2);
      d.rectfill(b.x + 44, y + 1, Math.max(0, Math.round(48 * v / max)), 6,
                 low ? 10 : 13);
      d.print(`${v}`, b.x + 98, y, 3, BIG);
    });

    for (const b of m.menu) {
      (b.ok ? STYLE.st_button : STYLE.st_button_off)(d, b);
      d.print(b.label, b.x + 4, b.y + 2, b.ok ? 5 : 2, BIG);
    }

    if (ctx.why) {
      d.rectfill(4, 18, 632, 198, 1);
      d.rect(4, 18, 632, 198, 2);
      let ly = 22;
      for (const line of ctx.why.split('\n')) {
        if (!line.trim() || ly > 210) continue;
        d.print(line.slice(0, 155), 8, ly, line.includes('-- applicable') ? 10 : 4);
        ly += 8;
      }
    }
  }

  // ---- input: a click is a command, and a command is an action ---------

  /** A blocked row is worth SHOWING when its refusal is an argument rather
   *  than an absence. One blocker means everything holds but one thing — the
   *  row is one step away — and a blocker that is a JUDGMENT means there is a
   *  `why` worth reading, where a base fact ("you are not there", "you are not
   *  holding it") is merely absent and has no trace to print. Both halves come
   *  from the artifact, so no story declares which of its guards are
   *  interesting. */
  const isJudgment = new Set(iface.judgments.map((j) => j.name));
  const predOf = (atom) => atom.replace(/[(=].*/, '');
  const offerable = (item) =>
    item.blockers?.length === 1 && isJudgment.has(predOf(item.blockers[0].atom));

  return {
    rebuild,
    draw,
    /** What the pointer is over: a menu row, or an occupant of a region. */
    /** The scene's clickable regions as FOCUS TARGETS (§12), in a stable
     *  order: menu commands first, then entities. `hit` answers the same
     *  regions positionally; this answers them navigably, which is the only
     *  form a d-pad can use. Order is the geometric-navigation tiebreak, so
     *  it is semantics rather than presentation (I4). */
    targets() {
      const m = model;
      if (!m) return [];
      const out = [];
      for (const b of m.menu)
        out.push({ id: `cmd:${b.term}`, x: b.x, y: b.y, w: b.w, h: b.h });
      for (const list of Object.values(m.slots))
        for (const o of list)
          if (o.at) out.push({ id: `ent:${o.e}`, x: o.at.x - 3, y: o.at.y - 3, w: 22, h: 22 });
      return out;
    },
    /** What a focus id stands for — the navigable twin of `hit`. */
    target(id) {
      const m = model;
      if (!m || !id) return null;
      if (id.startsWith('cmd:')) {
        const term = id.slice(4);
        const b = m.menu.find((x) => x.term === term);
        return b ? { kind: 'cmd', term: b.term, ok: b.ok, blockers: b.blockers } : null;
      }
      const e = id.slice(4);
      return { kind: 'entity', entity: e };
    },
    hit(p) {
      const m = model;
      if (!m) return null;
      for (const b of m.menu)
        if (p.x >= b.x && p.x < b.x + b.w && p.y >= b.y && p.y < b.y + b.h)
          return { kind: 'cmd', term: b.term, ok: b.ok, blockers: b.blockers };
      for (const list of Object.values(m.slots))
        for (const o of list)
          if (o.at && p.x >= o.at.x - 3 && p.x < o.at.x + 19 &&
                      p.y >= o.at.y - 3 && p.y < o.at.y + 19)
            return { kind: 'entity', entity: o.e };
      return null;
    },
    /** Every proved ground instance of a judgment, as tuples. */
    pairs: proved,
    /** What clicking an entity submits — see pickTerm. */
    pickAction: pickTerm,
    isProved,
    model: () => model,
    say,
  };
}
