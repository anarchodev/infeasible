// scene.mjs — a renderer that has never heard of the game it is drawing.
//
// DESIGN.md §12's "infeasible cart": a cart written entirely in `.story`, with
// no host code, drawn by a generic loop that reads what the world concluded.
// This is that loop. It knows the BLESSED VOCABULARY below and nothing else —
// no cellar, no door, no torch — so pointing it at a different story draws a
// different game with no edit here.
//
//   ax/ay/aw/ah(anchor)      geometry, as a numeric state table
//   panel(anchor, style)     a box
//   caption(anchor, word)    text; the ATOM IS THE LABEL (`w_the_cellar`)
//   shows(actor, sprite)     an actor's sprite
//   prop_shows(item, sprite) a prop's sprite
//   in_anchor(actor, anchor) actors packed into a region
//   prop_in(item, anchor)    props packed into a region
//   held(item, actor)        ...or carried beside a holder
//   shaded(anchor)           the composite op over a region
//   gauge(anchor, actor)     a bar, filled by the blessed hp / hp_max
//   picked(actor)            whose menu is showing
//   offers(actor, cmd)       a command, in `cmd` declaration order
//   blocked(actor, cmd)      ...offered but refused; clicking prints `why`
//   here(actor, room)        also fills a room-sorted action parameter
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
  const box = (a) => ({ x: w.state.ax(a), y: w.state.ay(a),
                        w: w.state.aw(a), h: w.state.ah(a) });

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

    const menu = [];
    if (picked) {
      const order = doms.cmd ?? [];
      const offered = new Set(proved('offers').filter((t) => t[0] === picked)
                                              .map((t) => t[1]));
      const blocked = new Set(proved('blocked').filter((t) => t[0] === picked)
                                               .map((t) => t[1]));
      const m = geom[anchorOf('a_menu')] ?? { x: 8, y: 244, w: 176, h: 12 };
      let i = 0;
      for (const c of order) {
        if (!offered.has(c)) continue;
        menu.push({ cmd: c, ok: !blocked.has(c), label: say(c),
                    x: m.x, y: m.y + i * (m.h + 3), w: m.w, h: m.h });
        i++;
      }
    }

    model = {
      geom,
      panels: proved('panel'),
      captions: proved('caption'),
      shaded: proved('shaded').map((t) => t[0]),
      gauges: proved('gauge'),
      slots, held, picked, menu,
      sprite: (e) => {
        const a = proved('shows').find((t) => t[0] === e);
        const p = proved('prop_shows').find((t) => t[0] === e);
        return spriteIndex((a ?? p ?? [])[1]);
      },
    };
    // sprite lookup is per-entity and would re-enumerate; flatten it once
    const spr = new Map([...proved('shows'), ...proved('prop_shows')]
                        .map(([e, s]) => [e, spriteIndex(s)]));
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
      // the one world word this renderer knows: a gauge reads `hp`/`hp_max`,
      // because a judgment can carry an entity but not a quantity
      const v = w.state.hp?.(who) ?? 0, max = w.state.hp_max?.(who) ?? 1;
      const y = b.y + i * 11;
      d.print(say(who), b.x, y, who === m.picked ? 5 : 3, BIG);
      d.rectfill(b.x + 44, y + 1, 48, 6, 2);
      d.rectfill(b.x + 44, y + 1, Math.max(0, Math.round(48 * v / Math.max(1, max))), 6,
                 v > max / 2 ? 13 : 10);
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

  /** Fill an action's declared parameters from the picked actor and its room. */
  function bind(cmdAtom) {
    const name = cmdAtom.replace(/^c_/, '');
    const params = actionParams.get(name);
    if (!params || !model?.picked) return null;
    const room = (proved('here').find((t) => t[0] === model.picked) ?? [])[1];
    const args = params.map((p) => (p === 'room' ? room : model.picked));
    if (args.some((a) => a === undefined)) return null;
    return w.a[name](...args);
  }

  return {
    rebuild,
    draw,
    /** What the pointer is over: a menu row, or an occupant of a region. */
    hit(p) {
      const m = model;
      if (!m) return null;
      for (const b of m.menu)
        if (p.x >= b.x && p.x < b.x + b.w && p.y >= b.y && p.y < b.y + b.h)
          return { kind: 'cmd', cmd: b.cmd, ok: b.ok };
      for (const list of Object.values(m.slots))
        for (const o of list)
          if (o.at && p.x >= o.at.x - 3 && p.x < o.at.x + 19 &&
                      p.y >= o.at.y - 3 && p.y < o.at.y + 19)
            return { kind: 'entity', entity: o.e };
      return null;
    },
    bind,
    /** Every proved ground instance of a judgment, as tuples. */
    pairs: proved,
    /** The `pick_<entity>` action, if the story declares one. */
    pick: (e) => (w.a[`pick_${e}`] ? w.a[`pick_${e}`]() : null),
    isProved,
    model: () => model,
    say,
  };
}
