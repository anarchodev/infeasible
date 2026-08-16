#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* one declared focusable region, with its id copied: a cart re-declares its
 * targets every tick, so the platform may not hold a borrowed pointer */
typedef struct { char id[PLAT_MAX_ID]; int x, y, w, h; } plat_slot;

struct plat {
    plat_backend be;
    char         err[160];
    bool         has_err;

    /* the once-per-tick input snapshot and its predecessor — everything a cart
     * reads during a tick is the frozen `cur`, and every edge query is the
     * difference between the two */
    plat_raw_input cur, prev;
    bool           moved;
    uint64_t       gen;

    /* focus: platform state, not cart state. It survives across ticks, which
     * costs replay nothing — what a save records is the action a cart
     * submitted, never the focus that led to it. */
    plat_slot targets[PLAT_MAX_TARGETS];
    int      ntargets;
    char     current[PLAT_MAX_ID];
    char     confirmed[PLAT_MAX_ID];
    bool     has_current, has_confirmed, cancelled;
    uint64_t resolved;

    int32_t cartdata[SPEC_CARTDATA_CELLS];
};

/* Reading a vtable slot by OFFSET needs a pointer type wide enough for a
 * function pointer, which `void *` is not required to be. */
typedef void (*plat_anyfn)(void);

/* A cart argument error is a bug in the cart: record it, skip the op. Drawing
 * something almost-right teaches an author that the call was fine. */
static void bad(plat *p, const char *op, const char *msg)
{
    if (!p->has_err) {
        snprintf(p->err, sizeof p->err, "%s: %s", op, msg);
        p->has_err = true;
    }
}

const char *plat_take_error(plat *p)
{
    if (!p->has_err) return NULL;
    p->has_err = false;
    return p->err;
}

static bool color_ok(plat *p, const char *op, int c)
{
    if (c >= 0 && c < SPEC_NCOLORS) return true;
    char m[96];
    snprintf(m, sizeof m, "color must be a palette index 0..%d, got %d",
             SPEC_NCOLORS - 1, c);
    bad(p, op, m);
    return false;
}

plat *plat_open(const plat_backend *be, char *err, size_t errsz)
{
    if (!be) return NULL;
    const char *raw = (const char *)be;
    for (int i = 0; i < SPEC_NDRAW_OPS; i++) {
        plat_anyfn fn;
        memcpy(&fn, raw + SPEC_DRAW_OPS[i].off, sizeof fn);
        if (!fn) {
            if (err) snprintf(err, errsz,
                              "backend is missing the frozen draw op '%s'",
                              SPEC_DRAW_OPS[i].name);
            return NULL;
        }
    }
    for (int i = 0; i < SPEC_NAUDIO_OPS; i++) {
        plat_anyfn fn;
        memcpy(&fn, raw + SPEC_AUDIO_OPS[i].off, sizeof fn);
        if (!fn) {
            if (err) snprintf(err, errsz,
                              "backend is missing the frozen audio op '%s'",
                              SPEC_AUDIO_OPS[i].name);
            return NULL;
        }
    }
    if (!be->read_input) {
        if (err) snprintf(err, errsz, "backend cannot report input");
        return NULL;
    }
    plat *p = calloc(1, sizeof *p);
    p->be = *be;
    if (be->load_cartdata) be->load_cartdata(be->ctx, p->cartdata);
    return p;
}

void plat_close(plat *p) { free(p); }

const plat_backend *plat_backend_of(const plat *p) { return &p->be; }

/* ---- draw ------------------------------------------------------------------ */

void plat_cls(plat *p, int color)
{
    if (color_ok(p, "cls", color)) p->be.cls(p->be.ctx, color);
}

void plat_camera(plat *p, int x, int y) { p->be.camera(p->be.ctx, x, y); }

void plat_clip(plat *p, int x, int y, int w, int h)
{
    plat_box r = { x, y, w, h };
    p->be.clip(p->be.ctx, &r);
}

void plat_clip_reset(plat *p) { p->be.clip(p->be.ctx, NULL); }

void plat_tile(plat *p, const char *sheet, int index, int x, int y)
{
    if (!sheet) { bad(p, "tile", "sheet name is required"); return; }
    p->be.tile(p->be.ctx, sheet, index, x, y);
}

void plat_spr(plat *p, const char *sheet, int index, int x, int y, plat_spr_opts o)
{
    if (!sheet) { bad(p, "spr", "sheet name is required"); return; }
    if (o.alpha < 0.0f || o.alpha > 1.0f) { bad(p, "spr", "alpha must be 0..1"); return; }
    p->be.spr(p->be.ctx, sheet, index, x, y, o);
}

void plat_shade(plat *p, const char *sheet, int index, int x, int y)
{
    if (!sheet) { bad(p, "shade", "sheet name is required"); return; }
    p->be.shade(p->be.ctx, sheet, index, x, y);
}

void plat_print(plat *p, const char *text, int x, int y, int color, bool big)
{
    if (!text) { bad(p, "print", "text is required"); return; }
    if (color_ok(p, "print", color)) p->be.print(p->be.ctx, text, x, y, color, big);
}

void plat_line(plat *p, int x0, int y0, int x1, int y1, int color)
{
    if (color_ok(p, "line", color)) p->be.line(p->be.ctx, x0, y0, x1, y1, color);
}

void plat_rect(plat *p, int x, int y, int w, int h, int color)
{
    if (color_ok(p, "rect", color)) p->be.rect(p->be.ctx, x, y, w, h, color);
}

void plat_rectfill(plat *p, int x, int y, int w, int h, int color)
{
    if (color_ok(p, "rectfill", color)) p->be.rectfill(p->be.ctx, x, y, w, h, color);
}

void plat_circ(plat *p, int x, int y, int r, int color)
{
    if (color_ok(p, "circ", color)) p->be.circ(p->be.ctx, x, y, r, color);
}

void plat_circfill(plat *p, int x, int y, int r, int color)
{
    if (color_ok(p, "circfill", color)) p->be.circfill(p->be.ctx, x, y, r, color);
}

/* ---- audio ----------------------------------------------------------------- */

void plat_sound(plat *p, const char *id, float gain)
{
    if (!id) { bad(p, "sound", "id is required"); return; }
    p->be.sound(p->be.ctx, id, gain);
}
void plat_stop(plat *p, const char *id)
{
    if (!id) { bad(p, "stop", "id is required"); return; }
    p->be.stop(p->be.ctx, id);
}
void plat_music(plat *p, const char *id, float gain)
{
    if (!id) { bad(p, "music", "id is required"); return; }
    p->be.music(p->be.ctx, id, gain);
}
void plat_music_stop(plat *p) { p->be.music_stop(p->be.ctx); }

/* ---- input ----------------------------------------------------------------- */

void plat_sample_input(plat *p)
{
    plat_raw_input raw;
    memset(&raw, 0, sizeof raw);
    p->be.read_input(p->be.ctx, &raw);
    p->prev = p->cur;
    p->cur = raw;
    p->moved = p->cur.x != p->prev.x || p->cur.y != p->prev.y;
    p->gen++;
}

void plat_pointer(const plat *p, int *x, int *y)
{
    if (x) *x = p->cur.x;
    if (y) *y = p->cur.y;
}

bool plat_button(const plat *p, int i)
{
    return i >= 0 && i < SPEC_BUTTONS && p->cur.buttons[i];
}

bool plat_pressed(const plat *p, int i)
{
    return i >= 0 && i < SPEC_BUTTONS && p->cur.buttons[i] && !p->prev.buttons[i];
}

bool plat_key(plat *p, const char *name)
{
    int k = spec_key_id(name);
    if (k < 0) { bad(p, "key", "not a frozen key name"); return false; }
    return (p->cur.keys >> k) & 1u;
}

bool plat_keyp(plat *p, const char *name)
{
    int k = spec_key_id(name);
    if (k < 0) { bad(p, "keyp", "not a frozen key name"); return false; }
    return ((p->cur.keys >> k) & 1u) && !((p->prev.keys >> k) & 1u);
}

/* ---- focus ----------------------------------------------------------------- */

static int target_index(const plat *p, const char *id)
{
    if (!id) return -1;
    for (int i = 0; i < p->ntargets; i++)
        if (strcmp(p->targets[i].id, id) == 0) return i;
    return -1;
}

/* Which target lies `dir` of `from`? The shortest RECT-TO-RECT distance, not
 * centre-to-centre, which matters whenever the targets are not all the same
 * size: a wide button's centre can be far from a small neighbour whose edge is
 * touching it, and centre distance picks the wrong one.
 *
 * Direction is filtered first, by whether any part of the candidate lies
 * beyond the source along the axis of travel. Ties break on DECLARATION ORDER,
 * which is why target order is semantics (I4): two equidistant targets must
 * resolve the same way on every machine and every replay. */
static int navigate(const plat *p, int from, spec_nav dir)
{
    if (from < 0) return p->ntargets ? 0 : -1;
    bool axis_x = dir == SPEC_NAV_LEFT || dir == SPEC_NAV_RIGHT;
    int sign = (dir == SPEC_NAV_RIGHT || dir == SPEC_NAV_DOWN) ? 1 : -1;
    const plat_slot *f = &p->targets[from];
    int flo = sign * (axis_x ? f->x : f->y);
    int fhi = sign * (axis_x ? f->x + f->w : f->y + f->h);
    int beyond = flo > fhi ? flo : fhi;

    int best = -1;
    long best_d = -1;
    for (int i = 0; i < p->ntargets; i++) {
        if (i == from) continue;
        const plat_slot *t = &p->targets[i];
        int lo = sign * (axis_x ? t->x : t->y);
        int hi = sign * (axis_x ? t->x + t->w : t->y + t->h);
        /* MAX of the two edges, so an overlapping candidate that extends
         * further in the direction of travel still counts as being that way */
        if ((lo > hi ? lo : hi) <= beyond) continue;
        /* gap between the rectangles on each axis; 0 when they overlap there */
        int gx1 = f->x - (t->x + t->w), gx2 = t->x - (f->x + f->w);
        int gy1 = f->y - (t->y + t->h), gy2 = t->y - (f->y + f->h);
        int gx = gx1 > gx2 ? gx1 : gx2, gy = gy1 > gy2 ? gy1 : gy2;
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        long d = (long)gx * gx + (long)gy * gy;
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return best >= 0 ? best : from;      /* nothing that way: stay put */
}

/* Resolution happens on DECLARATION, because the platform cannot navigate a
 * list it has not been given and the cart declares and reads inside one tick.
 * Guarded by the input generation so a cart that declares twice does not get
 * two d-pad steps out of one press. */
static void focus_resolve(plat *p)
{
    if (p->resolved == p->gen) return;
    p->resolved = p->gen;
    p->has_confirmed = false;
    p->cancelled = p->cur.cancel && !p->prev.cancel;

    int cur = p->has_current ? target_index(p, p->current) : -1;

    for (int d = 0; d < SPEC_NNAV; d++) {           /* edge-triggered, like keyp */
        bool now = (p->cur.nav >> d) & 1u, was = (p->prev.nav >> d) & 1u;
        if (now && !was) cur = navigate(p, cur, (spec_nav)d);
    }

    /* A pointer sets focus only when it MOVES. Otherwise a stale cursor
     * resting on a button would fight the d-pad for focus every tick. */
    int over = -1;
    for (int i = 0; i < p->ntargets; i++) {
        const plat_slot *t = &p->targets[i];
        if (p->cur.x >= t->x && p->cur.x < t->x + t->w &&
            p->cur.y >= t->y && p->cur.y < t->y + t->h) { over = i; break; }
    }
    if (p->moved && over >= 0) cur = over;

    bool clicked = p->cur.buttons[0] && !p->prev.buttons[0];
    int confirmed = -1;
    if (clicked) {
        if (over >= 0) { cur = over; confirmed = over; }
    } else if (p->cur.confirm && !p->prev.confirm) {
        confirmed = cur;
    }

    if (cur >= 0) {
        snprintf(p->current, sizeof p->current, "%s", p->targets[cur].id);
        p->has_current = true;
    } else {
        p->has_current = false;
    }
    if (confirmed >= 0) {
        snprintf(p->confirmed, sizeof p->confirmed, "%s", p->targets[confirmed].id);
        p->has_confirmed = true;
    }
}

void plat_focus_targets(plat *p, const plat_target *list, int n)
{
    if (n > PLAT_MAX_TARGETS) { bad(p, "focus.targets", "too many targets"); n = PLAT_MAX_TARGETS; }
    p->ntargets = 0;
    for (int i = 0; i < n; i++) {
        if (!list[i].id) { bad(p, "focus.targets", "a target needs an id"); continue; }
        int k = p->ntargets++;
        snprintf(p->targets[k].id, sizeof p->targets[k].id, "%s", list[i].id);
        p->targets[k].x = list[i].x;
        p->targets[k].y = list[i].y;
        p->targets[k].w = list[i].w;
        p->targets[k].h = list[i].h;
    }
    /* a focused thing that stopped existing hands focus to the first remaining
     * target rather than to nothing: a d-pad user with no focus has no way
     * back in */
    if (!p->has_current || target_index(p, p->current) < 0) {
        if (p->ntargets) {
            snprintf(p->current, sizeof p->current, "%s", p->targets[0].id);
            p->has_current = true;
        } else {
            p->has_current = false;
        }
    }
    focus_resolve(p);
}

const char *plat_focus_current(const plat *p)
{
    return p->has_current ? p->current : NULL;
}
const char *plat_focus_confirmed(const plat *p)
{
    return p->has_confirmed ? p->confirmed : NULL;
}
bool plat_focus_cancelled(const plat *p) { return p->cancelled; }

/* ---- persistence ----------------------------------------------------------- */

int32_t plat_data_get(plat *p, int cell)
{
    if (cell < 0 || cell >= SPEC_CARTDATA_CELLS) {
        bad(p, "data.get", "cell out of range");
        return 0;
    }
    return p->cartdata[cell];
}

void plat_data_set(plat *p, int cell, int32_t value)
{
    if (cell < 0 || cell >= SPEC_CARTDATA_CELLS) {
        bad(p, "data.set", "cell out of range");
        return;
    }
    p->cartdata[cell] = value;
    if (p->be.save_cartdata) p->be.save_cartdata(p->be.ctx, p->cartdata);
}

/* ---- frame ----------------------------------------------------------------- */

void plat_begin_frame(plat *p) { if (p->be.begin_frame) p->be.begin_frame(p->be.ctx); }
void plat_present(plat *p)     { if (p->be.present) p->be.present(p->be.ctx); }
void plat_define_sheet(plat *p, const char *name, const void *def)
{
    if (p->be.define_sheet) p->be.define_sheet(p->be.ctx, name, def);
}
