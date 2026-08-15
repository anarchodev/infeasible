// pure_check.mjs — the infeasible cart, played with no game code.
//
//   node web/pure_check.mjs
//
// §12 asks a falsifiable question: can a cart be `.story` and assets alone? The
// answer this file measures is "yes, at a cost you can count". Everything the
// player sees below is a conclusion the world reached — the frame, the labels,
// what is where, what a torch looks like, which commands exist and which are
// refused, which sound a cue plays. The JS that draws it (`platform/scene.mjs`)
// has never heard of a cellar; the JS that drives it (`platform/purecart.mjs`)
// contains no game at all.
//
// It also counts the residue honestly and prints it, because the interesting
// output of this experiment is not PASS but the price.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

import { createPlatform } from './platform/platform.mjs';
import { createHeadlessBackend } from './platform/headless.mjs';
import { createRuntime } from './platform/runtime.mjs';
import { pureCart } from './platform/purecart.mjs';
import { open, SORTS, ENUMS, IFACE, STORY } from './cellar_pure.binding.mjs';
import { TILES, SPRITES } from './carts/cellar.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.js');

let failed = 0;
const check = (what, cond, extra = '') => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${what}${cond || !extra ? '' : `\n        ${extra}`}`);
  if (!cond) failed++;
};

const M = await createInfeasible();
const src = readFileSync(new URL('../' + STORY, import.meta.url), 'utf8');
const world = open(M, src);

// The atlases are ASSETS, not logic — the sprite sheet is reused unchanged from
// the hand-written cart, since pixels are not expressible as rules for the same
// reason a .png is not. `main_fog` is the dither the composite op multiplies.
const FOG = { tile: 8, cols: 1, pixels: TILES.pixels.map((r) => r.slice(16, 24)) };
const cart = pureCart({ world, iface: IFACE, doms: { ...SORTS, ...ENUMS },
                        sheets: { main: SPRITES, main_fog: FOG } });

const backend = createHeadlessBackend({ resolution: cart.resolution });
const platform = createPlatform(backend);
const rt = createRuntime({ platform, backend, cart, world });
rt.advance(1);

const scene = cart.scene;
const labels = () => scene.model().menu.map((b) => `${b.label}${b.ok ? '' : '*'}`).join(' | ');

console.log('drawn from conclusions alone');
check('the frame is a set of panels the world concluded',
      scene.pairs('panel').length === 5, JSON.stringify(scene.pairs('panel')));
check('the captions are atoms read as English',
      backend.text().includes('THE CELLAR') && backend.text().includes('CELLAR') &&
      backend.text().includes('VAULT'), backend.text());
check('the map drew sprites for whatever the rules placed',
      backend.ops('spr').length >= 3, `${backend.ops('spr').length} sprites`);
check('the dark cellar shaded itself', backend.ops('shade').length > 0);
// No `cmd` vocabulary, no `offers`/`blocked` rules: this list is the engine's
// own answer about its own actions, filtered to rows that are one step away
// with a judgment refusing them — a refusal that is an argument, not an absence.
check('the menu is the engine\'s answer, with no rule written per action',
      labels() === 'GO HALL | ENTER VAULT* | TAKE KEY* | UNLOCK* | FORCE DOOR*',
      labels());
check('...and every greyed row is refused by a JUDGMENT, so `why` has something to say',
      scene.model().menu.filter((b) => !b.ok)
           .every((b) => b.blockers.length === 1),
      JSON.stringify(scene.model().menu.filter((b) => !b.ok).map((b) => b.blockers)));

console.log('\nplaying it — the renderer never learns what a torch is');
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

clickCmd('TAKE KEY');
check('a refused command printed the guard that refused it',
      backend.text().includes('in_dark'),
      backend.text().split('\n').slice(0, 3).join(' / '));

clickCmd('GO HALL');
check('a click moved the hero', world.state.at('hero') === 'hall', world.state.at('hero'));

// A verb grounds once per binding, so a flat menu shows DROP TORCH once per
// room. Every APPLICABLE instance is a real choice and stays; a verb with none
// contributes at most one refused row, because "you cannot drop it here" is
// not three different reasons.
check('no verb is listed twice', (() => {
  const labels = scene.model().menu.map((b) => b.label);
  return new Set(labels).size === labels.length;
})(), labels());
check('the cue table played a sound the STORY chose',
      backend.played.some(([k, id]) => k === 'sound' && id === 'snd_step'),
      JSON.stringify(backend.played));
clickCmd('TAKE TORCH');
check('...still no duplicates once the torch is in hand', (() => {
  const l = scene.model().menu.map((b) => b.label);
  return new Set(l).size === l.length && l.filter((x) => x === 'DROP TORCH').length === 1;
})(), labels());
check('and the one DROP TORCH kept is the one that applies',
      scene.model().menu.find((b) => b.label === 'DROP TORCH')?.term === 'drop_torch(hero,hall)',
      scene.model().menu.find((b) => b.label === 'DROP TORCH')?.term);
clickCmd('GO CELLAR');
check('the fetched torch lit the room, and the fog is gone',
      backend.ops('shade').length === 0);
check('...so the key is offered enabled now',
      scene.model().menu.some((b) => b.label === 'TAKE KEY' && b.ok), labels());
clickCmd('TAKE KEY');
clickCmd('GO HALL');
clickCmd('UNLOCK');
check('the door turned', world.state.door() === 'jammed');
check('forcing it is refused for the poisoned hero',
      scene.model().menu.some((b) => b.label === 'FORCE DOOR' && !b.ok), labels());

clickEntity('guard');
check('clicking an actor selected them — an ACTION, because selection is state',
      world.state.selected('guard') && !world.state.selected('hero'));
check('and the menu is now the guard\'s', labels().includes('FORCE DOOR'), labels());
clickCmd('FORCE DOOR');
check('the guard opened it', world.state.door() === 'open');
check('the receipt is in the log, not in a handler', world.state.hp('guard') === 10);

clickEntity('hero');
clickCmd('ENTER VAULT');
clickCmd('TAKE ANTIDOTE');
clickCmd('DRINK');
check('the cellar is solved with zero game code',
      !world.state.poisoned('hero') && world.state.at('hero') === 'vault');

// ---- the price, which is the actual result ---------------------------------

const story = readFileSync(new URL('../examples/cellar_pure.story', import.meta.url), 'utf8');
const code = (t) => t.split('\n').filter((l) => l.trim() && !l.trim().startsWith('//')).length;
const rules = (t, re) => (t.match(re) ?? []).length;
const presentation = story.slice(story.indexOf('// ---- PRESENTATION'));
const sceneSrc = readFileSync(new URL('./platform/scene.mjs', import.meta.url), 'utf8');
const cartSrc = readFileSync(new URL('./platform/purecart.mjs', import.meta.url), 'utf8');
const handSrc = readFileSync(new URL('./carts/cellar.mjs', import.meta.url), 'utf8');

// ---- the PORTABLE path: the same cart, played with NO POINTER -------------
//
// §12's input model is focus + confirm precisely so a cart runs on a d-pad,
// and the only way to know a cart does is to play it that way. `backend.pad()`
// drives nav/confirm directly — no pointer, no keyboard, the way a console
// backend would. Run last, because it moves the world.
const padStep = (o) => { backend.pad(o); rt.advance(1); backend.pad({}); rt.advance(1); };
const padTo = (want) => {
  for (let i = 0; i < 40; i++) {
    if (rt.ctx.focus.current() === want) { padStep({ confirm: true }); return true; }
    padStep({ nav: ['down'] });
  }
  return false;
};
{
  const offered = scene.model().menu.find((b) => b.ok);
  const before = rt.ctx.log.length;
  const reached = padTo(`cmd:${offered.term}`);
  check('a d-pad reaches a command with no pointer at all', reached, offered?.label);
  check('...and confirming it submits the action a click would',
        rt.ctx.log.length === before + 1, `log ${before} -> ${rt.ctx.log.length}`);
  // focus is geometric, so it must also come BACK
  const first = rt.ctx.focus.current();
  padStep({ nav: ['up'] });
  check('...and navigation is reversible', rt.ctx.focus.current() !== first ||
        scene.model().menu.length === 1, rt.ctx.focus.current());
}


console.log('\nthe price');
console.log(`  presentation rules in .story        ${rules(presentation, /^rule /gm)}`);
console.log(`  menu rules                          0  (the engine answers it)`);
console.log(`  presentation vocabulary (enums)     ${rules(story, /^enum /gm)} enums, ` +
            `${(ENUMS.anchor?.length ?? 0) + (ENUMS.word?.length ?? 0) + (ENUMS.cmd?.length ?? 0)} members`);
console.log(`  geometry rows in init               ${rules(story, /^ *a[xywh]\(/gm)}`);
console.log(`  GAME code in JS                     0`);
console.log(`  generic renderer (scene.mjs)        ${code(sceneSrc)} lines`);
console.log(`  generic driver  (purecart.mjs)      ${code(cartSrc)} lines`);
console.log(`  the hand-written cart it replaces   ${code(handSrc)} lines, all game-specific`);

world.close();
console.log(failed ? `\npure_check: ${failed} FAILED` : '\npure_check: all passed');
process.exit(failed ? 1 : 0);
