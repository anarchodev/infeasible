// save_check.mjs — a save is an action log, and this proves it round-trips.
//
//   node web/save_check.mjs
//
// §12: a save is `(engine-hash, game-hash, action-log)` — not a state dump,
// because state written outside the log is state replay cannot reproduce,
// which forfeits shareable playthroughs, branching and time travel in one
// move. That only means anything if a log actually reconstitutes a world, so:
// play the duel by clicking, save, open a FRESH world, load, and compare every
// fluent the interface artifact declares.
//
// The mechanism is the action SOURCE. `cart.tick()` proposes; the source
// disposes. Live they are the same thing; replaying, the log decides and the
// proposal is discarded — which is also the seat a network source takes for
// lockstep, and why saving needed no new concept, only a seam.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

import { createPlatform } from './platform/platform.mjs';
import { createHeadlessBackend } from './platform/headless.mjs';
import { createRuntime, replay } from './platform/runtime.mjs';
import { pureCart } from './platform/purecart.mjs';
import { open, SORTS, ENUMS, IFACE, STORY, SOURCE_HASH } from './duel_pure.binding.mjs';
import { SPRITES, FOG } from './carts/duel_art.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.js');
const M = await createInfeasible();
const src = readFileSync(new URL('../' + STORY, import.meta.url), 'utf8');

let failed = 0;
const check = (what, cond, extra = '') => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${cond || !extra ? '' : `\n        ${extra}`}`);
  if (!cond) failed++;
};

/** A fresh world, cart and runtime — the shape a load needs. */
function session(source) {
  const world = open(M, src);
  const cart = pureCart({ world, iface: IFACE, doms: { ...SORTS, ...ENUMS },
                          sheets: { main: SPRITES, main_fog: FOG } });
  const backend = createHeadlessBackend({ resolution: cart.resolution });
  const rt = createRuntime({ platform: createPlatform(backend), backend, cart, world,
                             source, identity: { story: STORY, game: SOURCE_HASH } });
  return { world, cart, backend, rt };
}

/** Every declared fluent's value — the whole observable state, not a sample. */
const snapshot = (w) => {
  const out = {};
  const dom = { ...SORTS, ...ENUMS };
  const tuples = (args) => args.reduce(
    (acc, s) => acc.flatMap((t) => (dom[s] ?? []).map((v) => [...t, v])), [[]]);
  for (const f of IFACE.state)
    for (const t of tuples(f.args))
      out[`${f.name}(${t.join(',')})`] = w.state[f.name](...t);
  return out;
};

// ---- play it ---------------------------------------------------------------

const a = session();
const labels = () => a.cart.scene.model().menu.map((b) => b.label).join(' | ');
const click = (label) => {
  const b = a.cart.scene.model().menu.find((x) => x.label === label);
  if (!b) throw new Error(`no command '${label}' — offered: ${labels()}`);
  a.backend.point(b.x + 4, b.y + 4, [true]); a.rt.advance(1);
  a.backend.point(b.x + 4, b.y + 4, []); a.rt.advance(1);
};
const clickEntity = (e) => {
  const o = Object.values(a.cart.scene.model().slots).flat().find((x) => x.e === e);
  a.backend.point(o.at.x + 8, o.at.y + 8, [true]); a.rt.advance(1);
  a.backend.point(o.at.x + 8, o.at.y + 8, []); a.rt.advance(1);
};

a.rt.advance(1);
click('STRIKE EDGE A');
clickEntity('imp');
click('BOLT');
click('END TURN');
click('STRIKE EDGE B');

console.log('a save is the log, and nothing else');
const saved = a.rt.save();
check('it carries what it was played against', saved.story === STORY && saved.game === SOURCE_HASH);
check('and one entry per committed tick — no state dump',
      Array.isArray(saved.log) && saved.log.length === 5 &&
      saved.log.every((t) => Array.isArray(t)),
      JSON.stringify(saved.log));
check('the aiming click is in it too, which is the cost of per-viewer state '
      + 'living in the world (#214)',
      saved.log.some((t) => t[0].startsWith('aim_')), JSON.stringify(saved.log));

console.log('\nloading it into a fresh world');
const b = session();
const n = b.rt.load(saved);
check('the load takes the log', n === saved.log.length);
while (b.rt.replaying()) b.rt.advance(1);

const before = snapshot(a.world), after = snapshot(b.world);
const differ = Object.keys(before).filter((k) => before[k] !== after[k]);
check('every declared fluent agrees — the whole state, not a sample',
      differ.length === 0, differ.map((k) => `${k}: ${before[k]} vs ${after[k]}`).join(', '));
// one strike on the gnoll, a bolt and a second strike on the imp (clamped at
// its floor), and one round of both foes hitting back
check('...and that is a real state, not two empty ones',
      after['hp(gnoll)'] === 8 && after['hp(imp)'] === 0 && after['hp(you)'] === 14,
      JSON.stringify({ gnoll: after['hp(gnoll)'], imp: after['hp(imp)'], you: after['hp(you)'] }));
check('the loaded world can be played on',
      b.cart.scene.model().menu.length > 0, labels());

console.log('\nwhat a save refuses');
{
  const c = session();
  let threw = '';
  try { c.rt.load({ ...saved, game: 'deadbeef', story: STORY }); }
  catch (e) { threw = e.message; }
  check('a save from a different version of the story', threw.includes('different version'), threw);
  c.world.close();
}
{
  const d = session();
  d.rt.advance(1);
  d.rt.ctx.log.push(['end_turn']);
  let threw = '';
  try { d.rt.load(saved); } catch (e) { threw = e.message; }
  check('...and loading into a world that already has a history',
        threw.includes('fresh world'), threw);
  d.world.close();
}

a.world.close(); b.world.close();
console.log(failed ? `\nsave_check: ${failed} FAILED` : '\nsave_check: all passed');
process.exit(failed ? 1 : 0);
