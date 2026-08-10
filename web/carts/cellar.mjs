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

/** The TILE atlas: one char per pixel, a hex palette index or '.' for
 *  transparent. Three 8×8 tiles that repeat across a floor — the layer that
 *  pre-renders and repaints only when a delta touches it (§12). */
export const TILES = {
  tile: 8,
  cols: 3,
  pixels: [
    '11111111' + '22222222' + '15151515',
    '12111211' + '23333332' + '51515151',
    '11111111' + '23222232' + '15151515',
    '11211121' + '23222232' + '51515151',
    '11111111' + '23222232' + '15151515',
    '12111211' + '23333332' + '51515151',
    '11111111' + '22222222' + '15151515',
    '11121111' + '20000002' + '51515151',
  ],
};

/** The SPRITE atlas: five 16×16 actors and props. A sheet carries its own tile
 *  size, so bigger art costs a second sheet and not a thirteenth draw op —
 *  §12's rule that pre-baked variants are an asset-pipeline product, applied to
 *  scale as well as to colour. */
export const SPRITES = {
  tile: 16,
  cols: 5,
  pixels: [
    //  hero              guard             rusty_key         torch             antidote
    '................' + '................' + '................' + '................' + '................',
    '.....444444.....' + '....22222222....' + '................' + '.......b........' + '......5555......',
    '....44444444....' + '...2222222222...' + '.....666666.....' + '......bab.......' + '.......55.......',
    '....41444414....' + '...2244444422...' + '....66....66....' + '.....baaab......' + '.......55.......',
    '....44444444....' + '....41444414....' + '...66......66...' + '.....ba6ab......' + '.......55.......',
    '....94444449....' + '....44444444....' + '...66......66...' + '.....b666b......' + '......5555......',
    '.....444444.....' + '.....444444.....' + '....66....66....' + '......b6b.......' + '.....d5555d.....',
    '...eeeeeeeeee...' + '...aaaaaaaaaa...' + '.....666666.....' + '.......8........' + '.....dccccd.....',
    '..eee444444eee..' + '..aaa444444aaa..' + '.......66.......' + '.......8........' + '....dcccccdd....',
    '..ee44444444ee..' + '..aa44444444aa..' + '.......66.......' + '.......8........' + '....dcccdccd....',
    '...e44444444e...' + '...a44444444a...' + '.......66.......' + '.......8........' + '...dccccccccd...',
    '...eeeeeeeeee...' + '...aaaaaaaaaa...' + '.......6666.....' + '.......9........' + '...dccccccccd...',
    '....ee....ee....' + '....aa....aa....' + '.......66.......' + '.......9........' + '...dccccccccd...',
    '....ee....ee....' + '....aa....aa....' + '.......6666.....' + '.......9........' + '....dcccccccd...',
    '....44....44....' + '....44....44....' + '.......66.......' + '................' + '.....dddddd.....',
    '...444....444...' + '...444....444...' + '................' + '................' + '................',
  ],
};

const T = { FLOOR: 0, WALL: 1, DITHER: 2 };
const S = { HERO: 0, GUARD: 1, KEY: 2, TORCH: 3, FLASK: 4 };
const ITEM_SPR = { rusty_key: S.KEY, torch: S.TORCH, antidote: S.FLASK };

const ROOMS = ['cellar', 'hall', 'vault'];
/** Room boxes in internal pixels — 24×22 floor tiles each, three across with
 *  the vault door in the right-hand gap. */
const BOX = {
  cellar: { x: 8,   y: 36, w: 192, h: 176 },
  hall:   { x: 224, y: 36, w: 192, h: 176 },
  vault:  { x: 440, y: 36, w: 192, h: 176 },
};
const DOOR = { x: 420, y: 108, w: 16, h: 32 };

const BAR_Y = 224;                     // the command bar's top edge
const STAT_X = 216;                    // where the status column starts

/** The large text cell (§12): one of two frozen sizes, chosen per call. The
 *  UI takes it; the why-trace does not, because a proof line wants columns. */
const BIG = { big: true };

// palette shorthands
const INK = 0, DIM = 2, MID = 3, PALE = 4, BONE = 5, GOLD = 6, RED = 10, LIME = 13, SKY = 15;

/** Where an actor or a floor item stands inside its room: a 4-wide grid of
 *  16px sprites, actors on the top row and dropped items on the second. */
const slot = (room, i) => ({ x: BOX[room].x + 24 + (i % 4) * 40,
                             y: BOX[room].y + 28 + Math.floor(i / 4) * 44 });

export const cart = {
  resolution: [640, 360],
  sheets: { tiles: TILES, sprites: SPRITES },

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

    // Keyboard first: the click paths below return early, and a tab pressed in
    // the same tick as a click would be read by nobody. One snapshot per tick
    // means every reader of it has to run.
    if (input.keyp('tab')) {
      this.sel = this.sel === 'hero' ? 'guard' : 'hero';
      this.why = '';
    }

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
    const half = text.length * 3;
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
      // You cannot pick up what you cannot see — and the cart does not know
      // that rule either, it asks. A greyed TAKE prints why the room is dark.
      if (w.state.on_floor(it, at))
        add(`TAKE ${word(it)}`, w.a.take(who, it, at),
            w.q.in_dark(who) !== 'proved', w.lit.in_dark(who));
      else if (w.state.holding(who, it)) add(`DROP ${word(it)}`, w.a.drop(who, it, at), true);
    }
    if (w.state.holding(who, 'antidote') && w.state.poisoned(who))
      add('DRINK ANTIDOTE', w.a.drink(who), true);

    let y = BAR_Y + 20;
    for (const b of out) { b.x = 8; b.y = y; b.w = 176; b.h = 12; y += 15; }
    return out;
  },

  actorRect(w, who) {
    const room = w.state.at(who);
    if (!room) return null;
    const s = slot(room, who === 'hero' ? 0 : 1);
    return { ...s, w: 16, h: 16 };
  },

  // ---- the frame ----------------------------------------------------------
  draw(ctx) {
    const { draw: d, world: w } = ctx;
    const [W, H] = this.resolution;
    d.cls(INK);

    // title. Everything a player reads at a glance is in the LARGE cell; the
    // small one is kept for the proof trace, which wants columns instead.
    d.rectfill(0, 0, W, 14, DIM);
    d.print('THE CELLAR', 8, 3, BONE, BIG);
    d.print(`DOOR: ${word(w.state.door())}`, 240, 3, MID, BIG);
    d.print(`TICK ${ctx.tick}`, 380, 3, MID, BIG);
    d.print('TAB SWITCHES', 512, 3, MID, BIG);

    for (const room of ROOMS) this.drawRoom(ctx, room);
    this.drawDoor(ctx);

    // the command bar
    d.line(0, BAR_Y, W - 1, BAR_Y, DIM);
    d.print(`${word(this.sel)} - ${word(w.state.at(this.sel))}`, 8, BAR_Y + 5,
            this.sel === 'hero' ? SKY : RED, BIG);
    for (const b of this.buttons) {
      d.rect(b.x, b.y, b.w, b.h, b.ok ? MID : DIM);
      d.print(b.label, b.x + 4, b.y + 2, b.ok ? BONE : DIM, BIG);
    }

    this.drawStatus(ctx);
    this.drawWhy(ctx);

    // burst cues, animating above the line: presentation state, no fact
    d.clip(0, 14, W, BAR_Y - 14);
    for (const f of this.fx) {
      d.print(f.text, f.x - d.textWidth(f.text, true) / 2, f.y - (40 - f.life) / 4,
              f.color, BIG);
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
        d.tile('tiles', T.FLOOR, b.x + tx * 8, b.y + ty * 8);
    d.rect(b.x - 1, b.y - 1, b.w + 2, b.h + 2, MID);
    d.print(word(room), b.x, b.y - 11, MID, BIG);

    // items on this floor — the second row of slots
    let i = 4;
    for (const it of SORTS.item)
      if (w.state.on_floor(it, room)) {
        const s = slot(room, i++);
        d.spr('sprites', ITEM_SPR[it], s.x, s.y);
      }

    // THE composite op, applied BEFORE the actors: the room goes dark, the
    // people in it do not. Fog is about what you can make out around you, and
    // a player who cannot see the character they are steering has been given
    // an atmosphere instead of a game. "Dark" is still the story's judgment;
    // only the draw order is presentation's business.
    const anyoneInDark = SORTS.actor.some(
      (a) => w.state.at(a) === room && w.q.in_dark(a) === 'proved');
    if (anyoneInDark)
      for (let ty = 0; ty < b.h / 8; ty++)
        for (let tx = 0; tx < b.w / 8; tx++)
          d.shade('tiles', T.DITHER, b.x + tx * 8, b.y + ty * 8);

    // actors, and what they carry
    for (const who of SORTS.actor) {
      if (w.state.at(who) !== room) continue;
      const s = slot(room, who === 'hero' ? 0 : 1);
      const down = w.q.down(who) === 'proved';
      d.spr('sprites', who === 'hero' ? S.HERO : S.GUARD, s.x, s.y,
            { flipY: down, alpha: down ? 0.5 : 1 });
      if (who === this.sel) d.rect(s.x - 3, s.y - 3, 22, 22, GOLD);
      if (w.state.poisoned(who)) d.circfill(s.x + 8, s.y - 6, 2, LIME);

      // carried items, in a row centred under the carrier
      const held = SORTS.item.filter((it) => w.state.holding(who, it));
      let k = 0;
      for (const it of held)
        d.spr('sprites', ITEM_SPR[it],
              s.x + 8 - held.length * 7 + (k++) * 14, s.y + 20, { alpha: 0.9 });
    }
  },

  drawDoor(ctx) {
    const { draw: d, world: w } = ctx;
    const v = w.state.door();
    const midY = DOOR.y + DOOR.h / 2;
    d.line(BOX.hall.x + BOX.hall.w, midY, DOOR.x, midY, DIM);
    d.line(DOOR.x + DOOR.w, midY, BOX.vault.x, midY, DIM);
    if (v === 'open') {                    // a doorway, with the leaf swung back
      d.rect(DOOR.x, DOOR.y, DOOR.w, DOOR.h, DIM);
      d.rectfill(DOOR.x + 1, DOOR.y + 1, 3, DOOR.h - 2, 9);
      return;
    }
    d.rectfill(DOOR.x, DOOR.y, DOOR.w, DOOR.h, v === 'jammed' ? 9 : 8);
    d.rect(DOOR.x, DOOR.y, DOOR.w, DOOR.h, MID);
    d.line(DOOR.x + 2, DOOR.y + 4, DOOR.x + DOOR.w - 3, DOOR.y + 4, 8);
    d.line(DOOR.x + 2, DOOR.y + DOOR.h - 5, DOOR.x + DOOR.w - 3, DOOR.y + DOOR.h - 5, 8);
    if (v === 'locked') d.circfill(DOOR.x + DOOR.w - 4, midY, 2, GOLD);
  },

  drawStatus(ctx) {
    const { draw: d, world: w } = ctx;
    let y = BAR_Y + 5;
    for (const who of SORTS.actor) {
      const hp = w.state.hp(who);
      d.print(word(who), STAT_X, y, who === this.sel ? BONE : MID, BIG);
      d.rectfill(STAT_X + 44, y + 1, 48, 6, DIM);
      d.rectfill(STAT_X + 44, y + 1, Math.max(0, Math.round(48 * hp / 12)), 6,
                 hp > 6 ? LIME : RED);
      d.print(`${hp}`, STAT_X + 98, y, MID, BIG);
      d.print(w.q.weakened(who) === 'proved' ? 'WEAKENED' : '', STAT_X + 122, y, RED, BIG);
      y += 11;
    }
    if (this.note)  d.print(clamp(this.note, 68), STAT_X, y + 4, GOLD, BIG);
    if (this.event) d.print(clamp(this.event, 68), STAT_X, y + 15, SKY, BIG);
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
    const [W] = this.resolution;
    d.rectfill(4, 18, W - 8, BAR_Y - 26, 1);
    d.rect(4, 18, W - 8, BAR_Y - 26, DIM);
    let ly = 22;
    // Wrapped, not truncated. A trace line ends in the operands the solve
    // actually compared, so cutting it at the panel edge throws away the half
    // that answers the question. Colour is decided per SOURCE line, so a rule
    // that applied stays one red statement however many rows it takes.
    for (const src of this.why.split('\n')) {
      if (!src.trim()) continue;
      const c = src.includes('-- applicable') ? RED : PALE;
      for (const line of wrap(src, (W - 20) / 4)) {
        if (ly > BAR_Y - 12) return;
        d.print(line, 8, ly, c);
        ly += 8;
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
