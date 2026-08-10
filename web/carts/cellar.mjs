// carts/cellar.mjs — the playable cellar (DESIGN.md §11 M2).
//
// This is a whole game as the platform means it: `examples/cellar_play.story`
// for the world, this file for everything else. It touches the engine ONLY
// through the generated typed binding (§6.3) and the presentation ONLY through
// the four frozen surfaces (§12) — no atom is spelled by hand here, and no DOM
// call is reachable from here.
//
// The thing worth reading it for is how little the cart decides. It does not
// know when the door may be forced; it asks `q.can_force_door(who)`. It does
// not compute what a click changed; it reads the step's changeset. It does not
// explain a greyed-out button; it prints `why?`. Every one of those is the
// engine's answer rendered, which is the property that makes a UI here
// impossible to drift out of sync with the rules — there is no second copy of
// the rules to drift from.
//
// The puzzle, for orientation: the hero starts poisoned in the dark cellar and
// so is too weak to shoulder the vault door; the antidote is behind it. The
// key is on the cellar floor. So the hero unlocks and the *guard* — unpoisoned,
// already in the hall — shoulders it open, at the cost of 2 hp.

import { open, SORTS, STORY, SOURCE_HASH } from '../cellar_play.binding.mjs';

export { STORY, SOURCE_HASH, open };

/** The atlas: one char per pixel, a hex palette index or '.' for transparent.
 *  Eight 8×8 tiles — floor, wall, hero, guard, key, torch, flask, dither. */
export const SHEET = {
  tile: 8,
  cols: 8,
  pixels: [
    '11111111' + '22222222' + '..4444..' + '..4444..' + '........' + '...b....' + '..555...' + '15151515',
    '12111211' + '23333332' + '..4444..' + '..4444..' + '..666...' + '..bab...' + '...5....' + '51515151',
    '11111111' + '23222232' + '.eeeeee.' + '.aaaaaa.' + '.6..6...' + '..b6b...' + '...5....' + '15151515',
    '11211121' + '23222232' + 'eee44eee' + 'aaa44aaa' + '.6..6...' + '...8....' + '..ddd...' + '51515151',
    '11111111' + '23222232' + '.e4444e.' + '.a4444a.' + '..666...' + '...8....' + '.ddccd..' + '15151515',
    '12111211' + '23333332' + '..e..e..' + '..a..a..' + '...6....' + '...8....' + '.dccccd.' + '51515151',
    '11111111' + '22222222' + '..4..4..' + '..4..4..' + '...666..' + '...9....' + '.dccccd.' + '15151515',
    '11121111' + '20000002' + '..4..4..' + '..4..4..' + '...6.6..' + '........' + '..dddd..' + '51515151',
  ],
};

const T = { FLOOR: 0, WALL: 1, HERO: 2, GUARD: 3, KEY: 4, TORCH: 5, FLASK: 6, DITHER: 7 };
const ITEM_TILE = { rusty_key: T.KEY, torch: T.TORCH, antidote: T.FLASK };

const ROOMS = ['cellar', 'hall', 'vault'];
/** Room boxes in internal pixels — 12×6 floor tiles each. */
const BOX = {
  cellar: { x: 6,   y: 20, w: 96, h: 48 },
  hall:   { x: 110, y: 20, w: 96, h: 48 },
  vault:  { x: 214, y: 20, w: 96, h: 48 },
};
const DOOR = { x: 206, y: 36, w: 8, h: 16 };

const BAR_Y = 76;                      // the command bar's top edge
const WHY_X = 132;

// palette shorthands
const INK = 0, DIM = 2, MID = 3, PALE = 4, BONE = 5, GOLD = 6, RED = 10, LIME = 13, SKY = 15;

/** Where an actor or a floor item stands inside its room. */
const slot = (room, i) => ({ x: BOX[room].x + 8 + (i % 5) * 17,
                             y: BOX[room].y + 12 + Math.floor(i / 5) * 20 });

export const cart = {
  resolution: [320, 180],
  sheets: { main: SHEET },

  init(ctx) {
    this.sel = 'hero';
    this.why = '';               // the trace under the last refused button
    this.buttons = [];
    this.fx = [];                // burst cues, mid-animation: presentation state
    this.pending = null;
    this.note = '';             // what the last step's receipt/cues said
    this.event = '';            // what a subscribed conclusion did

    // The reactive channel (§11 M2). The cart watches three conclusions and
    // narrates their EDGES; it reads their LEVEL nowhere, because for levels
    // it just asks. One call shape for the multi-valued fact and for the two
    // judgments standing on it.
    const w = ctx.world;
    this.watch = [
      [w.subscribe(w.lit.door('open')),         'the vault door swings open'],
      [w.subscribe(w.lit.can_force_door('hero')), 'hero could put a shoulder to it'],
      [w.subscribe(w.lit.weakened('hero')),     'the poison takes hold of hero'],
    ];
  },

  // ---- one tick: read the frozen input snapshot, return the orders ---------
  //
  // Everything here happens at the tick boundary. Nothing in `draw` may change
  // a fact, and nothing here may read input twice.
  tick(ctx) {
    const { input, world: w } = ctx;
    this.buttons = this.offer(w, this.sel);

    if (input.pressed(0)) {
      const p = input.pointer();

      // clicking an actor selects them
      for (const who of SORTS.actor) {
        const r = this.actorRect(w, who);
        if (r && hit(p, r)) { this.sel = who; this.why = ''; this.note = ''; return null; }
      }

      // clicking a command either queues it or explains why it is greyed
      for (const b of this.buttons) {
        if (!hit(p, b)) continue;
        if (b.ok) { this.why = ''; this.note = ''; return w.actions().add(b.act); }
        this.why = b.because ? w.why(b.because) : '';
        this.note = `${b.label}: refused`;
        return null;
      }
    }
    if (input.keyp('tab')) {
      this.sel = this.sel === 'hero' ? 'guard' : 'hero';
      this.why = '';
    }
    return null;
  },

  /** After a step: turn the engine's three streams into presentation. */
  after(ctx) {
    const { audio } = ctx;
    for (const cue of ctx.step.emits) {
      const [name, arg] = parseTerm(cue);
      audio.sound(name, 0.6);
      if (name === 'heave')   this.burst(ctx, arg, 'OOF', RED);
      if (name === 'sip')     this.burst(ctx, arg, 'AAH', LIME);
      if (name === 'pickup')  this.burst(ctx, arg, 'GOT IT', GOLD);
      if (name === 'clunk')   this.event = 'the lock turns - the door is jammed';
    }
    // The numeric receipt (#88): how hp reached its value, in the author's own
    // terms. A projection of the delta, never a parsed why-string.
    for (const d of ctx.step.changed) {
      if (!d.atom.startsWith('hp(')) continue;
      const r = ctx.world.receipt(d.atom);
      // `amount` is the SIGNED delta; `op` only says which operator wrote it,
      // so re-deriving a sign from `op` prints "--2".
      const rows = (r?.items ?? []).filter((i) => !i.defeated)
        .map((i) => `${i.rule} ${i.amount >= 0 ? '+' : ''}${i.amount}`);
      this.note = `${d.atom}: ${r.base} -> ${r.applied}  (${rows.join(', ')})`;
    }
    // The edges: subscribed conclusions that moved this tick.
    for (const e of ctx.step.edges) {
      const line = this.watch.find(([h]) => h === e.sub);
      if (line && e.rose) this.event = line[1];
    }
    if (ctx.step.rejected) this.note = `step refused: ${ctx.step.rejected}`;
  },

  burst(ctx, who, text, color) {
    const room = ctx.world.state.at(who) ?? 'hall';
    const p = this.actorRect(ctx.world, who) ?? slot(room, 0);
    const b = BOX[room];
    // keep a cue inside the room it belongs to, so it reads as that room's
    const half = text.length * 2;
    const x = Math.min(b.x + b.w - half, Math.max(b.x + half, p.x + 4));
    this.fx.push({ x, y: Math.max(b.y + 4, p.y - 4), text, color, life: 40 });
  },

  // ---- what the world currently allows ------------------------------------
  //
  // No `requires` clause is duplicated here. Each entry names the JUDGMENT the
  // story guards the action with, and the button's enabled state is that
  // judgment's verdict — so an author changing the rule changes the UI.
  offer(w, who) {
    const at = w.state.at(who);
    const out = [];
    const add = (label, act, ok, because) => out.push({ label, act, ok, because });

    if (at === 'cellar') add('GO TO HALL', w.a.go_hall(who), true);
    if (at === 'hall') {
      add('GO TO CELLAR', w.a.go_cellar(who), true);
      add('ENTER VAULT', w.a.enter_vault(who),
          w.q.can_enter_vault(who) === 'proved', w.lit.can_enter_vault(who));
      add('UNLOCK DOOR', w.a.unlock(who),
          w.q.can_unlock_door(who) === 'proved', w.lit.can_unlock_door(who));
      add('FORCE DOOR', w.a.force_door(who),
          w.q.can_force_door(who) === 'proved', w.lit.can_force_door(who));
    }
    if (at === 'vault') add('LEAVE VAULT', w.a.leave_vault(who), true);

    for (const it of SORTS.item) {
      if (w.state.on_floor(it, at)) add(`TAKE ${word(it)}`, w.a.take(who, it, at), true);
      else if (w.state.holding(who, it)) add(`DROP ${word(it)}`, w.a.drop(who, it, at), true);
    }
    if (w.state.holding(who, 'antidote') && w.state.poisoned(who))
      add('DRINK ANTIDOTE', w.a.drink(who), true);

    let y = BAR_Y + 12;
    for (const b of out) { b.x = 4; b.y = y; b.w = 120; b.h = 9; y += 10; }
    return out;
  },

  actorRect(w, who) {
    const room = w.state.at(who);
    if (!room) return null;
    const s = slot(room, who === 'hero' ? 0 : 1);
    return { ...s, w: 8, h: 8 };
  },

  // ---- the frame ----------------------------------------------------------
  draw(ctx) {
    const { draw: d, world: w } = ctx;
    d.cls(INK);

    // title
    d.rectfill(0, 0, 320, 10, DIM);
    d.print('THE CELLAR', 4, 2, BONE);
    d.print(`DOOR: ${word(w.state.door())}   TICK ${ctx.tick}`, 120, 2, MID);
    d.print('TAB SWITCHES', 244, 2, MID);

    for (const room of ROOMS) this.drawRoom(ctx, room);
    this.drawDoor(ctx);

    // the command bar
    d.rectfill(0, BAR_Y, 320, 180 - BAR_Y, INK);
    d.line(0, BAR_Y, 319, BAR_Y, DIM);
    d.print(`${word(this.sel)} — ${word(w.state.at(this.sel))}`, 4, BAR_Y + 3,
            this.sel === 'hero' ? SKY : RED);
    for (const b of this.buttons) {
      d.rect(b.x, b.y, b.w, b.h, b.ok ? MID : DIM);
      d.print(b.label, b.x + 3, b.y + 2, b.ok ? BONE : DIM);
    }

    this.drawStatus(ctx);
    this.drawWhy(ctx);

    // burst cues, animating above the line: presentation state, no fact
    d.clip(0, 10, 320, BAR_Y - 10);
    for (const f of this.fx) {
      d.print(f.text, f.x - d.textWidth(f.text) / 2, f.y - (40 - f.life) / 4, f.color);
      f.life--;
    }
    d.clip();
    this.fx = this.fx.filter((f) => f.life > 0);
  },

  drawRoom(ctx, room) {
    const { draw: d, world: w } = ctx;
    const b = BOX[room];
    for (let ty = 0; ty < b.h / 8; ty++)
      for (let tx = 0; tx < b.w / 8; tx++)
        d.tile('main', T.FLOOR, b.x + tx * 8, b.y + ty * 8);
    d.rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, MID);
    d.print(word(room), b.x, b.y - 8, MID);

    // items on this floor
    let i = 5;
    for (const it of SORTS.item)
      if (w.state.on_floor(it, room)) {
        const s = slot(room, i++);
        d.spr('main', ITEM_TILE[it], s.x, s.y);
      }

    // actors, and what they carry
    for (const who of SORTS.actor) {
      if (w.state.at(who) !== room) continue;
      const s = slot(room, who === 'hero' ? 0 : 1);
      const down = w.q.down(who) === 'proved';
      d.spr('main', who === 'hero' ? T.HERO : T.GUARD, s.x, s.y,
            { flipY: down, alpha: down ? 0.5 : 1 });
      if (who === this.sel) d.rect(s.x - 2, s.y - 2, 12, 12, GOLD);
      let k = 0;
      for (const it of SORTS.item)
        if (w.state.holding(who, it)) d.spr('main', ITEM_TILE[it], s.x + 9, s.y + (k++) * 6,
                                            { alpha: 0.9 });
      if (w.state.poisoned(who)) d.circfill(s.x + 4, s.y - 3, 1, LIME);
    }

    // THE composite op: the cellar is dark for whoever is standing in it
    // without the torch, and "dark" is a judgment the story owns.
    const anyoneInDark = SORTS.actor.some(
      (a) => w.state.at(a) === room && w.q.in_dark(a) === 'proved');
    if (anyoneInDark)
      for (let ty = 0; ty < b.h / 8; ty++)
        for (let tx = 0; tx < b.w / 8; tx++)
          d.shade('main', T.DITHER, b.x + tx * 8, b.y + ty * 8);
  },

  drawDoor(ctx) {
    const { draw: d, world: w } = ctx;
    const v = w.state.door();
    d.rectfill(DOOR.x, DOOR.y, DOOR.w, DOOR.h, v === 'open' ? INK : 8);
    d.rect(DOOR.x, DOOR.y, DOOR.w, DOOR.h, v === 'open' ? DIM : MID);
    if (v === 'locked') d.circfill(DOOR.x + 4, DOOR.y + 8, 1, GOLD);
    d.line(BOX.hall.x + BOX.hall.w, DOOR.y + 8, DOOR.x, DOOR.y + 8, DIM);
    d.line(DOOR.x + DOOR.w, DOOR.y + 8, BOX.vault.x, DOOR.y + 8, DIM);
  },

  drawStatus(ctx) {
    const { draw: d, world: w } = ctx;
    let y = BAR_Y + 3;
    for (const who of SORTS.actor) {
      const hp = w.state.hp(who);
      d.print(word(who), WHY_X, y, who === this.sel ? BONE : MID);
      d.rectfill(WHY_X + 28, y, 24, 5, DIM);
      d.rectfill(WHY_X + 28, y, Math.max(0, Math.round(24 * hp / 12)), 5,
                 hp > 6 ? LIME : RED);
      d.print(`${hp}`, WHY_X + 55, y, MID);
      d.print(w.q.weakened(who) === 'proved' ? 'WEAK' : '', WHY_X + 68, y, RED);
      y += 7;
    }
    if (this.note)  d.print(clamp(this.note, 46), WHY_X, y, GOLD);
    if (this.event) d.print(clamp(this.event, 46), WHY_X, y + 7, SKY);
  },

  /** The `why?` debugger, on screen. A refused button is not a dead end — it
   *  is the trace of the argument that refused it (§5.1), which is the whole
   *  product thesis pointed at a UI: the rule that would have applied, the
   *  rule that beat it, and the superiority that decided between them. It
   *  covers the map rather than the commands, so the answer and the question
   *  are readable together. */
  drawWhy(ctx) {
    if (!this.why) return;
    const { draw: d } = ctx;
    d.rectfill(2, 12, 316, BAR_Y - 14, 1);
    d.rect(2, 12, 316, BAR_Y - 14, DIM);
    let ly = 15;
    // Wrapped, not truncated. A trace line ends in the operands the solve
    // actually compared, so cutting it at the panel edge throws away the half
    // that answers the question. Colour is decided per SOURCE line, so a rule
    // that applied stays one red statement however many rows it takes.
    for (const src of this.why.split('\n')) {
      if (!src.trim()) continue;
      const c = src.includes('-- applicable') ? RED : PALE;
      for (const line of wrap(src, 77)) {
        if (ly > BAR_Y - 8) return;
        d.print(line, 5, ly, c);
        ly += 6;
      }
    }
  },
};

// ---- small helpers ---------------------------------------------------------

const hit = (p, r) => p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
const word = (s) => String(s ?? '?').replace(/_/g, ' ').toUpperCase();
const clamp = (s, n) => (s.length > n ? s.slice(0, n - 1) + '>' : s);

/** One line to `n` columns, continuing under its own indent so a proof trace
 *  keeps the shape that makes it readable. */
function wrap(line, n) {
  const cont = (line.match(/^ */)?.[0] ?? '') + '  ';
  const out = [];
  let rest = line;
  while (rest.length > n) {
    let cut = rest.lastIndexOf(' ', n);
    if (cut <= cont.length) cut = n;
    out.push(rest.slice(0, cut));
    rest = cont + rest.slice(cut).replace(/^ +/, '');
  }
  out.push(rest);
  return out;
}

/** `pickup(hero,torch)` -> ['pickup', 'hero'] — cue names, not atoms to query. */
function parseTerm(t) {
  const i = t.indexOf('(');
  if (i < 0) return [t, null];
  return [t.slice(0, i), t.slice(i + 1, -1).split(',')[0]];
}
