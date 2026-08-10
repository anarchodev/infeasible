// canvas2d.mjs — the reference renderer (DESIGN.md §12).
//
// A few hundred dependency-free lines, below the durability line. Canvas2D is
// the ONLY renderer (decided 2026-07-21): no WebGL/Pixi backend and no native
// one. The arithmetic is ample at the declared ceiling — SC1 is a tile layer
// plus a few hundred animated sprites per frame, software-rendered on 1998
// CPUs, and GPU-backed Canvas2D has orders of magnitude more headroom.
//
// The shape:
//   - everything is drawn into an OFFSCREEN canvas at the internal resolution,
//     then blitted once to the visible canvas with nearest-neighbor integer
//     upscale and letterbox bars. A wider window must never reveal more map —
//     under lockstep that is a fairness rule, not taste.
//   - a "sheet" is an atlas built once from palette-index pixel data. Player
//     colours would be pre-baked recolored variants of the same data: an
//     asset-pipeline product, not a renderer op, which is how the op set stays
//     at a dozen.
//   - `shade` is the one composite op, and it is `multiply` — dithered overlay
//     tiles multiplied over the scene is fog of war; alpha on `spr` is
//     cloaking. Neither needs a shader.
//
// Input listeners live here and nowhere else: a cart polls, and the DOM never
// reaches it (§12).

import { DRAW_OPS, AUDIO_OPS, PALETTE, DEFAULT_RESOLUTION, KEYS,
         GLYPH_W, letterbox, toInternal } from './spec.mjs';
import { glyph } from './font.mjs';

/** Browser key codes → the frozen named set. A raw code never reaches a cart. */
const KEYMAP = (() => {
  const m = { ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down',
              Space: 'space', Enter: 'enter', Escape: 'escape', Tab: 'tab',
              ShiftLeft: 'shift', ShiftRight: 'shift', ControlLeft: 'ctrl',
              ControlRight: 'ctrl', AltLeft: 'alt', AltRight: 'alt' };
  for (const k of KEYS) {
    if (k.length === 1 && k >= 'a' && k <= 'z') m['Key' + k.toUpperCase()] = k;
    if (k.length === 1 && k >= '0' && k <= '9') { m['Digit' + k] = k; m['Numpad' + k] = k; }
  }
  return m;
})();

/**
 * @param {HTMLCanvasElement} canvas  the visible canvas
 * @param {object} [opt]
 * @param {[number,number]} [opt.resolution]  one of spec.RESOLUTIONS
 */
export function createCanvasBackend(canvas, { resolution = DEFAULT_RESOLUTION } = {}) {
  const [W, H] = resolution;

  const buf = document.createElement('canvas');
  buf.width = W; buf.height = H;
  const g = buf.getContext('2d');
  g.imageSmoothingEnabled = false;

  const out = canvas.getContext('2d');
  out.imageSmoothingEnabled = false;

  const sheets = new Map();
  let clipped = false;
  let box = letterbox(canvas.width, canvas.height, W, H);

  // ---- assets: palette-index pixel data → an atlas canvas -----------------
  //
  // def = { tile: 8, cols: 4, pixels: ['0011', ...] } — one char per pixel, a
  // hex palette index or '.' for transparent.
  function defineSheet(name, def) {
    const rows = def.pixels;
    const w = rows[0].length, h = rows.length;
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    const cg = c.getContext('2d');
    const img = cg.createImageData(w, h);
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const ch = rows[y][x];
        const o = (y * w + x) * 4;
        if (ch === '.' || ch === undefined) { img.data[o + 3] = 0; continue; }
        const hex = PALETTE[parseInt(ch, 16)];
        img.data[o]     = parseInt(hex.slice(1, 3), 16);
        img.data[o + 1] = parseInt(hex.slice(3, 5), 16);
        img.data[o + 2] = parseInt(hex.slice(5, 7), 16);
        img.data[o + 3] = 255;
      }
    }
    cg.putImageData(img, 0, 0);
    sheets.set(name, { canvas: c, tile: def.tile, cols: def.cols ?? Math.floor(w / def.tile) });
  }

  const sheet = (name) => {
    const s = sheets.get(name);
    if (!s) throw new Error(`unknown sheet '${name}'`);
    return s;
  };

  const blit = (name, i, x, y, { flipX = false, flipY = false, alpha = 1 } = {}) => {
    const s = sheet(name);
    const sx = (i % s.cols) * s.tile, sy = Math.floor(i / s.cols) * s.tile;
    const t = s.tile;
    if (alpha !== 1) g.globalAlpha = Math.max(0, Math.min(1, alpha));
    if (flipX || flipY) {
      g.save();
      g.translate(x + (flipX ? t : 0), y + (flipY ? t : 0));
      g.scale(flipX ? -1 : 1, flipY ? -1 : 1);
      g.drawImage(s.canvas, sx, sy, t, t, 0, 0, t, t);
      g.restore();
    } else {
      g.drawImage(s.canvas, sx, sy, t, t, x, y, t, t);
    }
    if (alpha !== 1) g.globalAlpha = 1;
  };

  const be = {
    width: W, height: H,
    defineSheet, sheet,

    beginFrame() {
      if (clipped) { g.restore(); clipped = false; }
      g.setTransform(1, 0, 0, 1, 0, 0);
    },

    // ---- the frozen draw ops --------------------------------------------
    cls(c) {
      g.save();
      g.setTransform(1, 0, 0, 1, 0, 0);
      g.fillStyle = PALETTE[c];
      g.fillRect(0, 0, W, H);
      g.restore();
    },
    camera(x, y) { g.setTransform(1, 0, 0, 1, -x, -y); },
    clip(r) {
      if (clipped) { g.restore(); clipped = false; }
      if (!r) return;
      g.save(); g.beginPath(); g.rect(r.x, r.y, r.w, r.h); g.clip();
      clipped = true;
    },
    tile(name, i, x, y)  { blit(name, i, x, y); },
    spr(name, i, x, y, o) { blit(name, i, x, y, o); },
    shade(name, i, x, y) {
      const prev = g.globalCompositeOperation;
      g.globalCompositeOperation = 'multiply';
      blit(name, i, x, y);
      g.globalCompositeOperation = prev;
    },
    print(text, x, y, c) {
      g.fillStyle = PALETTE[c];
      let px = x;
      for (const ch of text) {
        const rows = glyph(ch);
        for (let r = 0; r < 5; r++)
          for (let k = 0; k < 3; k++)
            if (rows[r][k]) g.fillRect(px + k, y + r, 1, 1);
        px += GLYPH_W;
      }
    },
    line(x0, y0, x1, y1, c) {
      // Bresenham rather than ctx.stroke: a stroked 1px line lands on half
      // pixels and antialiases, which at internal resolution is a smear.
      g.fillStyle = PALETTE[c];
      let dx = Math.abs(x1 - x0), dy = -Math.abs(y1 - y0);
      const sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
      let err = dx + dy;
      for (;;) {
        g.fillRect(x0, y0, 1, 1);
        if (x0 === x1 && y0 === y1) break;
        const e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
      }
    },
    rect(x, y, w, h, c) {
      g.fillStyle = PALETTE[c];
      g.fillRect(x, y, w, 1); g.fillRect(x, y + h - 1, w, 1);
      g.fillRect(x, y, 1, h); g.fillRect(x + w - 1, y, 1, h);
    },
    rectfill(x, y, w, h, c) { g.fillStyle = PALETTE[c]; g.fillRect(x, y, w, h); },
    circ(x, y, r, c) {
      g.fillStyle = PALETTE[c];
      let px = r, py = 0, err = 1 - r;
      while (px >= py) {
        for (const [a, b] of [[px, py], [py, px], [-px, py], [-py, px],
                              [px, -py], [py, -px], [-px, -py], [-py, -px]])
          g.fillRect(x + a, y + b, 1, 1);
        py++;
        if (err < 0) { err += 2 * py + 1; }
        else { px--; err += 2 * (py - px) + 1; }
      }
    },
    circfill(x, y, r, c) {
      g.fillStyle = PALETTE[c];
      for (let dy = -r; dy <= r; dy++) {
        const dx = Math.floor(Math.sqrt(r * r - dy * dy));
        g.fillRect(x - dx, y + dy, 2 * dx + 1, 1);
      }
    },

    // ---- audio: write-only, and that is the whole API ---------------------
    //
    // Sources are registered by the page (`defineSound`), because loading is
    // above the line; starting and stopping is below it. A cart can never ask
    // what is playing — every such readback is wall-clock in disguise (I4).
    sounds: new Map(),
    defineSound(id, src) { be.sounds.set(id, src); },
    sound(id, gain) {
      const src = be.sounds.get(id);
      if (!src) return;
      const a = src.cloneNode();
      a.volume = Math.max(0, Math.min(1, gain));
      a.play().catch(() => {});                 // autoplay policy: silence, not a throw
      be.voices.set(id, a);
    },
    voices: new Map(),
    stop(id) { const a = be.voices.get(id); if (a) { a.pause(); be.voices.delete(id); } },
    music(id, gain) {
      be.music_stop();
      const src = be.sounds.get(id);
      if (!src) return;
      const a = src.cloneNode();
      a.loop = true;
      a.volume = Math.max(0, Math.min(1, gain));
      a.play().catch(() => {});
      be.track = a;
    },
    track: null,
    music_stop() { if (be.track) { be.track.pause(); be.track = null; } },

    // ---- input: listeners here, polling out there -------------------------
    raw: { x: 0, y: 0, buttons: [false, false, false], keys: new Set() },
    readInput() { return be.raw; },

    // ---- persistence: the cartdata blob, and nothing else ------------------
    loadCartdata() {
      try {
        const s = globalThis.localStorage?.getItem('infeasible.cartdata');
        return s ? Int32Array.from(JSON.parse(s)) : new Int32Array(64);
      } catch { return new Int32Array(64); }
    },
    saveCartdata(blob) {
      try {
        globalThis.localStorage?.setItem('infeasible.cartdata',
                                         JSON.stringify(Array.from(blob)));
      } catch { /* private mode: settings do not persist, the game still runs */ }
    },

    /** Blit the internal surface to the visible canvas: integer upscale,
     *  letterbox bars, nothing smoothed. */
    present() {
      if (canvas.width !== canvas.clientWidth * devicePixelRatio ||
          canvas.height !== canvas.clientHeight * devicePixelRatio) {
        canvas.width = Math.max(1, Math.floor(canvas.clientWidth * devicePixelRatio));
        canvas.height = Math.max(1, Math.floor(canvas.clientHeight * devicePixelRatio));
        out.imageSmoothingEnabled = false;
      }
      box = letterbox(canvas.width, canvas.height, W, H);
      out.fillStyle = PALETTE[0];
      out.fillRect(0, 0, canvas.width, canvas.height);
      out.drawImage(buf, 0, 0, W, H, box.x, box.y, box.w, box.h);
    },

    /** Attach DOM listeners. Called once by the page; a cart cannot reach it. */
    attach(target = canvas) {
      const move = (e) => {
        const r = canvas.getBoundingClientRect();
        const px = (e.clientX - r.left) * (canvas.width / r.width);
        const py = (e.clientY - r.top) * (canvas.height / r.height);
        const p = toInternal(px, py, box, W, H);
        be.raw = { ...be.raw, x: p.x, y: p.y };
      };
      target.addEventListener('pointermove', move);
      target.addEventListener('pointerdown', (e) => {
        move(e);
        const b = be.raw.buttons.slice(); b[Math.min(2, e.button)] = true;
        be.raw = { ...be.raw, buttons: b };
      });
      const up = (e) => {
        const b = be.raw.buttons.slice(); b[Math.min(2, e.button)] = false;
        be.raw = { ...be.raw, buttons: b };
      };
      target.addEventListener('pointerup', up);
      window.addEventListener('blur', () => {
        be.raw = { ...be.raw, buttons: [false, false, false], keys: new Set() };
      });
      window.addEventListener('keydown', (e) => {
        const k = KEYMAP[e.code];
        if (!k) return;
        e.preventDefault();
        const s = new Set(be.raw.keys); s.add(k);
        be.raw = { ...be.raw, keys: s };
      });
      window.addEventListener('keyup', (e) => {
        const k = KEYMAP[e.code];
        if (!k) return;
        const s = new Set(be.raw.keys); s.delete(k);
        be.raw = { ...be.raw, keys: s };
      });
    },
  };

  for (const op of [...DRAW_OPS, ...AUDIO_OPS])
    if (typeof be[op] !== 'function')
      throw new Error(`canvas backend is missing '${op}'`);
  return be;
}
