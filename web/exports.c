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

typedef struct { intern *syms; world *w; } inf_session;

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
    world *w = story_compile(src, syms, &diags);

    /* surface every diagnostic (errors and warnings) to the host */
    size_t off = 0;
    int shown = diags.count < diags.cap ? diags.count : diags.cap;
    for (int i = 0; i < shown && off < sizeof g_diag - 1; i++)
        off += (size_t)snprintf(g_diag + off, sizeof g_diag - off, "%s:%d:%d: %s\n",
                                items[i].sev == STORY_ERROR ? "error" : "warning",
                                items[i].line, items[i].col, items[i].msg);

    if (!w) { intern_free(syms); return NULL; }

    inf_session *s = malloc(sizeof *s);
    s->syms = syms;
    s->w = w;
    return s;
}

EXPORT void inf_free(inf_session *s)
{
    if (!s) return;
    world_free(s->w);
    intern_free(s->syms);
    free(s);
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
