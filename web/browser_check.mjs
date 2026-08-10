// browser_check.mjs — the cart in a real browser, driven by real mouse events.
//
//   node web/browser_check.mjs [--shots <dir>]
//
// `platform_check.mjs` plays the same cart headlessly and asserts on the op
// list, which pins what the cart DECIDES. It cannot pin what a browser DOES
// with those ops: whether the module graph loads, whether the WASM module is
// served with a MIME type Chrome will execute, whether a pointer event
// survives the letterbox inverse, whether anything is on screen at all. Those
// failures are invisible to a headless suite and land on a player first.
//
// So this drives Chromium over the DevTools protocol — no puppeteer, no
// dependency: Node's built-in WebSocket, a raw JSON-RPC channel, and the
// browser's own /json endpoint. It starts its own static server (from the repo
// root, because the page fetches the .story source) and cleans up after itself.
//
// It SKIPS, not fails, when no Chromium is on the box: the browser is not a
// build dependency and this is not part of ctest. Point it at one with
// CHROMIUM=/path/to/chromium if it cannot find yours.
//
// The trick that makes clicking possible without touching the product: ES
// modules are singletons per realm, so `import('/web/carts/cellar.mjs')` inside
// the page hands back the SAME module instance the running game is using.
// `cart.buttons` is therefore the live command list, in internal pixels, and
// mapping those through the frozen letterbox gives a real screen coordinate to
// press. No debug hook, no test-only branch in the cart.

import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { letterbox, RESOLUTIONS } from './platform/spec.mjs';

const ROOT = new URL('..', import.meta.url).pathname;
const PORT = 8099;

const shotsAt = process.argv.indexOf('--shots');
const SHOTS = shotsAt > 0 ? process.argv[shotsAt + 1] : null;
if (SHOTS) mkdirSync(SHOTS, { recursive: true });

let failed = 0;
const check = (what, cond, extra = '') => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${cond || !extra ? '' : `\n        ${extra}`}`);
  if (!cond) failed++;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- find a browser, or skip -----------------------------------------------

function findChromium() {
  if (process.env.CHROMIUM) return process.env.CHROMIUM;
  const candidates = ['chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable'];
  for (const c of candidates) {
    for (const dir of (process.env.PATH ?? '').split(':')) {
      const p = join(dir, c);
      // /usr/bin/chromium-browser is a snap stub on Ubuntu that only prints
      // instructions, so a path hit is not proof; the launch below is.
      if (existsSync(p)) return p;
    }
  }
  return null;
}

const CHROME = findChromium();
if (!CHROME) {
  console.log('browser_check: skipped — no chromium found (set CHROMIUM=/path/to/chromium)');
  process.exit(0);
}

// ---- a static server, rooted where the page expects -------------------------

const server = spawn('python3', ['-m', 'http.server', String(PORT), '--bind', '127.0.0.1'],
                     { cwd: ROOT, stdio: 'ignore' });
const profile = mkdtempSync(join(tmpdir(), 'infeasible-cr-'));
let browser = null;
const cleanup = () => {
  try { browser?.kill(); } catch { /* already gone */ }
  try { server.kill(); } catch { /* already gone */ }
  try { rmSync(profile, { recursive: true, force: true }); } catch { /* fine */ }
};
process.on('exit', cleanup);

await sleep(600);

// ---- launch, and find the DevTools endpoint --------------------------------

browser = spawn(CHROME, [
  '--headless=new', '--disable-gpu', '--no-sandbox', '--hide-scrollbars',
  // headless invents a 800x600 "display", which makes a fullscreen surface
  // nothing like a real one
  '--screen-info={1920x1080}',
  '--window-size=1920,1080', '--remote-debugging-port=0',
  `--user-data-dir=${profile}`, 'about:blank',
], { stdio: ['ignore', 'ignore', 'pipe'] });

let launchErr = '';
browser.stderr.on('data', (b) => { launchErr += b.toString(); });

let devtoolsPort = null;
for (let i = 0; i < 60 && devtoolsPort === null; i++) {
  await sleep(150);
  const f = join(profile, 'DevToolsActivePort');
  if (existsSync(f)) devtoolsPort = readFileSync(f, 'utf8').split('\n')[0].trim();
}
if (!devtoolsPort) {
  console.log('browser_check: skipped — chromium would not start\n' +
              launchErr.split('\n').slice(0, 3).map((l) => '  ' + l).join('\n'));
  process.exit(0);
}

const targets = await (await fetch(`http://127.0.0.1:${devtoolsPort}/json/list`)).json();
const page = targets.find((t) => t.type === 'page');

// ---- a minimal CDP client ---------------------------------------------------

const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

let nextId = 1;
const pending = new Map();
const problems = [];          // anything the page complained about, ever
let loaded = false;

ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) {
    const { res, rej } = pending.get(m.id);
    pending.delete(m.id);
    m.error ? rej(new Error(m.error.message)) : res(m.result);
    return;
  }
  if (m.method === 'Page.loadEventFired') loaded = true;
  if (m.method === 'Runtime.exceptionThrown')
    problems.push('exception: ' +
      (m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text));
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error')
    problems.push(`log: ${m.params.entry.text} <${m.params.entry.url ?? '?'}>`);
  if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error')
    problems.push('console: ' + m.params.args.map((a) => a.value ?? a.description).join(' '));
};

const send = (method, params = {}) => new Promise((res, rej) => {
  const id = nextId++;
  pending.set(id, { res, rej });
  ws.send(JSON.stringify({ id, method, params }));
});

const evaluate = async (expr) => {
  const r = await send('Runtime.evaluate',
                       { expression: expr, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails)
    throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
  return r.result.value;
};

await send('Page.enable');
await send('Runtime.enable');
await send('Log.enable');

// ---- load ------------------------------------------------------------------

console.log(`the cart in a browser (${page.title ? '' : ''}${CHROME.split('/').pop()})`);

await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/web/` });
for (let i = 0; i < 60 && !loaded; i++) await sleep(100);
await sleep(1200);                       // the WASM module compiles, then ticks

check('the page loads with nothing thrown', problems.length === 0,
      problems.slice(0, 4).join('\n        '));
check('the page reports no startup error of its own',
      (await evaluate(`document.getElementById('err').textContent`)) === '',
      await evaluate(`document.getElementById('err').textContent`));

// ---- is anything actually on screen? ---------------------------------------
//
// A canvas that throws is caught above; a canvas that is uniformly black is
// not, and is exactly what a silently-failed asset or a wrong transform looks
// like. Count distinct colours in the backing store.
const paint = await evaluate(`(() => {
  const c = document.getElementById('screen');
  const g = c.getContext('2d');
  const d = g.getImageData(0, 0, c.width, c.height).data;
  const seen = new Set();
  for (let i = 0; i < d.length; i += 4)
    seen.add((d[i] << 16) | (d[i + 1] << 8) | d[i + 2]);
  return { colours: seen.size, w: c.width, h: c.height };
})()`);
check('the canvas has a picture on it, not one flat colour', paint.colours > 8,
      JSON.stringify(paint));

// ---- geometry: internal pixels -> screen ------------------------------------

// The internal resolution is the CART's choice within the frozen set, so it is
// read from the running cart rather than assumed here — hardcoding it makes
// every click land somewhere else the day a game picks a different one.
const INTERNAL = await evaluate(
  `(async () => (await import('/web/carts/cellar.mjs')).cart.resolution)()`);

const geom = await evaluate(`(() => {
  const c = document.getElementById('screen');
  const r = c.getBoundingClientRect();
  return { left: r.left, top: r.top, cssW: r.width, bufW: c.width, bufH: c.height,
           dpr: devicePixelRatio };
})()`);
const box = letterbox(geom.bufW, geom.bufH, INTERNAL[0], INTERNAL[1]);
const perBuf = geom.cssW / geom.bufW;         // buffer px -> CSS px
const toScreen = (ix, iy) => ({
  x: geom.left + (box.x + ix * box.scale) * perBuf,
  y: geom.top + (box.y + iy * box.scale) * perBuf,
});

// Integer and inside the buffer is the frozen rule; ×1 is a legitimate scale
// for a game that picked a large internal resolution on a small window, so
// asserting ×2 here would be asserting a window size.
check('the frozen upscale is an integer that fits the surface',
      Number.isInteger(box.scale) && box.scale >= 1 &&
      box.w <= geom.bufW && box.h <= geom.bufH && box.x >= 0 && box.y >= 0,
      JSON.stringify({ ...box, buf: [geom.bufW, geom.bufH] }));

// ---- the live cart, through the module registry -----------------------------

const cartState = () => evaluate(`(async () => {
  const m = await import('/web/carts/cellar.mjs');
  return { sel: m.cart.sel, note: m.cart.note, event: m.cart.event,
           why: m.cart.why.split('\\n').slice(0, 3).join(' | '),
           buttons: m.cart.buttons.map((b) => ({ label: b.label, ok: b.ok,
                                                 x: b.x, y: b.y, w: b.w, h: b.h })) };
})()`);

let st = await cartState();
check('the running game is reachable as a module singleton',
      Array.isArray(st.buttons) && st.buttons.length > 0, JSON.stringify(st).slice(0, 160));
check('it opens on the commands the world allows',
      st.buttons.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | ') ===
        'GO TO HALL | TAKE RUSTY KEY*',
      st.buttons.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | '));

// ---- real mouse events ------------------------------------------------------
//
// The press must OUTLAST a tick: input is sampled once per tick at the tick
// boundary (§12), so a press and release inside one 50ms tick is a press the
// game never observes. That is the frozen contract, not a bug — a driver has
// to hold the button like a hand does.
async function clickInternal(ix, iy, holdMs = 150) {
  const p = toScreen(ix, iy);
  const base = { x: Math.round(p.x), y: Math.round(p.y), button: 'left',
                 buttons: 1, clickCount: 1 };
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...base, buttons: 0 });
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', ...base });
  await sleep(holdMs);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...base, buttons: 0 });
  await sleep(200);
}

async function command(label, holdMs) {
  st = await cartState();
  const b = st.buttons.find((x) => x.label === label);
  if (!b) throw new Error(`no command '${label}' — offered: ` +
                          st.buttons.map((x) => x.label).join(', '));
  await clickInternal(b.x + 4, b.y + 4, holdMs);
  st = await cartState();
  return b;
}

const shot = async (name) => {
  if (!SHOTS) return;
  const r = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(join(SHOTS, `${name}.png`), Buffer.from(r.data, 'base64'));
};

await shot('01-open');

console.log('\nplaying it with the mouse');
await command('GO TO HALL');
check('a click moved the hero', st.buttons.some((b) => b.label === 'GO TO CELLAR'),
      st.buttons.map((b) => b.label).join(' | '));
check('forcing the door is offered but greyed',
      st.buttons.some((b) => b.label === 'FORCE DOOR' && !b.ok));

await command('FORCE DOOR');
check('clicking the greyed command printed the argument that refused it',
      st.why.includes('too_weak') || st.why.includes('can_force_door'), st.why);
await shot('02-why');

// A FAST click: 15ms, well inside one 50ms tick, so the press and the release
// both land between two samples. The backend latches a press until the next
// sample consumes it — without that, this click is one the game never sees,
// and a real mouse is often quicker than a tick.
await command('TAKE TORCH', 15);
check('a click shorter than a tick is not dropped',
      st.buttons.some((b) => b.label === 'DROP TORCH'),
      st.buttons.map((b) => b.label).join(' | '));
await command('GO TO CELLAR');
check('the fetched torch lit the cellar',
      st.buttons.some((b) => b.label === 'TAKE RUSTY KEY' && b.ok),
      st.buttons.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | '));
await command('TAKE RUSTY KEY');
await command('GO TO HALL');
await command('UNLOCK DOOR');
check('the lock turned, and the cue said so', st.event.includes('lock'), st.event);
await shot('03-unlocked');

// Select the guard by clicking the SPRITE, not a button — the other input
// path, and the one that exercises the letterbox inverse on an arbitrary
// point rather than a wide button. The hall's second actor slot, in internal
// pixels; the cart's own hit test decides whether the click landed.
await clickInternal(296, 72);   // hall, second actor slot
st = await cartState();
check('clicking an actor selects them', st.sel === 'guard', st.sel);
check('the guard can force what the hero cannot',
      st.buttons.some((b) => b.label === 'FORCE DOOR' && b.ok),
      st.buttons.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | '));

await command('FORCE DOOR');
check('the receipt rendered the cost in the author\'s terms',
      st.note.includes('12 -> 10'), st.note);
check('and the subscription narrated the door', st.event.includes('door'), st.event);
await shot('04-forced');

// tab back to the hero — the keyboard path, and a TAP rather than a hold: a
// key has the same problem a button does, and 15ms is well inside a tick.
await send('Input.dispatchKeyEvent', { type: 'keyDown', code: 'Tab', key: 'Tab',
                                       windowsVirtualKeyCode: 9 });
await sleep(15);
await send('Input.dispatchKeyEvent', { type: 'keyUp', code: 'Tab', key: 'Tab',
                                       windowsVirtualKeyCode: 9 });
await sleep(200);
st = await cartState();
check('a tab tap shorter than a tick switches actors', st.sel === 'hero', st.sel);

await command('ENTER VAULT');
await command('TAKE ANTIDOTE');
await command('DRINK ANTIDOTE');
check('the antidote is drunk, so the command is gone',
      !st.buttons.some((b) => b.label === 'DRINK ANTIDOTE'),
      st.buttons.map((b) => b.label).join(' | '));
await shot('05-solved');

// ---- fullscreen: the case the resolution set exists for ---------------------
//
// Requires a user gesture, so it is driven the way a player drives it — a real
// click on the page's own control. What is being checked is the ARITHMETIC: at
// a 1920x1080 surface with no chrome in the way, every blessed internal
// resolution is a whole-number multiple, so the picture fills the display with
// no letterbox bars at all.
{
  const btn = await evaluate(`(() => {
    const r = document.getElementById('full').getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  })()`);
  const at = { x: Math.round(btn.x), y: Math.round(btn.y), button: 'left',
               buttons: 1, clickCount: 1 };
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', ...at, buttons: 0 });
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', ...at });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...at, buttons: 0 });
  await sleep(700);

  const fs = await evaluate(`(() => {
    const c = document.getElementById('screen');
    return { on: !!document.fullscreenElement, bufW: c.width, bufH: c.height,
             vw: innerWidth, vh: innerHeight,
             cw: c.clientWidth, ch: c.clientHeight, dpr: devicePixelRatio };
  })()`);
  check('the page can go fullscreen', fs.on, JSON.stringify(fs));
  if (fs.on) {
    // What the PAGE is responsible for: in fullscreen every pixel of the
    // viewport belongs to the canvas. Chrome around it does not merely waste
    // room, it costs integer scale — 46px of caption is the difference between
    // x2 and x1 at 720p.
    check('fullscreen gives the canvas the whole viewport',
          fs.cw === fs.vw && fs.ch === fs.vh,
          `canvas ${fs.cw}x${fs.ch} in viewport ${fs.vw}x${fs.vh}`);
    // What the RESOLUTION SET is responsible for, checked as the arithmetic it
    // is: headless reports a 1920x1080 screen but hands fullscreen a slightly
    // shorter window, so asserting exactness against this viewport would be
    // asserting a headless quirk rather than the contract.
    const bars = RESOLUTIONS.filter(([iw, ih]) => {
      const b = letterbox(1920, 1080, iw, ih);
      return b.w !== 1920 || b.h !== 1080;
    });
    check('and a 1920x1080 surface is exact for every blessed resolution',
          bars.length === 0, `letterboxed: ${bars.map((r) => r.join('x')).join(', ')}`);
    await shot('06-fullscreen');
    await send('Runtime.evaluate', { expression: 'document.exitFullscreen()' });
    await sleep(500);
  }
}

check('nothing threw during the whole playthrough', problems.length === 0,
      problems.slice(0, 4).join('\n        '));

if (SHOTS) console.log(`\nscreenshots in ${SHOTS}`);
console.log(failed ? `\nbrowser_check: ${failed} FAILED` : '\nbrowser_check: all passed');
cleanup();
process.exit(failed ? 1 : 0);
