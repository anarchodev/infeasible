#include "runtime/purecart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PC_MAXFX = 8, PC_WHY = 4096, PC_MAXTARGETS = PLAT_MAX_TARGETS };

typedef struct { char text[48]; int life; } floater;

struct purecart {
    world  *w;
    intern *syms;
    const iface *f;
    plat   *p;
    scene  *sc;
    char    sheet[32];
    char    why[PC_WHY];
    floater fx[PC_MAXFX];
    int     nfx;
};

purecart *purecart_new(world *w, intern *syms, const iface *f, plat *p,
                       const char *sheet)
{
    purecart *c = calloc(1, sizeof *c);
    c->w = w; c->syms = syms; c->f = f; c->p = p;
    c->sc = scene_new(w, syms, f, p);
    snprintf(c->sheet, sizeof c->sheet, "%s", sheet ? sheet : "main");
    return c;
}

void purecart_free(purecart *c)
{
    if (!c) return;
    scene_free(c->sc);
    free(c);
}

scene *purecart_scene(purecart *c) { return c->sc; }
const char *purecart_why(const purecart *c) { return c->why; }

/* The why-trace as text. `world_why` writes to a FILE*, and a runtime that
 * wants the string takes the standard-C route rather than a POSIX memory
 * stream — the native player has to build where `open_memstream` does not
 * exist. */
static void why_string(purecart *c, dl_lit q)
{
    c->why[0] = 0;
    FILE *tmp = tmpfile();
    if (!tmp) return;
    world_why(c->w, q, tmp);
    fflush(tmp);
    rewind(tmp);
    size_t n = fread(c->why, 1, sizeof c->why - 1, tmp);
    c->why[n] = 0;
    fclose(tmp);
}

static void pc_init(void *ctx, rt *r)
{
    purecart *c = ctx;
    (void)r;
    scene_rebuild(c->sc);
}

static int pc_tick(void *ctx, rt *r, uint32_t *out, int cap)
{
    purecart *c = ctx;
    (void)r;
    if (cap < 1) return 0;

    /* The PORTABLE input model (§12): declare what is focusable, ask what was
     * confirmed. This driver never reads a pointer or a key, so it plays
     * identically on a mouse, a touchscreen and a d-pad — and a cart that
     * reached for the pointer here would have been a desktop-only cart with
     * nothing saying so. */
    plat_target targets[PC_MAXTARGETS];
    int n = scene_targets(c->sc, targets, PC_MAXTARGETS);
    plat_focus_targets(c->p, targets, n);
    const char *id = plat_focus_confirmed(c->p);
    if (!id) return 0;

    scene_hit hit;
    if (!scene_target(c->sc, id, &hit)) { c->why[0] = 0; return 0; }

    if (hit.kind == SCENE_ENTITY) {
        c->why[0] = 0;
        const char *term = scene_pick_action(c->sc, hit.entity);
        if (!term) return 0;
        out[0] = intern_id(c->syms, term);
        return 1;
    }
    if (hit.ok) {
        c->why[0] = 0;
        /* the ground action IS the term the engine named; nothing to bind */
        out[0] = intern_id(c->syms, hit.term);
        return 1;
    }
    /* A refused command explains itself, and the literal to explain is the
     * GUARD the engine says refused it — the world's own argument, not a
     * message this file invented or a judgment the story wrote to mirror it. */
    if (hit.has_blocker) why_string(c, hit.blocker);
    else c->why[0] = 0;
    return 0;
}

static void pc_after(void *ctx, rt *r)
{
    purecart *c = ctx;
    /* Cues are a LOOKUP, not a handler per event: the story already said which
     * sound a cue plays and which word it floats, so this reads the table it
     * declared. An emission `heave(guard)` is the cue `q_heave`. */
    scene_pair sounds[64], words[64];
    int ns = scene_pairs(c->sc, "cue_sound", sounds, 64);
    int nw = scene_pairs(c->sc, "cue_word", words, 64);

    int nem = 0;
    const uint32_t *em = world_emits(rt_world(r), &nem);
    for (int i = 0; i < nem; i++) {
        const char *name = intern_name(c->syms, em[i]);
        char cue[64];
        size_t k = 0;
        cue[k++] = 'q'; cue[k++] = '_';
        for (size_t j = 0; name[j] && name[j] != '(' && k + 1 < sizeof cue; j++)
            cue[k++] = name[j];
        cue[k] = 0;
        for (int j = 0; j < ns; j++)
            if (strcmp(sounds[j].a, cue) == 0) plat_sound(c->p, sounds[j].b, 0.6f);
        for (int j = 0; j < nw; j++) {
            if (strcmp(words[j].a, cue) != 0) continue;
            if (c->nfx >= PC_MAXFX) continue;
            scene_say(words[j].b, c->fx[c->nfx].text, sizeof c->fx[0].text);
            c->fx[c->nfx].life = 40;
            c->nfx++;
        }
    }
    scene_rebuild(c->sc);
}

static void pc_draw(void *ctx, rt *r)
{
    purecart *c = ctx;
    (void)r;
    scene_draw(c->sc, c->sheet, c->why);
    int shown = 0;
    for (int i = 0; i < c->nfx; i++) {
        plat_print(c->p, c->fx[i].text, 300,
                   200 - (40 - c->fx[i].life) / 2 - shown * 10, 6, true);
        c->fx[i].life--;
        shown++;
    }
    int keep = 0;
    for (int i = 0; i < c->nfx; i++)
        if (c->fx[i].life > 0) c->fx[keep++] = c->fx[i];
    c->nfx = keep;
}

rt_cart purecart_cart(purecart *c)
{
    rt_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.ctx = c;
    cart.init = pc_init;
    cart.tick = pc_tick;
    cart.draw = pc_draw;
    cart.after = pc_after;
    return cart;
}
