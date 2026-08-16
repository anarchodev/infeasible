#ifndef INF_PLATFORM_PLATFORM_H
#define INF_PLATFORM_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/spec.h"

/* The cart-facing surfaces, assembled over a backend (DESIGN.md §12).
 *
 * A cart reaches exactly these calls and nothing else. That is the durability
 * line drawn in code: the moment content reaches past them for a raw event
 * API, a platform audio stack or a host storage API, a second backend stops
 * implementing a couple of dozen ops and starts reimplementing whichever
 * platform the first one sat on.
 *
 * The draw surface is thin on purpose — validate, then forward to the
 * backend's op of the same name, so the frozen op set is one list (spec.h) and
 * one dispatch. What is NOT thin is input: polling, edge detection, focus
 * navigation and the letterbox inverse live here, below the line, because they
 * are exactly the parts every cart would otherwise reimplement slightly
 * differently — and differently is a portability bug, not a style. */

typedef struct { int x, y, w, h; } plat_box;   /* a clip rectangle */
typedef struct { bool flip_x, flip_y; float alpha; } plat_spr_opts;

/* What a backend can tell the platform about this instant. A backend supplies
 * whatever it HAS: a gamepad fills `nav`/`confirm`/`cancel` and no pointer, a
 * mouse-and-keyboard backend fills all of it, a scripted one fills what a test
 * told it to. Keys and nav are bitsets over the frozen index spaces. */
typedef struct {
    int      x, y;
    bool     buttons[SPEC_BUTTONS];
    uint64_t keys;                 /* bit per SPEC_KEYS index */
    uint8_t  nav;                  /* bit per spec_nav */
    bool     confirm, cancel;
} plat_raw_input;

/* A backend implements the frozen ops and nothing more. Optional hooks are
 * marked; everything else must be present or `plat_open` refuses, naming the
 * op it could not find (a partially-implemented backend that runs is a cart
 * drawing into a hole). */
typedef struct plat_backend {
    void *ctx;
    int   width, height;           /* the chosen internal resolution */

    /* the frozen draw ops */
    void (*cls)(void *ctx, int color);
    void (*camera)(void *ctx, int x, int y);
    void (*clip)(void *ctx, const plat_box *r);      /* NULL resets */
    void (*tile)(void *ctx, const char *sheet, int index, int x, int y);
    void (*spr)(void *ctx, const char *sheet, int index, int x, int y,
                plat_spr_opts opts);
    void (*shade)(void *ctx, const char *sheet, int index, int x, int y);
    void (*print)(void *ctx, const char *text, int x, int y, int color, bool big);
    void (*line)(void *ctx, int x0, int y0, int x1, int y1, int color);
    void (*rect)(void *ctx, int x, int y, int w, int h, int color);
    void (*rectfill)(void *ctx, int x, int y, int w, int h, int color);
    void (*circ)(void *ctx, int x, int y, int r, int color);
    void (*circfill)(void *ctx, int x, int y, int r, int color);

    /* the frozen audio ops — write-only, deliberately: a cart may never ask
     * whether something is playing or how far in it is. Each such readback is
     * wall-clock in disguise, and a rule branching on one is nondeterministic
     * by construction (I4). */
    void (*sound)(void *ctx, const char *id, float gain);
    void (*stop)(void *ctx, const char *id);
    void (*music)(void *ctx, const char *id, float gain);
    void (*music_stop)(void *ctx);

    /* input, sampled ONCE per tick by the platform at the tick boundary. A
     * live backend may treat the call as CONSUMING input, since a click can
     * begin and end between two samples and would otherwise be lost. */
    void (*read_input)(void *ctx, plat_raw_input *out);

    /* optional */
    void (*begin_frame)(void *ctx);
    void (*present)(void *ctx);
    void (*define_sheet)(void *ctx, const char *name, const void *def);
    void (*load_cartdata)(void *ctx, int32_t *cells);
    void (*save_cartdata)(void *ctx, const int32_t *cells);
} plat_backend;

typedef struct plat plat;

/* Open the platform over a backend. Returns NULL and writes why into `err`
 * when the backend is missing a frozen op. */
plat *plat_open(const plat_backend *be, char *err, size_t errsz);
void  plat_close(plat *p);

/* A cart argument error is a bug in the cart. The op is SKIPPED rather than
 * drawn wrong, and the complaint is recorded here for the driver to surface —
 * a renderer that quietly clamps a bad colour teaches an author that the call
 * was fine. Cleared by reading it. */
const char *plat_take_error(plat *p);

/* ---- draw ------------------------------------------------------------------ */

void plat_cls(plat *p, int color);
void plat_camera(plat *p, int x, int y);
void plat_clip(plat *p, int x, int y, int w, int h);
void plat_clip_reset(plat *p);
void plat_tile(plat *p, const char *sheet, int index, int x, int y);
void plat_spr(plat *p, const char *sheet, int index, int x, int y, plat_spr_opts o);
void plat_shade(plat *p, const char *sheet, int index, int x, int y);
void plat_print(plat *p, const char *text, int x, int y, int color, bool big);
void plat_line(plat *p, int x0, int y0, int x1, int y1, int color);
void plat_rect(plat *p, int x, int y, int w, int h, int color);
void plat_rectfill(plat *p, int x, int y, int w, int h, int color);
void plat_circ(plat *p, int x, int y, int r, int color);
void plat_circfill(plat *p, int x, int y, int r, int color);

/* ---- audio ----------------------------------------------------------------- */

void plat_sound(plat *p, const char *id, float gain);
void plat_stop(plat *p, const char *id);
void plat_music(plat *p, const char *id, float gain);
void plat_music_stop(plat *p);

/* ---- input ----------------------------------------------------------------- */

/* The raw surface, for a cart that has decided to be a desktop cart. Pointer
 * and keys are not universal — a gamepad has neither, and a touchscreen has a
 * position only while a finger is down — so a cart reading these is choosing
 * its devices. That is a legitimate thing to choose, which is why they exist. */
void plat_pointer(const plat *p, int *x, int *y);
bool plat_button(const plat *p, int i);
bool plat_pressed(const plat *p, int i);
bool plat_key(plat *p, const char *name);
bool plat_keyp(plat *p, const char *name);

/* ---- focus: the portable input model --------------------------------------- */

/* The cart declares its focusable regions each tick and asks what was
 * confirmed; the platform owns movement, hit-testing and edge detection. A
 * cart built on this runs on a d-pad, a touchscreen and a mouse without
 * knowing which it has — because focus can be driven by any of them, while a
 * pointer can be derived from none of them.
 *
 * Target ORDER is the geometric tiebreak, so it is semantics (I4): two
 * equidistant targets must resolve the same way on every machine and every
 * replay. */
enum { PLAT_MAX_TARGETS = 64, PLAT_MAX_ID = 48 };
typedef struct { const char *id; int x, y, w, h; } plat_target;

void        plat_focus_targets(plat *p, const plat_target *list, int n);
const char *plat_focus_current(const plat *p);
const char *plat_focus_confirmed(const plat *p);
bool        plat_focus_cancelled(const plat *p);

/* ---- persistence ----------------------------------------------------------- */

int32_t plat_data_get(plat *p, int cell);
void    plat_data_set(plat *p, int cell, int32_t value);

/* ---- runtime-facing (not reachable from a cart) ---------------------------- */

/* Sample input for one tick. Called by the runtime at the tick boundary and
 * nowhere else: a cart that could resample mid-tick could observe input the
 * replay never saw. */
void plat_sample_input(plat *p);
void plat_begin_frame(plat *p);
void plat_present(plat *p);
void plat_define_sheet(plat *p, const char *name, const void *def);
const plat_backend *plat_backend_of(const plat *p);

/* ---- the headless backend --------------------------------------------------- */

/* A backend that records ops instead of drawing them, so "what did the cart
 * draw?" has a data answer rather than a screenshot. It is also the honest
 * second implementation of the interface: a backend that implements every op
 * with no drawing at all cannot accidentally depend on a display. */
typedef struct headless headless;

headless     *headless_new(int width, int height);
void          headless_free(headless *h);
plat_backend  headless_backend(headless *h);

/* the frame just recorded, as text: one line per op, arguments in order. The
 * cheapest assertion a client test can make, and enough to pin what a cart
 * said about the world. */
int          headless_op_count(const headless *h);
const char  *headless_op(const headless *h, int i);
int          headless_count_of(const headless *h, const char *op);
/* every `print` this frame, newline-joined */
const char  *headless_text(const headless *h);
/* audio is recorded, never played */
int          headless_audio_count(const headless *h);
const char  *headless_audio(const headless *h, int i);

/* scripted input: what a test told it to report at the next sample */
void headless_point(headless *h, int x, int y, const bool *buttons, int nbuttons);
void headless_press(headless *h, const char *const *keys, int nkeys);
/* Drive the PORTABLE path directly — no pointer, no keyboard, the way a
 * gamepad backend would. Playing a cart through this proves a console can. */
void headless_pad(headless *h, const spec_nav *nav, int nnav, bool confirm, bool cancel);

#endif
