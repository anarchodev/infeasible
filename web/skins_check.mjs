// skins_check.mjs — two games, one renderer, and the intersection between them.
//
//   node web/skins_check.mjs
//
// `pure_check.mjs` shows that a cart can be `.story` and assets. It cannot say
// which of that vocabulary is a PRIMITIVE and which was one game's furniture,
// because one game never can. This plays a second, deliberately chosen to be as
// far from the first as a game can be and still be drawn by the same
// `platform/scene.mjs`:
//
//   cellar_pure            duel_pure
//   -------------------    ------------------------------------------------
//   space is containment   no space at all — things live in zones
//   one actor acts         you act ON a target: subject and object
//   numbers are scenery    numbers are the game
//   the menu is verbs      the menu is a hand, and it changes every turn
//
// Then it MEASURES the overlap rather than asserting it: which blessed
// predicates both stories concluded, which only one did, and what neither
// could say. The middle column is one game's shape; the last is the language's
// missing feature list, and an item on it demanded by two independent games is
// no longer a matter of taste.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

import { createPlatform } from './platform/platform.mjs';
import { createHeadlessBackend } from './platform/headless.mjs';
import { createRuntime } from './platform/runtime.mjs';
import { pureCart } from './platform/purecart.mjs';
import * as duel from './duel_pure.binding.mjs';
import * as cellar from './cellar_pure.binding.mjs';
import { SPRITES, FOG } from './carts/duel_art.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.js');

let failed = 0;
const check = (what, cond, extra = '') => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${cond || !extra ? '' : `\n        ${extra}`}`);
  if (!cond) failed++;
};

const M = await createInfeasible();
const src = readFileSync(new URL('../' + duel.STORY, import.meta.url), 'utf8');
const world = duel.open(M, src);

const cart = pureCart({ world, iface: duel.IFACE,
                        doms: { ...duel.SORTS, ...duel.ENUMS },
                        sheets: { main: SPRITES, main_fog: FOG } });
const backend = createHeadlessBackend({ resolution: cart.resolution });
const rt = createRuntime({ platform: createPlatform(backend), backend, cart, world });
rt.advance(1);

const scene = cart.scene;
const labels = () => scene.model().menu.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | ');
const clickCmd = (label) => {
  const b = scene.model().menu.find((x) => x.label === label);
  if (!b) throw new Error(`no command '${label}' — offered: ${labels()}`);
  backend.point(b.x + 4, b.y + 4, [true]); rt.advance(1);
  backend.point(b.x + 4, b.y + 4, []); rt.advance(1);
};
const clickEntity = (e) => {
  const o = Object.values(scene.model().slots).flat().find((x) => x.e === e);
  backend.point(o.at.x + 8, o.at.y + 8, [true]); rt.advance(1);
  backend.point(o.at.x + 8, o.at.y + 8, []); rt.advance(1);
};

console.log('a second game, through a renderer that was written for the first');
check('zones, not rooms: things are laid out where the world put them',
      JSON.stringify(Object.fromEntries(
        Object.entries(scene.model().slots).map(([a, l]) => [a, l.map((o) => o.e)]))) ===
      '{"a_self":["you"],"a_foes":["gnoll","imp"],"a_hand":["edge_a","edge_b","spark","salve"]}',
      JSON.stringify(Object.entries(scene.model().slots).map(([a, l]) => [a, l.map((o) => o.e)])));
check('the subject is you and the object is what you are aiming at',
      scene.model().picked === 'you' && scene.model().aimed === 'gnoll');
check('the menu is the hand, aimed at the object',
      labels() === 'STRIKE EDGE A | STRIKE EDGE B | BOLT | MEND | END TURN', labels());
check('a global command with no arguments is still reachable',
      scene.model().menu.some((b) => b.term === 'end_turn'));
check('selection is not duplicated as a menu row',
      !scene.model().menu.some((b) => b.term.startsWith('aim_')), labels());

console.log('\nplaying it');
clickCmd('STRIKE EDGE A');
check('a card was played at the aimed foe', world.state.hp('gnoll') === 8,
      String(world.state.hp('gnoll')));
check('...and the card left the hand', world.state.in_zone('edge_a') === 'z_spent');
check('...and it cost energy', world.state.energy() === 2, String(world.state.energy()));
check('the cue the STORY chose was played',
      backend.played.some(([k, id]) => k === 'sound' && id === 'snd_thud'));

clickEntity('imp');
check('clicking a foe re-aims — the object moved, not the subject',
      scene.model().aimed === 'imp' && scene.model().picked === 'you');
check('and the menu is now aimed there',
      scene.model().menu.some((b) => b.term === 'bolt(spark,imp)'), labels());
clickCmd('BOLT');
check('the imp is down', world.state.hp('imp') === 1, String(world.state.hp('imp')));

clickCmd('END TURN');
check('the turn refilled energy', world.state.energy() === 3);
check('...returned the spent cards', world.state.in_zone('edge_a') === 'z_hand');
check('...and every standing foe hit back', world.state.hp('you') === 14,
      String(world.state.hp('you')));

// ---- the measurement -------------------------------------------------------

const BLESSED = ['panel', 'caption', 'shows', 'in_anchor', 'prop_in',
                 'held', 'shaded', 'gauge', 'picked', 'aimed', 'here',
                 'cue_sound', 'cue_word'];
const has = (iface, n) => iface.judgments.some((j) => j.name === n);
const both = BLESSED.filter((n) => has(cellar.IFACE, n) && has(duel.IFACE, n));
const one = BLESSED.filter((n) => has(cellar.IFACE, n) !== has(duel.IFACE, n))
                   .map((n) => `${n} (${has(cellar.IFACE, n) ? 'cellar' : 'duel'})`);

console.log('\nthe intersection — measured, not asserted');
console.log(`  both games needed     ${both.join(', ')}`);
console.log(`  one game's furniture  ${one.join(', ')}`);
console.log(`  neither could say     a number that is not hp (energy, a card's cost);`);
console.log(`                        a label on a THING rather than on a region;`);
console.log(`                        "select one, clear the rest" — both games paid`);
console.log(`                        one concrete action per entity for it`);

check('the shared core is most of the vocabulary, not a coincidence',
      both.length >= 8, both.join(','));
// Comments may name the games they were learned from; CODE may not. A branch
// on a game's own word is the failure this whole exercise is testing for, and
// it would be invisible in a passing playthrough.
check('and the renderer has no per-game branch in its CODE', (() => {
  const code = readFileSync(new URL('./platform/scene.mjs', import.meta.url), 'utf8')
    .split('\n')
    .filter((l) => !/^\s*(\/\/|\*|\/\*)/.test(l))
    .map((l) => l.replace(/\/\/.*$/, ''))
    .join('\n');
  return !/cellar|torch|door|hero|duel|gnoll|card|fighter/i.test(code);
})());

world.close();
console.log(failed ? `\nskins_check: ${failed} FAILED` : '\nskins_check: all passed');
process.exit(failed ? 1 : 0);
