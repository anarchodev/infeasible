// binding_check.mjs — the generated binding's own checks.
//
//   web/build.sh
//   node web/gen_binding.mjs examples/reaction5e.story
//   node web/binding_check.mjs
//
// reaction5e is bound because it carries the two things this file is about: an
// `exclusive` group (#159) and a multi-valued `phase` fluent. What is pinned is
// §6.3's totality boundary — the host-protocol `-1`s die at CONSTRUCTION, not
// at the step:
//
//   an unknown action is unconstructible (there is no function to call),
//   a bad entity is a TypeError where it is written,
//   an exclusive violation is refused by `add`, naming the order already in the
//   set — a networked host collecting orders from remote clients has to reject
//   one order, not lose the tick,
//
// so a `world_step` -1 reaching a bound host is unconditionally a bug.

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { open, SORTS, STORY, SOURCE_HASH } from './reaction5e.binding.mjs';

const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.cjs');

const M = await createInfeasible();
const src = readFileSync(new URL('../' + STORY, import.meta.url), 'utf8');
const w = open(M, src);

let failures = 0;
const check = (label, cond) => {
  console.log(`  ${cond ? 'PASS' : 'FAIL'}  ${label}`);
  if (!cond) failures++;
};
const throws = (fn, frag) => {
  try { fn(); return false; } catch (e) { return String(e.message).includes(frag); }
};

console.log(`binding: ${STORY} (${SOURCE_HASH})`);

console.log('\nvocabulary');
check('an unknown action has no constructor', w.a.teleport === undefined);
check('an unknown judgment has no query', w.q.can_teleport === undefined);
check('a bad entity is refused where it is written',
      throws(() => w.a.strike('gruk', 'vera'), "not a member of 'actor'"));
check('the declared sorts are the ones the story declared',
      SORTS.actor.join(',') === 'grunk,vera');

console.log('\nstate readers');
check('the phase fluent reads as its value', w.state.phase() === 'declare');
check('a numeric fluent reads as a number', w.state.hp('grunk') === 7);
check('a boolean fluent reads as a boolean', w.state.alive('grunk') === true);

console.log('\nthe action-set builder (#159 at construction)');
check('two strikes by one attacker are refused at add',
      throws(() => w.actions()
                    .add(w.a.strike('grunk', 'vera'))
                    .add(w.a.strike('grunk', 'grunk')),
             'exclusive with'));
check('the same order twice is one order, not a violation',
      w.actions().add(w.a.strike('grunk', 'vera'))
                 .add(w.a.strike('grunk', 'vera')).length === 1);
check('distinct attackers are independent keys',
      w.actions().add(w.a.strike('grunk', 'vera'))
                 .add(w.a.strike('vera', 'grunk')).length === 2);

console.log('\nthe step, and what it reports');
w.step(w.actions().add(w.a.strike('grunk', 'vera')));
check('the phase advanced by rule, not by a host write',
      w.state.phase() === 'react');
const moved = w.changed().map((d) => d.atom);
check('the changeset names what moved', moved.includes('pending(grunk,vera)'));
check('the changeset carries the locked die',
      w.changed().some((d) => d.atom === 'atk_die(grunk)' && d.to > 0));
const rc = w.receipt('atk_die(grunk)');
check('the receipt attributes the write to its rule',
      rc !== null && rc.items.length === 1 && rc.items[0].rule === 'strike');
check('the receipt binds the rule to its arguments',
      rc.items[0].bind.A === 'grunk' && rc.items[0].bind.T === 'vera');

console.log('\nstaleness');
check('a binding refuses a story it was not generated from',
      throws(() => open(M, src + '\n// edited\n'), 'regenerate it'));

w.close();
console.log(failures === 0 ? '\nbinding_check: all passed'
                           : `\nbinding_check: ${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);
