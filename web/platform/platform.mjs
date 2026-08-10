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
         GLYPH_W, GLYPH_H, textWidth } from './spec.mjs';

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
    print:    (text, x, y, c = 5)        => be.print(String(text),
                                                     int('print', 'x', x), int('print', 'y', y),
                                                     col('print', c)),
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
 */
function makeInput(be) {
  const empty = { x: 0, y: 0, buttons: new Array(BUTTONS).fill(false), keys: new Set() };
  let cur = empty, prev = empty;
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
            keys: new Set(raw.keys ?? []) };
  };
  return { api, sample };
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

  const { api: input, sample } = makeInput(backend);
  return {
    /** exactly what a cart sees, and nothing more */
    cart: {
      draw:  makeDraw(backend),
      input,
      audio: makeAudio(backend),
      data:  makeData(backend),
    },
    /** runtime-only: the once-per-tick input snapshot */
    sampleInput: sample,
  };
}
