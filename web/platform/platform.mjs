// platform.mjs — the four cart-facing surfaces, assembled over a backend
// (DESIGN.md §12).
//
// A cart is handed exactly one object, `{ draw, input, audio, data }`, and
// nothing else is reachable from it. That is the durability line drawn in
// code: the moment content reaches for `addEventListener`, Web Audio or
// `localStorage`, the optional native player (§13) stops implementing a dozen
// ops and starts reimplementing a browser.
//
// The surfaces here are thin on purpose. `draw` validates its arguments and
// forwards to the backend's op of the same name — the frozen op set is one
// list (spec.mjs) and one dispatch, so a backend cannot quietly grow a
// thirteenth op that a cart then depends on. What is NOT thin is `input`:
// polling, edge detection and the letterbox inverse live here, below the line,
// because they are exactly the parts every cart would otherwise reimplement
// slightly differently.

import { DRAW_OPS, AUDIO_OPS, KEYS, BUTTONS, PALETTE, CARTDATA_CELLS,
         NAV_DIRS,
         GLYPH_W, GLYPH_H, BIG_GLYPH_W, BIG_GLYPH_H, textWidth } from './spec.mjs';

const KEYSET = new Set(KEYS);

/** A cart argument error is a bug in the cart, and it should say so at the
 *  call rather than paint something wrong. */
function bad(op, msg) { throw new TypeError(`${op}: ${msg}`); }

const int = (op, name, v) => {
  if (typeof v !== 'number' || !Number.isFinite(v)) bad(op, `${name} must be a number`);
  return Math.round(v);
};
const col = (op, v) => {
  if (!Number.isInteger(v) || v < 0 || v >= PALETTE.length)
    bad(op, `color must be a palette index 0..${PALETTE.length - 1}, got ${v}`);
  return v;
};

/**
 * The draw surface: validate, then forward. Every op is listed in
 * spec.DRAW_OPS and implemented by every backend — platform_check.mjs pins
 * that correspondence in both directions.
 */
function makeDraw(be) {
  const d = {
    cls:      (c = 0)                    => be.cls(col('cls', c)),
    camera:   (x = 0, y = 0)             => be.camera(int('camera', 'x', x),
                                                      int('camera', 'y', y)),
    clip:     (x, y, w, h)               => (x === undefined ? be.clip(null)
                                             : be.clip({ x: int('clip', 'x', x),
                                                         y: int('clip', 'y', y),
                                                         w: int('clip', 'w', w),
                                                         h: int('clip', 'h', h) })),
    tile:     (sheet, i, x, y)           => be.tile(sheet, i | 0,
                                                    int('tile', 'x', x), int('tile', 'y', y)),
    spr:      (sheet, i, x, y, o = {})   => be.spr(sheet, i | 0,
                                                   int('spr', 'x', x), int('spr', 'y', y),
                                                   { flipX: !!o.flipX, flipY: !!o.flipY,
                                                     alpha: o.alpha === undefined ? 1 : +o.alpha }),
    shade:    (sheet, i, x, y)           => be.shade(sheet, i | 0,
                                                     int('shade', 'x', x), int('shade', 'y', y)),
    print:    (text, x, y, c = 5, o = {}) => be.print(String(text),
                                                      int('print', 'x', x), int('print', 'y', y),
                                                      col('print', c), { big: !!o.big }),
    line:     (x0, y0, x1, y1, c)        => be.line(int('line', 'x0', x0), int('line', 'y0', y0),
                                                    int('line', 'x1', x1), int('line', 'y1', y1),
                                                    col('line', c)),
    rect:     (x, y, w, h, c)            => be.rect(int('rect', 'x', x), int('rect', 'y', y),
                                                    int('rect', 'w', w), int('rect', 'h', h),
                                                    col('rect', c)),
    rectfill: (x, y, w, h, c)            => be.rectfill(int('rectfill', 'x', x), int('rectfill', 'y', y),
                                                        int('rectfill', 'w', w), int('rectfill', 'h', h),
                                                        col('rectfill', c)),
    circ:     (x, y, r, c)               => be.circ(int('circ', 'x', x), int('circ', 'y', y),
                                                    int('circ', 'r', r), col('circ', c)),
    circfill: (x, y, r, c)               => be.circfill(int('circfill', 'x', x), int('circfill', 'y', y),
                                                        int('circfill', 'r', r), col('circfill', c)),
  };
  // metrics, not ops: derived from the frozen text cell so a cart never
  // measures text by guessing at the backend's font
  d.glyphW = GLYPH_W;
  d.glyphH = GLYPH_H;
  d.bigGlyphW = BIG_GLYPH_W;
  d.bigGlyphH = BIG_GLYPH_H;
  d.textWidth = textWidth;
  return d;
}

/** Audio: four write-only calls, forwarded. There is deliberately no `playing`
 *  or `position` — see spec.mjs. */
function makeAudio(be) {
  return {
    sound:      (id, gain = 1) => be.sound(String(id), +gain),
    stop:       (id)           => be.stop(String(id)),
    music:      (id, gain = 1) => be.music(String(id), +gain),
    music_stop: ()             => be.music_stop(),
  };
}

/** Persistence: one fixed-size numeric blob, loaded once and written through
 *  the backend. A fluent may never be initialised from it, or a world's
 *  history stops being a function of its log (§12). */
function makeData(be) {
  const cells = Int32Array.from(be.loadCartdata?.() ?? new Int32Array(CARTDATA_CELLS));
  const blob = new Int32Array(CARTDATA_CELLS);
  blob.set(cells.subarray(0, CARTDATA_CELLS));
  return {
    get: (i) => {
      if (!Number.isInteger(i) || i < 0 || i >= CARTDATA_CELLS)
        bad('data.get', `index must be 0..${CARTDATA_CELLS - 1}`);
      return blob[i];
    },
    set: (i, v) => {
      if (!Number.isInteger(i) || i < 0 || i >= CARTDATA_CELLS)
        bad('data.set', `index must be 0..${CARTDATA_CELLS - 1}`);
      blob[i] = v | 0;
      be.saveCartdata?.(blob);
    },
  };
}

/**
 * The input surface. The backend supplies raw state on demand
 * (`readInput() -> {x, y, buttons:boolean[], keys:Set<string>}`); the sampling
 * discipline is here, because it is a determinism rule and not a backend
 * detail: `sample()` is called ONCE per tick at the tick boundary, and
 * everything a cart reads during that tick is that frozen snapshot. Edge
 * queries (`pressed`, `keyp`) compare it against the previous tick's.
 *
 * `readInput` is therefore called exactly once per tick, and a backend may
 * treat the call as CONSUMING input — a live one has to, since a click can
 * begin and end between two samples and would otherwise be lost (see the latch
 * in canvas2d.mjs). A scripted backend has no such problem and just returns
 * what it was told.
 */
function makeInput(be) {
  const empty = { x: 0, y: 0, buttons: new Array(BUTTONS).fill(false),
                  keys: new Set(), nav: new Set(), confirm: false, cancel: false,
                  moved: false };
  let cur = empty, prev = empty, gen = 0;
  const api = {
    pointer: () => ({ x: cur.x, y: cur.y }),
    button:  (i = 0) => !!cur.buttons[i],
    pressed: (i = 0) => !!cur.buttons[i] && !prev.buttons[i],
    key:     (k) => { if (!KEYSET.has(k)) bad('key', `'${k}' is not a frozen key name`);
                      return cur.keys.has(k); },
    keyp:    (k) => { if (!KEYSET.has(k)) bad('keyp', `'${k}' is not a frozen key name`);
                      return cur.keys.has(k) && !prev.keys.has(k); },
  };
  // `sample` is NOT on the cart-facing object: a cart that could resample
  // mid-tick could observe input the replay never saw.
  const sample = () => {
    const raw = be.readInput();
    prev = cur;
    cur = { x: raw.x | 0, y: raw.y | 0,
            buttons: Array.from({ length: BUTTONS }, (_, i) => !!raw.buttons?.[i]),
            keys: new Set(raw.keys ?? []),
            nav: new Set(raw.nav ?? []),
            confirm: !!raw.confirm, cancel: !!raw.cancel,
            moved: false };
    cur.moved = cur.x !== prev.x || cur.y !== prev.y;
    gen++;
  };
  return { api, sample, state: () => ({ cur, prev, gen }) };
}

/**
 * Which target lies `dir` of `from`? The shortest RECT-TO-RECT distance, not
 * centre-to-centre — which is Godot's approach (`Control::_window_find_focus_
 * neighbor`) and matters whenever the targets are not all the same size: a
 * wide button's centre can be far from a small neighbour whose edge is
 * touching it, and centre distance picks the wrong one.
 *
 * Direction is filtered first, by whether any part of the candidate lies
 * beyond the source along the axis of travel. Unity (`Selectable.
 * FindSelectable`) instead scores `dot / |v|²` from a point on the source's
 * edge — "blow up a balloon in the direction of travel and take the first
 * centre it touches" — which is elegant but still measures to a centre, and
 * so has the same size sensitivity.
 *
 * Ties break on DECLARATION ORDER, which is why spec.mjs calls target order
 * semantics: two equidistant targets must resolve the same way on every
 * machine and every replay (I4). Both engines above tie-break too, and both
 * do it on scene order, which is the same choice.
 */
function navigate(targets, fromId, dir) {
  const from = targets.find((t) => t.id === fromId);
  if (!from) return targets.length ? targets[0].id : null;
  const ax = dir === 'left' || dir === 'right';
  const sign = (dir === 'right' || dir === 'down') ? 1 : -1;
  /* how far along the axis of travel a rect's near and far edges sit */
  const lo = (t) => sign * (ax ? t.x : t.y);
  const hi = (t) => sign * ((ax ? t.x + t.w : t.y + t.h));
  const beyond = Math.max(lo(from), hi(from));

  let best = null, bestD = Infinity;
  for (const t of targets) {
    if (t.id === fromId) continue;
    /* MAX of the two edges, so an overlapping candidate that extends further
     * in the direction of travel still counts as being that way */
    if (Math.max(lo(t), hi(t)) <= beyond) continue;
    /* gap between the rectangles on each axis; 0 when they overlap there */
    const gx = Math.max(0, Math.max(from.x - (t.x + t.w), t.x - (from.x + from.w)));
    const gy = Math.max(0, Math.max(from.y - (t.y + t.h), t.y - (from.y + from.h)));
    const d = gx * gx + gy * gy;
    if (d < bestD) { bestD = d; best = t.id; }
  }
  return best ?? fromId;                      /* nothing that way: stay put */
}

/**
 * The focus surface — the portable input model (§12, spec.FOCUS_OPS).
 *
 * The cart declares its focusable regions each tick and asks what was
 * confirmed. Everything device-specific resolves here: a d-pad walks the
 * regions geometrically, a mouse hit-tests them, a tap does both at once.
 *
 * Focus SURVIVES across ticks and is platform state, not cart state — which
 * costs replay nothing, because what a save records is the action a cart
 * submitted, never the focus that led to it.
 */
function makeFocus(inp) {
  let targets = [], id = null, confirmedId = null, cancelled = false, done = -1;
  const api = {
    targets(list) {
      targets = (list ?? []).map((t) => ({
        id: t.id, x: t.x | 0, y: t.y | 0, w: t.w | 0, h: t.h | 0,
      }));
      /* a focused thing that stopped existing hands focus to the first
       * remaining target rather than to nothing: a d-pad user with no focus
       * has no way back in */
      if (!targets.some((t) => t.id === id)) id = targets.length ? targets[0].id : null;
      resolve();
      return api;
    },
    current: () => id,
    confirmed: () => confirmedId,
    cancelled: () => cancelled,
  };

  /* Resolution happens on DECLARATION, because the platform cannot navigate a
   * list it has not been given and the cart declares and reads inside one
   * tick(). Guarded by the input generation so a cart that declares twice does
   * not get two d-pad steps out of one press. */
  const resolve = () => {
    const { cur, prev, gen } = inp.state();
    if (gen === done) return;
    done = gen;
    confirmedId = null;
    cancelled = cur.cancel && !prev.cancel;

    for (const d of NAV_DIRS)                 /* edge-triggered, like keyp */
      if (cur.nav.has(d) && !prev.nav.has(d)) id = navigate(targets, id, d);

    /* A pointer sets focus only when it MOVES. Otherwise a stale cursor
     * resting on a button would fight the d-pad for focus every tick. */
    const over = targets.find((t) => cur.x >= t.x && cur.x < t.x + t.w &&
                                     cur.y >= t.y && cur.y < t.y + t.h);
    if (cur.moved && over) id = over.id;

    const clicked = cur.buttons[0] && !prev.buttons[0];
    if (clicked) { if (over) { id = over.id; confirmedId = over.id; } }
    else if (cur.confirm && !prev.confirm) confirmedId = id;
  };
  return { api };
}

/**
 * Assemble the cart-facing platform over a backend.
 * @param {any} backend  implements the frozen draw ops, the audio ops,
 *                       `readInput()`, and optionally cartdata load/save.
 */
export function createPlatform(backend) {
  for (const op of DRAW_OPS)
    if (typeof backend[op] !== 'function')
      throw new Error(`backend is missing the frozen draw op '${op}'`);
  for (const op of AUDIO_OPS)
    if (typeof backend[op] !== 'function')
      throw new Error(`backend is missing the frozen audio op '${op}'`);

  const inp = makeInput(backend);
  const foc = makeFocus(inp);
  return {
    /** exactly what a cart sees, and nothing more */
    cart: {
      draw:  makeDraw(backend),
      input: inp.api,
      focus: foc.api,
      audio: makeAudio(backend),
      data:  makeData(backend),
    },
    /** runtime-only: the once-per-tick input snapshot */
    sampleInput: inp.sample,
  };
}
