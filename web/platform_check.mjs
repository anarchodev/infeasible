// platform_check.mjs — the presentation interface and the cellar cart, played
// to the end without a browser.
//
//   node web/platform_check.mjs
//
// Two things are pinned here, and they are different in kind.
//
// THE FREEZE. §12's presentation interface is only frozen if something fails
// when it grows. The op lists live in `platform/spec.mjs`; this file asserts
// that the cart-facing surfaces expose exactly those names and that every
// backend implements exactly those names. Adding a thirteenth draw op now
// breaks the build until it is added to the spec deliberately, and every
// backend grows it too — which is the whole content of the word "frozen".
//
// THE CART. The real cart, on the real world, driven by real clicks through
// the polled input surface, played from the locked door to the antidote. What
// is asserted is what the player would see: a greyed button, the `why?` behind
// it, the fog lifting when the torch is picked up, the receipt for the hp the
// door cost. A headless backend makes a frame a LIST OF OPS, so "what did the
// cart draw?" has a data answer.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

import { DRAW_OPS, AUDIO_OPS, INPUT_OPS, DATA_OPS, RESOLUTIONS, PALETTE,
         letterbox, toInternal } from './platform/spec.mjs';
import { createPlatform } from './platform/platform.mjs';
import { createHeadlessBackend } from './platform/headless.mjs';
import { createRuntime } from './platform/runtime.mjs';
import { cart, open, STORY } from './carts/cellar.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.js');

let failed = 0;
const check = (what, cond, extra = '') => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${cond || !extra ? '' : `\n        ${extra}`}`);
  if (!cond) failed++;
};
const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

// ---- the freeze -------------------------------------------------------------

console.log('the frozen interface (§12)');
check('a dozen draw ops, no more', DRAW_OPS.length === 12, `got ${DRAW_OPS.length}`);
check('every resolution is a 1080 divisor',
      RESOLUTIONS.every(([w, h]) => 1920 % w === 0 && 1080 % h === 0));
check('every resolution is 16:9', RESOLUTIONS.every(([w, h]) => w * 9 === h * 16));
check('the palette is sixteen entries', PALETTE.length === 16);
check('a wider window reveals no more map',
      letterbox(1920, 1080, 320, 180).scale === 6 &&
      letterbox(1000, 1080, 320, 180).scale === 3);
check('the upscale is integer and centred',
      JSON.stringify(letterbox(700, 400, 320, 180)) ===
      JSON.stringify({ scale: 2, w: 640, h: 360, x: 30, y: 20 }));
{
  const box = letterbox(700, 400, 320, 180);
  const p = toInternal(30 + 2 * 17, 20 + 2 * 5, box, 320, 180);
  check('the letterbox inverse lands on the pixel under the cursor',
        p.x === 17 && p.y === 5, JSON.stringify(p));
  const off = toInternal(0, 0, box, 320, 180);
  check('a click on the bar clamps to the surface', off.x === 0 && off.y === 0);
}

// ---- the world, the platform, the cart --------------------------------------

const M = await createInfeasible();
const src = readFileSync(new URL('../' + STORY, import.meta.url), 'utf8');
const world = open(M, src);

const backend = createHeadlessBackend({ resolution: cart.resolution });
const platform = createPlatform(backend);
const rt = createRuntime({ platform, backend, cart, world });

console.log('\nthe cart surface');
// `glyphW`/`glyphH`/`textWidth` are the frozen text CELL, not ops: derived
// constants a cart lays text out against, which is why they are not a
// thirteenth call for a backend to implement.
const METRICS = ['glyphW', 'glyphH', 'textWidth'];
check('draw exposes exactly the frozen ops, plus the text metric',
      same(Object.keys(platform.cart.draw).filter((k) => !METRICS.includes(k)), DRAW_OPS),
      Object.keys(platform.cart.draw).join(','));
check('audio exposes exactly the frozen ops',
      same(Object.keys(platform.cart.audio), AUDIO_OPS));
check('input exposes exactly the frozen ops',
      same(Object.keys(platform.cart.input), INPUT_OPS));
check('persistence is one small blob and nothing else',
      same(Object.keys(platform.cart.data), DATA_OPS));
check('a cart cannot resample input mid-tick',
      platform.cart.input.sample === undefined && platform.cart.input._sample === undefined);
check('a colour is a palette index, not a CSS string', (() => {
  try { platform.cart.draw.rectfill(0, 0, 1, 1, '#fff'); return false; }
  catch { return true; }
})());
check('a key is a frozen name, not a browser code', (() => {
  try { platform.cart.input.key('KeyW'); return false; } catch { return true; }
})());
// The Canvas2D backend gets the same op-coverage check — its own constructor
// throws on a missing op — plus one frame through a recording 2D-context stub.
// The stub is not a renderer test (nothing here can tell whether the picture is
// right); it is a CRASH test, which is the failure a browserless suite can
// actually catch and the one that would otherwise reach a player first.
{
  const g = stubDom();
  const { createCanvasBackend } = await import('./platform/canvas2d.mjs');
  let ok = true, err = '';
  try {
    const cbe = createCanvasBackend(g.canvas, { resolution: cart.resolution });
    for (const [n, def] of Object.entries(cart.sheets)) cbe.defineSheet(n, def);
    const cplat = createPlatform(cbe);
    const crt = createRuntime({ platform: cplat, backend: cbe, cart: { ...cart }, world });
    crt.render();
    cbe.present();
  } catch (e) { ok = false; err = e.stack ?? e.message; }
  check('the Canvas2D backend renders a frame without throwing', ok, err);
  check('...and actually put pixels down', g.calls.fillRect > 0 && g.calls.drawImage > 0,
        JSON.stringify(g.calls));
}

// ---- playing it -------------------------------------------------------------

/** Click the command with this label — through the polled input surface, at
 *  the pixel the cart actually drew the button on. Press and release are two
 *  ticks because an edge needs two samples, which is the point of `pressed`. */
function click(label) {
  const b = cart.buttons.find((x) => x.label === label);
  if (!b) throw new Error(`no command '${label}' — offered: ` +
                          cart.buttons.map((x) => x.label).join(', '));
  backend.point(b.x + 4, b.y + 4, [true]);
  rt.advance(1);
  backend.point(b.x + 4, b.y + 4, []);
  rt.advance(1);
}
const clickAt = (x, y) => {
  backend.point(x, y, [true]); rt.advance(1);
  backend.point(x, y, []); rt.advance(1);
};
const offered = () => cart.buttons.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | ');

rt.advance(1);                            // one tick to draw the opening frame

console.log('\nthe opening state');
check('the hero starts poisoned in the cellar',
      world.state.at('hero') === 'cellar' && world.state.poisoned('hero'));
check('the cart drew the map with the tile op', backend.ops('tile').length > 0);
check('the cellar is dark, drawn with THE composite op',
      backend.ops('shade').length > 0);
check('the commands are the ones the world allows',
      offered() === 'GO TO HALL | TAKE RUSTY KEY | TAKE TORCH', offered());

console.log('\nwhat a refused command says');
click('GO TO HALL');
check('the hero walked', world.state.at('hero') === 'hall');
check('the step emitted a footstep, and the cart played it',
      backend.played.some(([k, id]) => k === 'sound' && id === 'footstep'));
check('forcing the door is offered but refused',
      cart.buttons.some((b) => b.label === 'FORCE DOOR' && !b.ok), offered());
click('FORCE DOOR');
check('the door did not move', world.state.door() === 'locked');
check('clicking it printed the argument that refused it',
      backend.text().includes('too_weak'), backend.text());
check('...naming the exception that beat the norm',
      backend.text().includes('can_force') && backend.text().includes('weakened(hero)'));

console.log('\nthe puzzle');
click('GO TO CELLAR');
click('TAKE TORCH');
check('the hero is holding the torch', world.state.holding('hero', 'torch'));
rt.advance(1);
check('the fog lifted — a judgment the renderer reads',
      world.q.in_dark('hero') === 'refuted' && backend.ops('shade').length === 0);
click('TAKE RUSTY KEY');
click('GO TO HALL');
click('UNLOCK DOOR');
check('the lock turned, but the door is jammed', world.state.door() === 'jammed');
check('the cue said so', backend.played.some(([k, id]) => k === 'sound' && id === 'clunk'));
check('the hero still cannot force it — poison, not the lock',
      world.q.can_force_door('hero') === 'refuted');

// the guard: unpoisoned, already in the hall, and the one who can shoulder it
clickAt(...(() => { const r = cart.actorRect(world, 'guard'); return [r.x + 4, r.y + 4]; })());
check('clicking an actor selects them', cart.sel === 'guard', cart.sel);
check('the guard can force what the hero cannot',
      cart.buttons.some((b) => b.label === 'FORCE DOOR' && b.ok), offered());
click('FORCE DOOR');
check('the way is open', world.state.door() === 'open');
check('it cost the guard two hit points', world.state.hp('guard') === 10);
check('the receipt says where they went, in the author\'s terms',
      cart.note.includes('12 -> 10') && cart.note.includes('force_door'), cart.note);
check('and the subscription narrated the door, not the hit points',
      cart.event === 'the vault door swings open', cart.event);

// back to the hero, and through
backend.press('tab'); rt.advance(1); backend.press(); rt.advance(1);
check('tab switches actors', cart.sel === 'hero');
click('ENTER VAULT');
check('the hero is in the vault', world.state.at('hero') === 'vault');
click('TAKE ANTIDOTE');
click('DRINK ANTIDOTE');
check('the poison is gone', !world.state.poisoned('hero'));
check('and the judgment standing on it followed',
      world.q.weakened('hero') === 'refuted');
check('the cart never had to know any of that',
      cart.buttons.every((b) => b.label !== 'DRINK ANTIDOTE'), offered());

console.log('\nthe log, and the streams');
check('every committed tick is one entry in the action log',
      rt.ctx.log.length === 10, `${rt.ctx.log.length}: ${rt.ctx.log.map((l) => l[0]).join(' ')}`);
check('a save is that log — no other state was written',
      rt.ctx.log.every((entry) => entry.length === 1));
check('cues are transient: the last tick emitted, this one does not',
      (() => { rt.advance(1); return rt.ctx.step.emits.length === 0; })());
check('cartdata survives a write and is not part of the log', (() => {
  platform.cart.data.set(3, 42);
  return platform.cart.data.get(3) === 42 && rt.ctx.log.length === 10;
})());

console.log('\nlight belongs to the room, not to the carrier');
{
  // A fresh session, driven straight through the binding: the cart is not the
  // subject here, the STORY is. Darkness written as "am I holding the torch"
  // leaves you in the dark beside someone carrying a lit one, which is a
  // modelling bug no amount of renderer work can fix.
  const w2 = open(M, src);
  check('the hero starts in the dark, alone in the cellar',
        w2.q.in_dark('hero') === 'proved');
  w2.step(w2.actions().add(w2.a.go_cellar('guard')));
  w2.step(w2.actions().add(w2.a.take('guard', 'torch', 'cellar')));
  check('a torch carried by SOMEONE ELSE lights the room you share',
        w2.q.in_dark('hero') === 'refuted', w2.q.in_dark('hero'));
  w2.step(w2.actions().add(w2.a.go_hall('guard')));
  check('and it goes with them when they leave',
        w2.q.in_dark('hero') === 'proved', w2.q.in_dark('hero'));
  w2.close();
}

console.log('\nthe two clients meet on one save');
{
  // `examples/cellar_play.log` is the fixture `tests/test_secondclient.c`
  // replays through `world_*` alone. The cart reached it by CLICKING; the C
  // client has to land in the same world from the same lines. Neither client
  // can be quietly special, because the save is the only thing they share.
  const text = readFileSync(new URL('../examples/cellar_play.log', import.meta.url), 'utf8');
  const lines = text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('#'));
  const named = lines.shift();
  check('the save names the world it was played against', named === `story ${STORY}`, named);
  const played = rt.ctx.log.map((t) => t.join(' '));
  check('the log the cart produced by clicking IS that fixture', same(lines, played),
        `file:   ${lines.join(' / ')}\n        played: ${played.join(' / ')}`);
}

world.close();
console.log(failed ? `\nplatform_check: ${failed} FAILED` : '\nplatform_check: all passed');
process.exit(failed ? 1 : 0);


// ---- a recording stand-in for the two DOM objects the renderer touches ------
//
// Deliberately dumb: a 2D context that counts calls and an ImageData that is a
// plain array. Enough for the backend to run end to end, and small enough that
// it cannot quietly become the thing under test.
function stubDom() {
  const calls = { fillRect: 0, drawImage: 0, putImageData: 0 };
  const ctx = () => ({
    imageSmoothingEnabled: false, globalAlpha: 1, globalCompositeOperation: 'source-over',
    fillStyle: '#000',
    fillRect: () => { calls.fillRect++; },
    drawImage: () => { calls.drawImage++; },
    putImageData: () => { calls.putImageData++; },
    createImageData: (w, h) => ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h }),
    save() {}, restore() {}, translate() {}, scale() {}, setTransform() {},
    beginPath() {}, rect() {}, clip() {},
  });
  const canvas = { width: 960, height: 540, clientWidth: 960, clientHeight: 540,
                   getContext: ctx, getBoundingClientRect: () => ({ left: 0, top: 0, width: 960, height: 540 }) };
  globalThis.devicePixelRatio = 1;
  globalThis.document = { createElement: () => ({ width: 0, height: 0, getContext: ctx }) };
  return { canvas, calls };
}
