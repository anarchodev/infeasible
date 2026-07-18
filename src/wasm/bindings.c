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
