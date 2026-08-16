#include "platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A backend that records ops instead of drawing them (DESIGN.md §12).
 *
 * This is what makes the frozen op set testable without a display, and it is
 * the honest second implementation of the interface: a backend that implements
 * every op with no drawing at all cannot accidentally depend on one. A frame
 * is a LIST OF OPS, so "what did the cart draw?" is a question with a data
 * answer rather than a screenshot.
 *
 * Ops are recorded as TEXT — `spr hero 3 12 40 flipx alpha=0.50` — because the
 * assertion a client test wants to write is about what was drawn, and a string
 * comparison says that in one line where a struct comparison says it in
 * fifteen. */

enum { MAX_OPS = 4096, MAX_OPLEN = 160, MAX_AUDIO = 256, MAX_SHEETS = 32 };

struct headless {
    int    w, h;
    char   ops[MAX_OPS][MAX_OPLEN];
    int    nops;
    char   audio[MAX_AUDIO][MAX_OPLEN];
    int    naudio;
    char   sheets[MAX_SHEETS][PLAT_MAX_ID];
    int    nsheets;
    char   text[MAX_OPS * 8];
    int    camera_x, camera_y;
    int32_t cartdata[SPEC_CARTDATA_CELLS];
    plat_raw_input raw;
};

static void rec(headless *h, const char *fmt, ...)
{
    if (h->nops >= MAX_OPS) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->ops[h->nops], MAX_OPLEN, fmt, ap);
    va_end(ap);
    h->nops++;
}

static void rec_audio(headless *h, const char *fmt, ...)
{
    if (h->naudio >= MAX_AUDIO) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->audio[h->naudio], MAX_OPLEN, fmt, ap);
    va_end(ap);
    h->naudio++;
}

/* A sheet must be defined before it is drawn: a typo'd atlas name is a cart
 * bug that a display backend would show as a blank rectangle and this one can
 * name outright. */
static bool known_sheet(headless *h, const char *name)
{
    for (int i = 0; i < h->nsheets; i++)
        if (strcmp(h->sheets[i], name) == 0) return true;
    return false;
}

static void h_begin_frame(void *ctx)
{
    headless *h = ctx;
    h->nops = 0;
    h->camera_x = h->camera_y = 0;
    h->text[0] = 0;
}

static void h_cls(void *ctx, int c) { rec(ctx, "cls %d", c); }
static void h_camera(void *ctx, int x, int y)
{
    headless *h = ctx;
    h->camera_x = x; h->camera_y = y;
    rec(h, "camera %d %d", x, y);
}
static void h_clip(void *ctx, const plat_box *r)
{
    if (r) rec(ctx, "clip %d %d %d %d", r->x, r->y, r->w, r->h);
    else   rec(ctx, "clip reset");
}
static void h_tile(void *ctx, const char *sheet, int i, int x, int y)
{
    headless *h = ctx;
    rec(h, "tile %s %d %d %d%s", sheet, i, x, y,
        known_sheet(h, sheet) ? "" : " [UNDEFINED SHEET]");
}
static void h_spr(void *ctx, const char *sheet, int i, int x, int y, plat_spr_opts o)
{
    headless *h = ctx;
    rec(h, "spr %s %d %d %d%s%s alpha=%.2f%s", sheet, i, x, y,
        o.flip_x ? " flipx" : "", o.flip_y ? " flipy" : "", (double)o.alpha,
        known_sheet(h, sheet) ? "" : " [UNDEFINED SHEET]");
}
static void h_shade(void *ctx, const char *sheet, int i, int x, int y)
{
    headless *h = ctx;
    rec(h, "shade %s %d %d %d%s", sheet, i, x, y,
        known_sheet(h, sheet) ? "" : " [UNDEFINED SHEET]");
}
static void h_print(void *ctx, const char *t, int x, int y, int c, bool big)
{
    headless *h = ctx;
    rec(h, "print %d %d %d %s %s", x, y, c, big ? "big" : "small", t);
    size_t used = strlen(h->text);
    snprintf(h->text + used, sizeof h->text - used, "%s%s", used ? "\n" : "", t);
}
static void h_line(void *ctx, int x0, int y0, int x1, int y1, int c)
{
    rec(ctx, "line %d %d %d %d %d", x0, y0, x1, y1, c);
}
static void h_rect(void *ctx, int x, int y, int w, int h, int c)
{
    rec(ctx, "rect %d %d %d %d %d", x, y, w, h, c);
}
static void h_rectfill(void *ctx, int x, int y, int w, int h, int c)
{
    rec(ctx, "rectfill %d %d %d %d %d", x, y, w, h, c);
}
static void h_circ(void *ctx, int x, int y, int r, int c)
{
    rec(ctx, "circ %d %d %d %d", x, y, r, c);
}
static void h_circfill(void *ctx, int x, int y, int r, int c)
{
    rec(ctx, "circfill %d %d %d %d", x, y, r, c);
}

static void h_sound(void *ctx, const char *id, float g)
{
    rec_audio(ctx, "sound %s %.2f", id, (double)g);
}
static void h_stop(void *ctx, const char *id) { rec_audio(ctx, "stop %s", id); }
static void h_music(void *ctx, const char *id, float g)
{
    rec_audio(ctx, "music %s %.2f", id, (double)g);
}
static void h_music_stop(void *ctx) { rec_audio(ctx, "music_stop"); }

static void h_define_sheet(void *ctx, const char *name, const void *def)
{
    headless *h = ctx;
    (void)def;                            /* an atlas IS whatever a backend says */
    if (h->nsheets < MAX_SHEETS && !known_sheet(h, name))
        snprintf(h->sheets[h->nsheets++], PLAT_MAX_ID, "%s", name);
}

/* Input is scripted, never live: the test driver sets it and the platform
 * samples it at tick boundaries exactly as it samples a display backend's.
 * Arrow keys also drive nav, the way a keyboard backend maps them. */
static void h_read_input(void *ctx, plat_raw_input *out)
{
    headless *h = ctx;
    *out = h->raw;
    for (int d = 0; d < SPEC_NNAV; d++) {
        int k = spec_key_id(SPEC_NAV_DIRS[d]);
        if (k >= 0 && ((h->raw.keys >> k) & 1u)) out->nav |= (uint8_t)(1u << d);
    }
    int enter = spec_key_id("enter"), space = spec_key_id("space"),
        esc   = spec_key_id("escape");
    if (((h->raw.keys >> enter) & 1u) || ((h->raw.keys >> space) & 1u))
        out->confirm = true;
    if ((h->raw.keys >> esc) & 1u) out->cancel = true;
}

static void h_load_cartdata(void *ctx, int32_t *cells)
{
    headless *h = ctx;
    memcpy(cells, h->cartdata, sizeof h->cartdata);
}
static void h_save_cartdata(void *ctx, const int32_t *cells)
{
    headless *h = ctx;
    memcpy(h->cartdata, cells, sizeof h->cartdata);
}

headless *headless_new(int width, int height)
{
    headless *h = calloc(1, sizeof *h);
    h->w = width > 0 ? width : SPEC_DEFAULT_W;
    h->h = height > 0 ? height : SPEC_DEFAULT_H;
    return h;
}

void headless_free(headless *h) { free(h); }

plat_backend headless_backend(headless *h)
{
    plat_backend be;
    memset(&be, 0, sizeof be);
    be.ctx = h;
    be.width = h->w;
    be.height = h->h;
    be.cls = h_cls;             be.camera = h_camera;   be.clip = h_clip;
    be.tile = h_tile;           be.spr = h_spr;         be.shade = h_shade;
    be.print = h_print;         be.line = h_line;       be.rect = h_rect;
    be.rectfill = h_rectfill;   be.circ = h_circ;       be.circfill = h_circfill;
    be.sound = h_sound;         be.stop = h_stop;
    be.music = h_music;         be.music_stop = h_music_stop;
    be.read_input = h_read_input;
    be.begin_frame = h_begin_frame;
    be.define_sheet = h_define_sheet;
    be.load_cartdata = h_load_cartdata;
    be.save_cartdata = h_save_cartdata;
    return be;
}

int headless_op_count(const headless *h) { return h->nops; }

const char *headless_op(const headless *h, int i)
{
    return (i >= 0 && i < h->nops) ? h->ops[i] : NULL;
}

int headless_count_of(const headless *h, const char *op)
{
    size_t n = strlen(op);
    int found = 0;
    for (int i = 0; i < h->nops; i++)
        if (strncmp(h->ops[i], op, n) == 0 &&
            (h->ops[i][n] == ' ' || h->ops[i][n] == 0)) found++;
    return found;
}

const char *headless_text(const headless *h) { return h->text; }

int headless_audio_count(const headless *h) { return h->naudio; }

const char *headless_audio(const headless *h, int i)
{
    return (i >= 0 && i < h->naudio) ? h->audio[i] : NULL;
}

void headless_point(headless *h, int x, int y, const bool *buttons, int nbuttons)
{
    h->raw.x = x;
    h->raw.y = y;
    for (int i = 0; i < SPEC_BUTTONS; i++)
        h->raw.buttons[i] = (buttons && i < nbuttons) ? buttons[i] : false;
}

void headless_press(headless *h, const char *const *keys, int nkeys)
{
    h->raw.keys = 0;
    for (int i = 0; i < nkeys; i++) {
        int k = spec_key_id(keys[i]);
        if (k >= 0) h->raw.keys |= (uint64_t)1u << k;
    }
}

void headless_pad(headless *h, const spec_nav *nav, int nnav, bool confirm, bool cancel)
{
    h->raw.nav = 0;
    for (int i = 0; i < nnav; i++) h->raw.nav |= (uint8_t)(1u << (int)nav[i]);
    h->raw.confirm = confirm;
    h->raw.cancel = cancel;
}
