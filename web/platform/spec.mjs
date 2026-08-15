// spec.mjs — the presentation interface, as data (DESIGN.md §12).
//
// §12 fixes the SIZE of the presentation swap surface: a PICO-8-sized, frozen
// API, so that porting the client is a weekend and not a rewrite. This file is
// that freeze written down once, in one place, so nothing has to be trusted to
// stay small by convention:
//
//   - the four surfaces a cart may call (draw, input, audio, persistence) and
//     their exact op names;
//   - the constants those ops are defined against — the blessed resolution
//     set, the 16-entry palette, the text cell, the key set;
//   - the letterbox arithmetic, which lives BELOW the line because every cart
//     computing it independently is every cart getting the edges wrong.
//
// `platform_check.mjs` asserts that each backend implements exactly the op
// lists below — no more, no less. That mechanical check is what makes "frozen"
// a property of the code rather than a promise in a document: adding an op to
// a renderer fails the check until it is added here, deliberately, and every
// other backend grows it too.

/** Internal resolutions: the 1080-divisor set, so every choice integer-scales
 *  cleanly to 1080p and 4K forever (§12). A game picks one in its manifest;
 *  the choice is *within* the frozen contract, never a widening of it. */
export const RESOLUTIONS = [[320, 180], [480, 270], [640, 360], [960, 540]];

/** The reference default: SC1's density in 16:9, ×3 to 1080p. */
export const DEFAULT_RESOLUTION = [640, 360];

/** The 16-entry palette. Every colour argument in the draw ops is an index
 *  into this, never a CSS string — a fixed palette is what makes pre-baked
 *  recolored atlas variants an asset-pipeline product instead of a renderer
 *  op, and what lets a native player match the browser exactly. */
export const PALETTE = [
  '#0b0d10', '#1d2433', '#3b4b63', '#6b7f99',   //  0-3  ink → mist
  '#c3cfdd', '#f2f0e6', '#e8c37a', '#c98a3e',   //  4-7  bone → brass
  '#8a5a2b', '#5a3a22', '#a33b3b', '#e06666',   //  8-11 wood → blood
  '#4c8c4a', '#8fd45a', '#3f6fa8', '#79b8e8',   // 12-15 moss → sky
];

/** The draw surface (§12: "roughly a dozen ops"). Kept in one flat list so the
 *  freeze is countable. */
export const DRAW_OPS = [
  'cls',        // cls(color)                       — clear to a palette index
  'camera',     // camera(x, y)                     — translate every later op
  'clip',       // clip(x, y, w, h) | clip()        — restrict later ops
  'tile',       // tile(sheet, index, x, y)         — atlas blit, the map layer
  'spr',        // spr(sheet, index, x, y, opts)    — sprite: flipX/flipY/alpha
  'shade',      // shade(sheet, index, x, y)        — THE composite op (fog)
  'print',      // print(text, x, y, color, opts)   — text: one of two cells
  'line',       // line(x0, y0, x1, y1, color)
  'rect',       // rect(x, y, w, h, color)
  'rectfill',   // rectfill(x, y, w, h, color)
  'circ',       // circ(x, y, r, color)
  'circfill',   // circfill(x, y, r, color)
];

/** Audio is WRITE-ONLY (§12): a cart may start and stop sound and may never
 *  ask whether something is playing, how far in it is, or whether it finished.
 *  Each of those readbacks is wall-clock in disguise, and a rule branching on
 *  one is nondeterministic by construction (I4). */
export const AUDIO_OPS = ['sound', 'stop', 'music', 'music_stop'];

/** Input is POLLED, never delivered as events (§12) — the save is an action
 *  log, so a callback firing between ticks would let a cart branch on input
 *  the replay never observes. Sampled once per tick, at the tick boundary.
 *
 *  POINTER AND KEYS ARE NOT UNIVERSAL. A gamepad has neither: no position, no
 *  keyboard. A touchscreen has a position only while a finger is down, so it
 *  has taps but no hover. Only the FOCUS surface below is answerable on every
 *  device, so a cart that reads these directly is a desktop cart — which is a
 *  legitimate thing to be, and is why they are still here. */
export const INPUT_OPS = ['pointer', 'button', 'pressed', 'key', 'keyp'];

/** The portable input model (§12).
 *
 *  Pointer-and-keyboard is not portable, and the fix is not to add a gamepad
 *  op beside it: a cart written against three input styles is a cart written
 *  three times. The asymmetry that decides the design is that FOCUS CAN BE
 *  DRIVEN BY ANY OF THEM, and pointer cannot be derived from the others —
 *  given a set of focusable regions, a d-pad walks it geometrically, a tap
 *  hit-tests it, and an arrow key does what the d-pad does. Given only a
 *  pointer, nothing can synthesise navigation, because nothing knows what is
 *  navigable.
 *
 *  So the cart declares its focusable regions each tick and asks what was
 *  confirmed. The platform owns focus movement, hit-testing and the edge
 *  detection; the backend supplies whichever raw inputs it has.
 *
 *    focus.targets(list)  declare this tick's focusables, in order:
 *                         [{ id, x, y, w, h }, ...]. Order is the tiebreak for
 *                         geometric navigation, so it is semantics (I4).
 *    focus.current()      the focused id, or null
 *    focus.confirmed()    the id confirmed THIS tick, or null
 *    focus.cancelled()    whether cancel was pressed this tick
 *
 *  A cart built on this runs on a d-pad, a touchscreen and a mouse without
 *  knowing which it has. */
export const FOCUS_OPS = ['targets', 'current', 'confirmed', 'cancelled'];

/** Directional navigation intent, edge-triggered like `keyp`. A backend maps
 *  its own d-pad, stick or arrow keys onto these; it is never raw axes, for
 *  the same reason KEYS is never raw codes. */
export const NAV_DIRS = ['left', 'right', 'up', 'down'];

/** Persistence is not storage (§12): one small fixed-size numeric blob for
 *  cross-run NON-game state (settings, cosmetic unlocks), explicitly outside
 *  the save and explicitly not readable by rules. The save itself is
 *  (engine-hash, game-hash, action-log) and the platform owns it. */
export const DATA_OPS = ['get', 'set'];
export const CARTDATA_CELLS = 64;

/** Three pointer buttons — left, middle, right. */
export const BUTTONS = 3;

/** The frozen named key set. Never raw codes: browser `KeyboardEvent.code`
 *  and native scancodes disagree, and a raw code is the platform leaking into
 *  the cart. A backend maps its own codes onto these names. */
export const KEYS = [
  'left', 'right', 'up', 'down',
  'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
  'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
  'space', 'enter', 'escape', 'tab', 'shift', 'ctrl', 'alt',
];

/** The text cells. Frozen as METRICS, not as glyphs: a backend may draw its
 *  letters however it likes, but `print` advances the cell width per character
 *  and a line is the cell height everywhere. Freezing layout and not shapes is
 *  what lets a native player substitute its own bitmap font without every
 *  cart's UI shifting by a pixel.
 *
 *  There are TWO cells because one density cannot serve both jobs, and the
 *  arithmetic says so rather than taste. At 640×360 the small cell gives 160
 *  columns — a proof trace reads unwrapped — but a capital is 1.4% of screen
 *  height against PICO-8's 3.9%, which is a terminal, not a game UI. So `print`
 *  takes `{ big: true }`, the way `spr` takes flip and alpha: a second SIZE on
 *  an existing op, never a thirteenth op. A backend implements two bitmap
 *  fonts; that is still a weekend to port. */
export const GLYPH_W = 4;
export const GLYPH_H = 6;
export const BIG_GLYPH_W = 6;
export const BIG_GLYPH_H = 8;

/** Width of a string in internal pixels — derived, so it is not an op. */
export const textWidth = (s, big = false) =>
  s.length * (big ? BIG_GLYPH_W : GLYPH_W);

/**
 * The upscale: nearest-neighbor INTEGER scale with letterboxing. A wider
 * window must never reveal more map — under lockstep that is a fairness rule,
 * not taste (§12) — so the internal resolution is fixed and the surplus
 * becomes bars.
 * @returns {{scale:number, x:number, y:number, w:number, h:number}} the
 *   destination rect in display pixels.
 */
export function letterbox(displayW, displayH, internalW, internalH) {
  const scale = Math.max(1, Math.floor(Math.min(displayW / internalW,
                                                displayH / internalH)));
  const w = internalW * scale, h = internalH * scale;
  return { scale, w, h,
           x: Math.floor((displayW - w) / 2), y: Math.floor((displayH - h) / 2) };
}

/**
 * The inverse: a display-space point back to internal coordinates, clamped to
 * the surface. This belongs below the line for the same reason the letterbox
 * does — every cart computing it independently is every cart getting the edges
 * wrong (§12).
 */
export function toInternal(px, py, box, internalW, internalH) {
  const x = Math.floor((px - box.x) / box.scale);
  const y = Math.floor((py - box.y) / box.scale);
  return { x: Math.min(internalW - 1, Math.max(0, x)),
           y: Math.min(internalH - 1, Math.max(0, y)) };
}
