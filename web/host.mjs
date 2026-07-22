// host.mjs — a hand-written plain-JS host driving the grounded cellar over the
// WASM core (DESIGN.md §4.2 driver, §12 web target). No renderer, no codegen:
// this is the raw JS↔WASM boundary, proving a game loop can query judgments,
// propose actions, step, and read `why?` traces entirely from JavaScript
// against `world_*`. A typed binding (§6.3) would sit on top of exactly this.
//
// Run: node web/host.mjs   (after web/build.sh produces web/infeasible.mjs)

import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

// The WASM module is CommonJS (see web/build.sh); load it from ESM via
// createRequire. SINGLE_FILE means the .wasm is embedded — nothing is fetched.
const require = createRequire(import.meta.url);
const createInfeasible = require('./infeasible.cjs');

const M = await createInfeasible();

// cwrap the flat shim (exports.c). This hand-wiring is what the generated
// binding will one day replace — but the boundary is identical either way.
const inf = {
  compile:  M.cwrap('inf_compile',   'number', ['string']),
  free:     M.cwrap('inf_free',       null,    ['number']),
  intern:   M.cwrap('inf_intern',    'number', ['number', 'string']),
  query:    M.cwrap('inf_query',     'number', ['number', 'number', 'number']),
  get:      M.cwrap('inf_get',       'number', ['number', 'number']),
  step1:    M.cwrap('inf_step1',     'number', ['number', 'number']),
  lastErr:  M.cwrap('inf_last_err',  'string', []),
  lastDiag: M.cwrap('inf_last_diag', 'string', []),
  why:      M.cwrap('inf_why',       'string', ['number', 'number', 'number']),
};

const VERDICT = ['undecided', 'PROVED', 'REFUTED'];

const src = readFileSync(new URL('../examples/cellar_ground.story', import.meta.url), 'utf8');
const s = inf.compile(src);
if (!s) {
  console.error('compile failed:\n' + inf.lastDiag());
  process.exit(1);
}
const diag = inf.lastDiag();
if (diag) process.stdout.write(diag);   // warnings, if any

// query a ground judgment by its interned name — same id a C client would hit
const q = (name) => VERDICT[inf.query(s, inf.intern(s, name), 0)];
const fluent = (name) => (inf.get(s, inf.intern(s, name)) ? 'true' : 'false');

console.log('=== grounded cellar over WASM ===');
console.log('the guard holds the antidote; the hero does not\n');
for (const who of ['hero', 'guard']) {
  console.log(`${who}:  weakened=${q(`weakened(${who})`)}` +
              `  can_force_door=${q(`can_force_door(${who})`)}`);
}

console.log('\n--- why can_force_door(guard)? ---');
process.stdout.write(inf.why(s, inf.intern(s, 'can_force_door(guard)'), 0));

// propose the guard's force_door and step; the shared door fluent should flip
console.log(`\ndoor_closed before: ${fluent('door_closed')}`);
const rc = inf.step1(s, inf.intern(s, 'force_door(guard)'));
if (rc !== 0) { console.error('step failed on fluent: ' + inf.lastErr()); process.exit(1); }
console.log(`door_closed after force_door(guard): ${fluent('door_closed')}`);

inf.free(s);
console.log('\nboundary OK: .story in, judgments/why/step out — all from JS.');
