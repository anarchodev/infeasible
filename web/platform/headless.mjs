// headless.mjs — a backend that draws into an array instead of a canvas.
//
// This is what makes the frozen op set testable without a browser, and it is
// the shape a native player (§13) implements too: a frame is a LIST OF OPS, so
// "what did the cart draw?" is a question with a data answer rather than a
// screenshot. `platform_check.mjs` runs the real cart against this backend and
// asserts on the ops.
//
// It is also the honest second implementation of the interface. §12 gave up
// the two-live-renderers argument when Canvas2D became the only renderer; what
// replaced it is the frozen op-set discipline, and a backend that implements
// the ops without any drawing at all is the cheapest possible proof that the
// discipline holds — nothing here can accidentally depend on a canvas.

import { DRAW_OPS, AUDIO_OPS, PALETTE, DEFAULT_RESOLUTION } from './spec.mjs';

export function createHeadlessBackend({ resolution = DEFAULT_RESOLUTION } = {}) {
  const [W, H] = resolution;
  const sheets = new Map();
  let frame = [];
  let camera = { x: 0, y: 0 };
  let cartdata = new Int32Array(64);

  const rec = (...op) => { frame.push(op); };

  const be = {
    width: W, height: H,

    defineSheet(name, def) { sheets.set(name, def); },
    sheet(name) {
      const s = sheets.get(name);
      if (!s) throw new Error(`unknown sheet '${name}'`);
      return s;
    },

    // ---- the frozen draw ops --------------------------------------------
    beginFrame()               { frame = []; camera = { x: 0, y: 0 }; },
    cls(c)                     { rec('cls', c); },
    camera(x, y)               { camera = { x, y }; rec('camera', x, y); },
    clip(r)                    { rec('clip', r); },
    tile(sheet, i, x, y)       { be.sheet(sheet); rec('tile', sheet, i, x, y); },
    spr(sheet, i, x, y, o)     { be.sheet(sheet); rec('spr', sheet, i, x, y, o); },
    shade(sheet, i, x, y)      { be.sheet(sheet); rec('shade', sheet, i, x, y); },
    print(t, x, y, c, o)       { rec('print', t, x, y, c, o); },
    line(x0, y0, x1, y1, c)    { rec('line', x0, y0, x1, y1, c); },
    rect(x, y, w, h, c)        { rec('rect', x, y, w, h, c); },
    rectfill(x, y, w, h, c)    { rec('rectfill', x, y, w, h, c); },
    circ(x, y, r, c)           { rec('circ', x, y, r, c); },
    circfill(x, y, r, c)       { rec('circfill', x, y, r, c); },

    // ---- audio: recorded, never played ----------------------------------
    played: [],
    sound(id, gain)  { be.played.push(['sound', id, gain]); },
    stop(id)         { be.played.push(['stop', id]); },
    music(id, gain)  { be.played.push(['music', id, gain]); },
    music_stop()     { be.played.push(['music_stop']); },

    // ---- input: scripted, never live ------------------------------------
    // The test driver sets this; the platform samples it at tick boundaries
    // exactly as it samples a browser's.
    raw: { x: 0, y: 0, buttons: [false, false, false], keys: new Set() },
    readInput() {
      const keys = be.raw.keys ?? new Set();
      const nav = new Set(be.raw.nav ?? []);
      for (const d of ['left', 'right', 'up', 'down']) if (keys.has(d)) nav.add(d);
      return { ...be.raw, keys, nav,
               confirm: !!be.raw.confirm || keys.has('enter') || keys.has('space'),
               cancel:  !!be.raw.cancel  || keys.has('escape') };
    },
    point(x, y, buttons = []) {
      be.raw = { ...be.raw, x, y,
                 buttons: [!!buttons[0], !!buttons[1], !!buttons[2]] };
    },
    press(...keys) { be.raw = { ...be.raw, keys: new Set(keys) }; },
    /** Drive the PORTABLE path directly — no pointer, no keyboard, the way a
     *  gamepad backend would. This is what lets a check play a cart the way a
     *  console would and prove it is reachable without a mouse. */
    pad({ nav = [], confirm = false, cancel = false } = {}) {
      be.raw = { ...be.raw, nav: new Set(nav), confirm, cancel };
    },

    loadCartdata() { return cartdata; },
    saveCartdata(blob) { cartdata = Int32Array.from(blob); },

    // ---- inspection -------------------------------------------------------
    /** The ops recorded this frame — one frame, as data. */
    lastFrame: () => frame,
    /** Every op of a given kind in the last frame. */
    ops: (kind) => frame.filter((o) => o[0] === kind),
    /** Text drawn this frame, joined — the cheapest assertion a UI test can
     *  make, and enough to pin what the cart said about the world. */
    text: () => frame.filter((o) => o[0] === 'print').map((o) => o[1]).join('\n'),
    cameraPos: () => camera,
    palette: PALETTE,
    present() {},
  };

  // the freeze, checked at construction rather than trusted
  for (const op of [...DRAW_OPS, ...AUDIO_OPS])
    if (typeof be[op] !== 'function')
      throw new Error(`headless backend is missing '${op}'`);
  return be;
}
