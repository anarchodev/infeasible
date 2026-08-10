/* WASM export shim: a flat, primitive-only boundary over the engine's public
 * surface (DESIGN.md §4.2 kernel ports, §12 WASM target). This is the
 * hand-written JS↔WASM seam — no `dl_lit` structs or FILE* cross into JS;
 * everything is atom ids (uint32) and C strings that Emscripten marshals.
 *
 * A `.story` source string comes in, a session (intern table + world) comes
 * back, and the JS host queries judgments by ground-atom name, proposes
 * actions, steps, and pulls `why?` traces as strings. No codegen yet: the JS
 * host interns names by hand exactly as the C golden tests do — this proves
 * the boundary before any typed binding is generated over it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/intern.h"
#include "state/world.h"
#include "lang/story.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

typedef struct { intern *syms; world *w; char *iface; } inf_session;

/* Reused across calls; the cwrap 'string' return type copies into a JS string
 * immediately, so a single owned buffer per kind is safe. */
static char  g_err[256];
static char  g_diag[4096];
static char *g_why;

/* Compile a .story source. Returns an opaque session pointer, or NULL on an
 * error-severity diagnostic (retrieve the joined messages via inf_last_diag). */
EXPORT inf_session *inf_compile(const char *src)
{
    g_diag[0] = '\0';
    intern *syms = intern_new();

    story_diag items[64];
    story_diags diags = { items, 64, 0, 0 };
    char *iface = NULL;
    world *w = story_compile_iface(src, "<host>", syms, &diags, &iface);

    /* surface every diagnostic (errors and warnings) to the host */
    size_t off = 0;
    int shown = diags.count < diags.cap ? diags.count : diags.cap;
    for (int i = 0; i < shown && off < sizeof g_diag - 1; i++)
        off += (size_t)snprintf(g_diag + off, sizeof g_diag - off, "%s:%d:%d: %s\n",
                                items[i].sev == STORY_ERROR ? "error" : "warning",
                                items[i].line, items[i].col, items[i].msg);

    if (!w) { intern_free(syms); free(iface); return NULL; }

    inf_session *s = malloc(sizeof *s);
    s->syms = syms;
    s->w = w;
    s->iface = iface;
    return s;
}

EXPORT void inf_free(inf_session *s)
{
    if (!s) return;
    world_free(s->w);
    intern_free(s->syms);
    free(s->iface);
    free(s);
}

/* The §6.3 interface artifact for this session's story: the declared
 * vocabulary as JSON. web/gen_binding.mjs reads it and emits the typed host
 * binding, so a client never hand-spells a ground atom. */
EXPORT const char *inf_interface(inf_session *s)
{
    return s->iface ? s->iface : "{}";
}

/* name -> atom id; interns if absent (a fresh id is a fresh always-false atom,
 * which is exactly the silent-typo failure mode a generated binding closes). */
EXPORT unsigned inf_intern(inf_session *s, const char *name)
{
    return intern_id(s->syms, name);
}

EXPORT const char *inf_name(inf_session *s, unsigned atom)
{
    return intern_name(s->syms, atom);
}

/* Defeasible verdict for (atom, neg): 0 UNDECIDED, 1 PROVED, 2 REFUTED
 * (matches dl_verdict). */
EXPORT int inf_query(inf_session *s, unsigned atom, int neg)
{
    dl_lit q = neg ? dl_neg(atom) : dl_pos(atom);
    return (int)world_query(s->w, q);
}

EXPORT int inf_get(inf_session *s, unsigned atom)
{
    return world_get(s->w, atom) ? 1 : 0;
}

EXPORT void inf_set(inf_session *s, unsigned atom, int value)
{
    world_set(s->w, atom, value != 0);
}

/* The value store (§5.8): numeric fluents never become atoms, so they are read
 * and written by value rather than through a verdict. */
EXPORT double inf_get_num(inf_session *s, unsigned atom)
{
    return (double)world_get_num(s->w, atom);
}

EXPORT void inf_set_num(inf_session *s, unsigned atom, double value)
{
    world_set_num(s->w, atom, (long)value);
}

/* Step with an array of occurring action atom ids (a pointer into WASM heap the
 * host allocates). Returns 0 on commit, -1 on an unresolved fluent (name in
 * inf_last_err). */
EXPORT int inf_step(inf_session *s, const unsigned *actions, int nactions)
{
    g_err[0] = '\0';
    return world_step(s->w, actions, nactions, g_err, sizeof g_err);
}

/* Convenience for the common single-action step — no heap marshalling needed. */
EXPORT int inf_step1(inf_session *s, unsigned action)
{
    g_err[0] = '\0';
    return world_step(s->w, &action, 1, g_err, sizeof g_err);
}

/* Subscriptions (§11 M2): the reactive channel. `inf_subscribe` returns a
 * stable handle; `inf_sub_edges` marshals the last step's flips as quadruples
 * [sub, atom, neg, from, to] — five cells each — and returns the size the FULL
 * answer needs, so a short buffer is a grow-and-retry. Verdicts are dl_verdict:
 * 0 undecided, 1 proved, 2 refuted. */
EXPORT int inf_subscribe(inf_session *s, unsigned atom, int neg)
{
    dl_lit q = neg ? dl_neg(atom) : dl_pos(atom);
    return world_subscribe(s->w, q);
}

EXPORT void inf_unsubscribe(inf_session *s, int sub)
{
    world_unsubscribe(s->w, sub);
}

EXPORT int inf_sub_verdict(inf_session *s, int sub)
{
    return (int)world_sub_verdict(s->w, sub);
}

EXPORT int inf_sub_edges(inf_session *s, long *out, int cap)
{
    int n;
    const world_sub_edge *e = world_sub_edges(s->w, &n);
    for (int i = 0; i < n && 5 * i + 4 < cap; i++) {
        out[5 * i]     = e[i].sub;
        out[5 * i + 1] = (long)e[i].lit.atom;
        out[5 * i + 2] = e[i].lit.neg ? 1 : 0;
        out[5 * i + 3] = (long)e[i].from;
        out[5 * i + 4] = (long)e[i].to;
    }
    return 5 * n;
}

/* The step log (#88): the changeset and the commit receipt, marshalled into a
 * caller-provided buffer of `long`. Each returns the number of cells the FULL
 * answer needs, so a short buffer is a grow-and-retry rather than a silent
 * truncation. Layouts are the ones documented on the wf_* shim: pairs
 * [atom, value], triples [atom, from, to], and a receipt header
 * [base, raw, applied, lo, hi, flags, nrows] followed by variable-length rows
 * [op, amount, defeated, pred, nbind, (var, ent) * nbind]. */
EXPORT int inf_bool_deltas(inf_session *s, long *out, int cap)
{
    int n;
    const world_bool_delta *d = world_bool_deltas(s->w, &n);
    for (int i = 0; i < n && 2 * i + 1 < cap; i++) {
        out[2 * i]     = (long)d[i].atom;
        out[2 * i + 1] = d[i].value ? 1 : 0;
    }
    return 2 * n;
}

EXPORT int inf_num_deltas(inf_session *s, long *out, int cap)
{
    int n;
    const world_num_delta *d = world_num_deltas(s->w, &n);
    for (int i = 0; i < n && 3 * i + 2 < cap; i++) {
        out[3 * i]     = (long)d[i].atom;
        out[3 * i + 1] = d[i].from;
        out[3 * i + 2] = d[i].to;
    }
    return 3 * n;
}

EXPORT int inf_num_receipt(inf_session *s, unsigned atom, long *out, int cap)
{
    world_receipt r;
    if (!world_num_receipt(s->w, atom, &r)) return 0;
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

/* Burst cues (#11, §12): the step's transient emission stream. One boundary
 * crossing per step, like the delta buffer — inf_emits hands back a pointer
 * into the engine's linear memory, which JS reads as a zero-copy
 * `new Uint32Array(M.HEAPU32.buffer, ptr, n)` view and resolves to names with
 * inf_name. Valid until the next step. */
EXPORT int inf_emit_count(inf_session *s)
{
    int n;
    world_emits(s->w, &n);
    return n;
}

EXPORT const unsigned *inf_emits(inf_session *s)
{
    int n;
    return world_emits(s->w, &n);
}

EXPORT const char *inf_last_err(void)  { return g_err; }
EXPORT const char *inf_last_diag(void) { return g_diag; }

/* The proof/defeat trace as a string. Captures world_why's FILE* output via
 * open_memstream (POSIX; Emscripten libc provides it). Returns an owned buffer
 * reused each call. */
EXPORT const char *inf_why(inf_session *s, unsigned atom, int neg)
{
    char  *buf = NULL;
    size_t len = 0;
    FILE  *m = open_memstream(&buf, &len);
    if (!m) return "";
    dl_lit q = neg ? dl_neg(atom) : dl_pos(atom);
    world_why(s->w, q, m);
    fclose(m);                 /* flushes; buf now holds the trace */
    free(g_why);
    g_why = buf;
    return g_why ? g_why : "";
}
