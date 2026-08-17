#ifndef INF_RUNTIME_SCENE_H
#define INF_RUNTIME_SCENE_H

#include "core/intern.h"
#include "platform/platform.h"
#include "runtime/iface.h"
#include "state/world.h"

/* A renderer that has never heard of the game it is drawing (DESIGN.md §12).
 *
 * §12's "infeasible cart": a cart written entirely in `.story`, with no host
 * code, drawn by a generic loop that reads what the world concluded. This is
 * that loop, natively. It knows the BLESSED VOCABULARY and nothing else — no
 * cellar, no door, no torch — so pointing it at a different story draws a
 * different game with no edit here:
 *
 *   ax/ay/aw/ah(anchor)      geometry, as derived VALUES (not stored facts)
 *   panel(anchor, style)     a box
 *   caption(anchor, word)    text; the ATOM IS THE LABEL (`w_the_cellar`)
 *   shows(drawable, sprite)  anything's sprite — ONE predicate over a declared
 *                            cover (#231), not one per sort
 *   in_anchor(actor, anchor) actors packed into a region
 *   prop_in(item, anchor)    props packed into a region
 *   held(item, actor)        ...or carried beside a holder
 *   shaded(anchor)           the composite op over a region
 *   gauge(anchor, E)         a bar; the STORY supplies gauge_value/gauge_max
 *                            (derived values) and gauge_low (a judgment)
 *   picked(E) / aimed(E)     the subject, and what a command is aimed at
 *   cue_sound(cue, sound)    an emission plays a sound
 *   cue_word(cue, word)      ...and floats a word
 *
 * Two conventions carry meaning that would otherwise need syntax. **Enum order
 * is meaning**: a `sprite` member's position is its atlas index, and
 * declaration order is already the engine's tie-break (I4), so two clients
 * cannot disagree about what is drawn on top. And **the atom is the label**:
 * `w_the_cellar` prints as "THE CELLAR", which is what a world with no string
 * type buys — one source of truth for UI copy, at the cost of punctuation and
 * of any second language.
 *
 * The scene is rebuilt once per TICK, not per frame: enumerating a predicate
 * means crossing its argument domains and asking, which is noise at a tick and
 * waste at 60fps. Frames only replay the model. */

typedef struct scene scene;

typedef enum { SCENE_NOTHING, SCENE_CMD, SCENE_ENTITY } scene_kind;

typedef struct {
    scene_kind  kind;
    const char *term;        /* SCENE_CMD: the ground action atom, as named */
    bool        ok;          /* ...and whether it applies now */
    dl_lit      blocker;     /* the guard that refused it, when ok is false */
    bool        has_blocker;
    const char *entity;      /* SCENE_ENTITY */
} scene_hit;

scene *scene_new(world *w, intern *syms, const iface *f, plat *p);
void   scene_free(scene *s);

/* Re-derive the model from the world. Once per tick. */
void scene_rebuild(scene *s);

/* Draw the model. `sheet` is the atlas name (the fog composite reads
 * `<sheet>_fog`); `why` is an optional proof trace to overlay. */
void scene_draw(scene *s, const char *sheet, const char *why);

/* The scene's clickable regions as FOCUS TARGETS (§12), in a stable order:
 * menu commands first, then entities. Order is the geometric-navigation
 * tiebreak, so it is semantics rather than presentation (I4). */
int  scene_targets(scene *s, plat_target *out, int cap);
/* What a focus id stands for — the navigable twin of `scene_hit_at`. */
bool scene_target(scene *s, const char *id, scene_hit *out);
bool scene_hit_at(scene *s, int x, int y, scene_hit *out);

/* What clicking an entity submits: the `pick_<e>` / `aim_<e>` action, or NULL.
 * Selection is per-viewer state with no home until scopes exist, so for now it
 * lives in the shared world — which means it has to be an ACTION. */
const char *scene_pick_action(scene *s, const char *entity);

/* The menu, for a driver that wants to inspect rather than hit-test. */
int         scene_menu_count(const scene *s);
const char *scene_menu_label(const scene *s, int i);
const char *scene_menu_term(const scene *s, int i);
bool        scene_menu_ok(const scene *s, int i);

/* Every proved ground pair of a binary judgment — the cue table's shape.
 * Copied into the caller's array, so two lookups do not clobber each other. */
typedef struct { char a[64], b[64]; } scene_pair;
int scene_pairs(scene *s, const char *judgment, scene_pair *out, int cap);

/* `w_the_cellar` -> "THE CELLAR": strip the vocabulary prefix an author uses
 * to keep enum members from colliding, then read the atom as English. */
void scene_say(const char *atom, char *out, size_t cap);

#endif
