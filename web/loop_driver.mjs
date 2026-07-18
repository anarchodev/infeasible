// web/loop_driver.mjs — the WASM full-loop proof.
//
// A JS "outer engine" drives a scripted combat scenario against the logic core
// compiled to WASM (build with scripts/build_wasm.sh, then `node web/loop_driver.mjs`).
// This is the boolean shadow of combat5e (M0 has no numerics): grunk's HP is a
// two-step state machine (healthy -> wounded -> dead). It exercises the real
// host->core->step->read-judgments->branch loop across the JS/WASM boundary.

import createModule from "../build-wasm/infeasible.mjs";

const M = await createModule();

// ---- binding layer --------------------------------------------------------
const c = (f, ret, args) => M.cwrap(f, ret, args);
const internNew = c("wf_intern_new", "number", []);
const symId     = c("wf_id",         "number", ["number", "string"]);
const worldNew  = c("wf_world_new",  "number", ["number"]);
const declare   = c("wf_declare",    null,     ["number", "number"]);
const setFluent = c("wf_set",        null,     ["number", "number", "number"]);
const getFluent = c("wf_get",        "number", ["number", "number"]);
const queryRaw  = c("wf_query",      "number", ["number", "number", "number"]);
const addRule   = c("wf_add_rule",   "number",
  ["number", "string", "number", "number", "number", "number", "number", "number"]);
const addStepR  = c("wf_add_step_rule", null,
  ["number","string","number","number","number","number","number","number","number","number"]);
const stepRaw   = c("wf_step",       "number", ["number","number","number","number","number"]);
const wmalloc   = c("wf_malloc",     "number", ["number"]);
const wfree     = c("wf_free",       null,     ["number"]);

// dl_verdict / dl_rule_kind mirror dl.h
const UNDECIDED = 0, PROVED = 1, REFUTED = 2;
const DEFEASIBLE = 1;
const NONE = 0; // INTERN_NONE: ramification trigger

function u32(arr) { if (!arr.length) return 0; const p = wmalloc(arr.length*4); M.HEAPU32.set(Uint32Array.from(arr), p>>2); return p; }
function i32(arr) { if (!arr.length) return 0; const p = wmalloc(arr.length*4); M.HEAP32 .set(Int32Array .from(arr), p>>2); return p; }

const VERDICT = { [UNDECIDED]: "undecided", [PROVED]: "proved", [REFUTED]: "refuted" };

// ---- world construction ---------------------------------------------------
const sym = internNew();
const A = {}; // name -> atom id
const atom = (name) => (A[name] ??= symId(sym, name));

const w = worldNew(sym);

const FLUENTS = [
  "holding_longsword_aria", "adjacent", "monster_grunk",
  "holding_shortbow_grunk", "wounded_grunk", "dead_grunk", "on_floor_shortbow",
];
for (const f of FLUENTS) declare(w, atom(f));

// wants_flee_grunk is a JUDGMENT (derived, never stored) — not a fluent.
function judgment(name, headName, bodyNames) {
  const bAtoms = bodyNames.map(atom), bNegs = bodyNames.map(() => 0);
  const pa = u32(bAtoms), pn = i32(bNegs);
  addRule(w, name, DEFEASIBLE, atom(headName), 0, pa, pn, bodyNames.length);
  wfree(pa); wfree(pn);
}
judgment("goblin_flees", "wants_flee_grunk", ["wounded_grunk", "monster_grunk"]);

// step rules: [atom, neg, primed] body triples; [atom, neg] effect pairs
function stepRule(name, action, body, effects) {
  const pba = u32(body.map(t => atom(t[0]))), pbn = i32(body.map(t => t[1])), pbp = i32(body.map(t => t[2]));
  const pea = u32(effects.map(e => atom(e[0]))), pen = i32(effects.map(e => e[1]));
  addStepR(w, name, action, pba, pbn, pbp, body.length, pea, pen, effects.length);
  for (const p of [pba, pbn, pbp, pea, pen]) wfree(p);
}
const strike = atom("sword_strike");
//                         [ atom,                      neg, primed ]
stepRule("wound", strike, [["holding_longsword_aria",0,0], ["adjacent",0,0], ["wounded_grunk",1,0]],
                          [["wounded_grunk", 0]]);
stepRule("kill",  strike, [["holding_longsword_aria",0,0], ["adjacent",0,0], ["wounded_grunk",0,0]],
                          [["dead_grunk", 0]]);
// ramification: dead' & holding shortbow  ->  drop it
stepRule("death_drop", NONE, [["dead_grunk",0,1], ["holding_shortbow_grunk",0,0]],
                             [["holding_shortbow_grunk", 1], ["on_floor_shortbow", 0]]);

// initial state
const INIT = { holding_longsword_aria:1, adjacent:1, monster_grunk:1, holding_shortbow_grunk:1,
               wounded_grunk:0, dead_grunk:0, on_floor_shortbow:0 };
for (const [f, v] of Object.entries(INIT)) setFluent(w, atom(f), v);

// ---- helpers --------------------------------------------------------------
const query = (name) => queryRaw(w, atom(name), 0);
const isSet = (name) => getFluent(w, atom(name)) === 1;
function step(actionName) {
  const pa = u32([atom(actionName)]);
  const errp = wmalloc(128);
  const rc = stepRaw(w, pa, 1, errp, 128);
  const err = rc === -1 ? M.UTF8ToString(errp) : "";
  wfree(pa); wfree(errp);
  if (rc === -1) throw new Error(`step(${actionName}) rejected: contested fluent '${err}'`);
}

let failures = 0;
function check(label, cond) { console.log(`  ${cond ? "PASS" : "FAIL"}  ${label}`); if (!cond) failures++; }

// ---- the scenario ---------------------------------------------------------
console.log("turn 0 — before combat");
check("goblin does not want to flee", query("wants_flee_grunk") === REFUTED);

console.log("turn 1 — aria: sword_strike");
step("sword_strike");
check("grunk is wounded", isSet("wounded_grunk"));
check("grunk not yet dead", !isSet("dead_grunk"));
check("wounded goblin now wants to flee (judgment)", query("wants_flee_grunk") === PROVED);

console.log("turn 2 — aria: sword_strike");
step("sword_strike");
check("grunk is dead", isSet("dead_grunk"));
check("ramification fired: shortbow dropped", !isSet("holding_shortbow_grunk"));
check("ramification fired: shortbow on floor", isSet("on_floor_shortbow"));

console.log(failures === 0 ? "\nloop_driver: all passed" : `\nloop_driver: ${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);
