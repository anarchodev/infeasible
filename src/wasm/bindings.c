/* src/wasm/bindings.c — flat, JS-callable shim over the world_* API for the
 * WASM loop driver. Compiles ONLY under emcc (see scripts/build_wasm.sh); it is
 * not part of the CMake core library.
 *
 * Design: the boundary carries scalars and typed-array pointers only. dl_lit is
 * flattened to parallel (atom[], neg[]) arrays and step_cond to (atom[], neg[],
 * primed[]), so the Node side never lays out a C struct. wf_malloc/wf_free let
 * JS stage those arrays in the wasm heap without exporting libc malloc. */

#include <emscripten.h>
#include <stdlib.h>

#include "core/intern.h"
#include "logic/dl.h"
#include "state/world.h"

#define API EMSCRIPTEN_KEEPALIVE

/* --- scratch allocation for marshalled arrays --- */
API void *wf_malloc(int nbytes) { return malloc((size_t) nbytes); }
API void  wf_free(void *p)      { free(p); }

/* --- interning + world lifecycle --- */
API intern  *wf_intern_new(void)                     { return intern_new(); }
API uint32_t wf_id(intern *t, const char *name)      { return intern_id(t, name); }
API world   *wf_world_new(intern *t)                 { return world_new(t); }

/* --- fluents --- */
API void wf_declare(world *w, uint32_t atom)         { world_declare_fluent(w, atom); }
API void wf_set(world *w, uint32_t atom, int value)  { world_set(w, atom, value != 0); }
API int  wf_get(world *w, uint32_t atom)             { return world_get(w, atom) ? 1 : 0; }

/* --- queries (returns dl_verdict as int: 0 undecided, 1 proved, 2 refuted) --- */
API int wf_query(world *w, uint32_t atom, int neg) {
    dl_lit q = { atom, neg != 0 };
    return (int) world_query(w, q);
}

/* --- rule/step construction --- */
static dl_lit *build_lits(const uint32_t *atoms, const int *negs, int n) {
    if (n <= 0) return NULL;
    dl_lit *ls = malloc(sizeof(dl_lit) * (size_t) n);
    for (int i = 0; i < n; i++) {
        ls[i].atom = atoms[i];
        ls[i].neg  = negs[i] != 0;
    }
    return ls;
}

API int wf_add_rule(world *w, const char *name, int kind,
                    uint32_t headAtom, int headNeg,
                    const uint32_t *bodyAtoms, const int *bodyNegs, int nbody) {
    dl_lit  head = { headAtom, headNeg != 0 };
    dl_lit *body = build_lits(bodyAtoms, bodyNegs, nbody);
    int h = world_add_rule(w, name, (dl_rule_kind) kind, head, body, nbody);
    free(body);
    return h;
}

API void wf_add_sup(world *w, int winner, int loser) { world_add_sup(w, winner, loser); }

/* action == 0 (INTERN_NONE) makes this a ramification. */
API void wf_add_step_rule(world *w, const char *name, uint32_t action,
                          const uint32_t *bAtoms, const int *bNegs,
                          const int *bPrimed, int nbody,
                          const uint32_t *eAtoms, const int *eNegs, int neff) {
    step_cond *body = NULL;
    if (nbody > 0) {
        body = malloc(sizeof(step_cond) * (size_t) nbody);
        for (int i = 0; i < nbody; i++) {
            body[i].lit.atom = bAtoms[i];
            body[i].lit.neg  = bNegs[i] != 0;
            body[i].primed   = bPrimed[i] != 0;
        }
    }
    dl_lit *eff = build_lits(eAtoms, eNegs, neff);
    world_add_step_rule(w, name, action, body, nbody, eff, neff);
    free(body);
    free(eff);
}

/* Returns 0 on success (state committed), -1 on contested/undecided fluent
 * (state untouched); err receives the offending fluent name. */
API int wf_step(world *w, const uint32_t *actions, int nactions,
                char *err, int errsz) {
    return world_step(w, actions, nactions, err, (size_t) errsz);
}

/* --- the step log (#88): the changeset and the commit receipt ---
 *
 * Both marshal into a caller-provided buffer of `long` and return the number of
 * cells the FULL answer needs — so a host that guessed too small grows and asks
 * again rather than being handed a silently truncated log (the same
 * loud-failure rule the engine's own caps follow). One crossing per readout.
 *
 * wf_bool_deltas: pairs   [atom, value]
 * wf_num_deltas:  triples [atom, from, to]
 * wf_num_receipt: a header [base, raw, applied, lo, hi, flags, nrows] — flags
 *   bit 0 = has_range, bit 1 = clamped — then per contribution a
 *   variable-length row [op, amount, defeated, pred, nbind, (var, ent) * nbind],
 *   which the host walks with a cursor. Rows are variable rather than padded
 *   because a binding is 1-2 pairs in practice and a fixed stride would either
 *   waste the common case or cap the general one. */
API int wf_bool_deltas(world *w, long *out, int cap) {
    int n;
    const world_bool_delta *d = world_bool_deltas(w, &n);
    for (int i = 0; i < n && 2 * i + 1 < cap; i++) {
        out[2 * i]     = (long) d[i].atom;
        out[2 * i + 1] = d[i].value ? 1 : 0;
    }
    return 2 * n;
}

API int wf_num_deltas(world *w, long *out, int cap) {
    int n;
    const world_num_delta *d = world_num_deltas(w, &n);
    for (int i = 0; i < n && 3 * i + 2 < cap; i++) {
        out[3 * i]     = (long) d[i].atom;
        out[3 * i + 1] = d[i].from;
        out[3 * i + 2] = d[i].to;
    }
    return 3 * n;
}

API int wf_num_receipt(world *w, uint32_t atom, long *out, int cap) {
    world_receipt r;
    if (!world_num_receipt(w, atom, &r)) return 0;
    int k = 0;
#define PUT(v) do { if (k < cap) out[k] = (long)(v); k++; } while (0)
    PUT(r.base); PUT(r.raw); PUT(r.applied); PUT(r.lo); PUT(r.hi);
    PUT((r.has_range ? 1 : 0) | (r.clamped ? 2 : 0));
    PUT(r.n);
    for (int i = 0; i < r.n; i++) {
        const world_contrib *c = &r.items[i];
        PUT(c->op); PUT(c->amount); PUT(c->defeated ? 1 : 0);
        PUT(c->pred); PUT(c->nbind);
        for (int b = 0; b < c->nbind; b++) { PUT(c->vars[b]); PUT(c->ents[b]); }
    }
#undef PUT
    return k;
}

/* The name behind any atom id a readout returns (a rule's pred, a delta's
 * fluent) — the JS side holds ids, not strings, until it renders. */
API const char *wf_name(intern *t, uint32_t atom) { return intern_name(t, atom); }

/* --- burst cues (#11, §12): the transient emission channel ---
 *
 * The per-tick stream crosses the boundary the way §12's delta buffer does:
 * ONE crossing per step, not one per event. wf_emits returns a pointer into
 * the engine's own linear memory, so JS reads it as a zero-copy
 * `new Uint32Array(M.HEAPU32.buffer, ptr, n)` view — the engine writes its
 * memory, JS reads through the view, nothing is marshalled per atom. The view
 * is valid until the next wf_step (and until any memory.grow detaches it). */
API void wf_declare_emit(world *w, uint32_t atom) { world_declare_emit(w, atom); }
API int  wf_emit_count(world *w) { int n; world_emits(w, &n); return n; }
API const uint32_t *wf_emits(world *w) { int n; return world_emits(w, &n); }
