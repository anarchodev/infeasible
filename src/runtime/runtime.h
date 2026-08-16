#ifndef INF_RUNTIME_RUNTIME_H
#define INF_RUNTIME_RUNTIME_H

#include <stdbool.h>
#include <stdio.h>

#include "core/intern.h"
#include "platform/platform.h"
#include "state/world.h"

/* The loop that joins a cart, a platform backend and a world (DESIGN.md §12).
 *
 * Two clocks, deliberately separated:
 *
 *   TICKS are the world's. One tick = one `world_step`, and the only things
 *   that reach it are the action set the cart returned and the input snapshot
 *   taken at that tick's boundary. The tick count, the action log and the
 *   resulting state are a pure function of each other (I4), which is what
 *   makes a save an action log and a replay exact.
 *
 *   FRAMES are the renderer's. A frame may repaint several times per tick or
 *   skip ticks entirely; interpolation and cue animation are presentation
 *   state and live above the line. Nothing a frame does can change a fact.
 *
 * The wall clock is therefore allowed to decide HOW MANY ticks elapse and
 * never WHAT a tick contains — which is also why the runtime itself never
 * reads a clock: a driver decides when to call `rt_step`. Under lockstep the
 * network's tick schedule takes that seat and nothing about a cart changes. */

typedef struct rt rt;

/* A cart PROPOSES; the source DISPOSES. `tick` writes the ground action atoms
 * it wants submitted and returns how many — live, that is what happens;
 * replaying, the log decides and the proposal is discarded. That seam is what
 * makes `rt_save` a save rather than a recording. */
typedef struct {
    void *ctx;
    void (*init)(void *ctx, rt *r);
    int  (*tick)(void *ctx, rt *r, uint32_t *actions, int cap);
    void (*draw)(void *ctx, rt *r);
    void (*after)(void *ctx, rt *r);
    /* Atlas names registered before the cart runs: loading is above the
     * durability line (a backend decides what an atlas IS), naming one is
     * below it. */
    const char *const *sheets;
    int                nsheets;
} rt_cart;

enum { RT_MAX_ACTIONS = 8 };

/* `story` is the source path the save records, so a log replayed against the
 * wrong world is a refusal rather than a mystery. NULL means "unnamed". */
rt *rt_open(plat *p, world *w, intern *syms, rt_cart cart, const char *story);
void rt_close(rt *r);

/* Advance the world exactly one tick, then repaint (`rt_advance`) or not
 * (`rt_step`). A rejected step — a contested world or a protocol violation —
 * leaves the world unmoved, so the log does not move either; it is reported
 * through `rt_rejected` rather than thrown past the cart, because a client
 * that cannot render "that did not happen" is a client that hides authoring
 * errors. */
void rt_step(rt *r);
void rt_render(rt *r);
void rt_advance(rt *r, int nticks);

world      *rt_world(rt *r);
plat       *rt_platform(rt *r);
intern     *rt_syms(rt *r);
uint64_t    rt_tick_count(const rt *r);
const char *rt_rejected(const rt *r);

/* A save is (engine-hash, game-hash, action-log) — never a state dump: state
 * written outside the log is state replay cannot reproduce, which forfeits
 * shareable playthroughs, branching and time travel in one move. The written
 * form is the one `examples/cellar_play.log` uses, so a save this runtime
 * writes is a save the other clients read. */
int rt_save(const rt *r, FILE *out);

/* Replay a save into THIS world, which must be freshly opened: the log is a
 * history from genesis, so loading into a played world would append a second
 * history to the first. Returns the number of logged ticks, or -1 with `err`
 * filled. The story line is checked when both sides name one. */
int  rt_load(rt *r, const char *text, char *err, size_t errsz);
/* True while a loaded log still has ticks to feed. */
bool rt_replaying(const rt *r);

#endif
