// host.mjs — a plain-JS host driving the grounded cellar over the WASM core
// (DESIGN.md §4.2 driver, §12 web target), through the GENERATED TYPED BINDING
// (§6.3) rather than by hand-interning atom names.
//
// That is the point of this file. The engine's own boundary is atom ids, and an
// id comes from a string: `query(intern("can_force_dor(guard)"))` interns a
// fresh, always-false atom and answers REFUTED forever — the silent failure the
// orphan pass exists to kill, reappearing on the host side. The binding is
// generated from the compiler's own vocabulary (the §6.3 interface artifact),
// so every ground term is spelled once, by the compiler, and a rename in the
// .story breaks regeneration and the typecheck instead of quietly never firing.
//
// Run:  web/build.sh                                  (emits web/infeasible.js)
//       node web/gen_binding.mjs examples/cellar_ground.story
//       node web/host.mjs
//
// The binding ships as plain ES-module JS with JSDoc types — `tsc --checkJs`
// and any editor check this file against it with no build step (§12).

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { open, SORTS, STORY } from './cellar_ground.binding.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.js');

const M = await createInfeasible();
const src = readFileSync(new URL('../' + STORY, import.meta.url), 'utf8');
const w = open(M, src);

console.log('=== grounded cellar over WASM, through the typed binding ===');
console.log(`story: ${STORY}   actors: ${SORTS.actor.join(', ')}\n`);

// Judgments, by name and arity — `w.q.can_force_door('guard')`, not a string.
// A typo is a missing property; a bad entity is a TypeError at the call, not a
// REFUTED verdict three screens later.
for (const who of SORTS.actor) {
  console.log(`${who}:  weakened=${w.q.weakened(who)}` +
              `  can_force_door=${w.q.can_force_door(who)}`);
}

console.log('\n--- why can_force_door(guard)? ---');
process.stdout.write(w.why('can_force_door(guard)'));

// The reactive channel (§11 M2): declare interest once, then react to what
// flipped. One call shape for the base fact and for the judgment in its cone —
// which is the point, since a fluent refactored into a judgment must not touch
// a client call site.
const watch = {
  door: w.subscribe('door_closed'),
  guardCanForce: w.subscribe('can_force_door(guard)'),
  heroCanForce: w.subscribe('can_force_door(hero)'),
};
console.log('\nsubscribed:',
  Object.entries(watch).map(([k, h]) => `${k}=${w.verdict(h)}`).join('  '));

// The action set is assembled through the builder (§6.3): an action that does
// not exist is unconstructible, and the protocol checks happen at `add`.
console.log(`\ndoor_closed before: ${w.state.door_closed()}`);
const orders = w.actions().add(w.a.force_door('guard'));
w.step(orders);
console.log(`door_closed after force_door(guard): ${w.state.door_closed()}`);

// What the step changed, enumerated by the engine (#88) — the host does not
// diff, and does not need to know which atoms to ask about.
for (const d of w.changed())
  console.log(`  changed: ${d.atom} = ${'value' in d ? d.value : `${d.from} -> ${d.to}`}`);

// …and what it changed that this client asked about. The door fact and both
// judgments standing on it flip together, reported in subscription order.
for (const e of w.edges())
  console.log(`  ${e.rose ? 'rose' : e.fell ? 'fell' : 'moved'}: ` +
              `${e.neg ? '~' : ''}${e.atom} (${e.from} -> ${e.to})`);

// The vocabulary check is real, not decorative:
try {
  w.q.can_force_door('goblin');            // not an actor in this story
} catch (e) {
  console.log(`\nvocabulary check: ${e.message}`);
}
console.log(`unknown action is unconstructible: ${w.a.pick_lock === undefined}`);

w.close();
console.log('\nboundary OK: .story in, typed judgments/why/step/changeset out — all from JS.');
