#include "platform/spec.h"
#include "platform/platform.h"

#include <string.h>

const spec_res SPEC_RESOLUTIONS[SPEC_NRESOLUTIONS] = {
    { 320, 180 }, { 480, 270 }, { 640, 360 }, { 960, 540 },
};

const uint32_t SPEC_PALETTE[SPEC_NCOLORS] = {
    0x0b0d10, 0x1d2433, 0x3b4b63, 0x6b7f99,   /*  0-3  ink → mist   */
    0xc3cfdd, 0xf2f0e6, 0xe8c37a, 0xc98a3e,   /*  4-7  bone → brass */
    0x8a5a2b, 0x5a3a22, 0xa33b3b, 0xe06666,   /*  8-11 wood → blood */
    0x4c8c4a, 0x8fd45a, 0x3f6fa8, 0x79b8e8,   /* 12-15 moss → sky   */
};

/* The op tables carry the OFFSET of each function pointer, so the presence
 * check walks this list rather than a hand-written sequence of NULL tests —
 * one place to add an op, and adding one everywhere is then mechanical. */
#define DRAW_OP(n) { #n, offsetof(plat_backend, n) }

const spec_op SPEC_DRAW_OPS[SPEC_NDRAW_OPS] = {
    DRAW_OP(cls),      /* cls(color)                     — clear to a palette index */
    DRAW_OP(camera),   /* camera(x, y)                   — translate every later op */
    DRAW_OP(clip),     /* clip(rect) | clip(NULL)        — restrict later ops */
    DRAW_OP(tile),     /* tile(sheet, index, x, y)       — atlas blit, the map layer */
    DRAW_OP(spr),      /* spr(sheet, index, x, y, opts)  — sprite: flip, alpha */
    DRAW_OP(shade),    /* shade(sheet, index, x, y)      — THE composite op (fog) */
    DRAW_OP(print),    /* print(text, x, y, color, big)  — text: one of two cells */
    DRAW_OP(line),
    DRAW_OP(rect),
    DRAW_OP(rectfill),
    DRAW_OP(circ),
    DRAW_OP(circfill),
};

const spec_op SPEC_AUDIO_OPS[SPEC_NAUDIO_OPS] = {
    DRAW_OP(sound), DRAW_OP(stop), DRAW_OP(music), DRAW_OP(music_stop),
};

const char *const SPEC_INPUT_OPS[SPEC_NINPUT_OPS] = {
    "pointer", "button", "pressed", "key", "keyp",
};
const char *const SPEC_FOCUS_OPS[SPEC_NFOCUS_OPS] = {
    "targets", "current", "confirmed", "cancelled",
};
const char *const SPEC_DATA_OPS[SPEC_NDATA_OPS] = { "get", "set" };

const char *const SPEC_NAV_DIRS[SPEC_NNAV] = { "left", "right", "up", "down" };

const char *const SPEC_KEYS[SPEC_NKEYS] = {
    "left", "right", "up", "down",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "space", "enter", "escape", "tab", "shift", "ctrl", "alt",
};

int spec_key_id(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < SPEC_NKEYS; i++)
        if (strcmp(SPEC_KEYS[i], name) == 0) return i;
    return -1;
}

int spec_text_width(const char *s, bool big)
{
    size_t n = s ? strlen(s) : 0;
    return (int)n * (big ? SPEC_BIG_GLYPH_W : SPEC_GLYPH_W);
}

spec_box spec_letterbox(int display_w, int display_h, int internal_w, int internal_h)
{
    int sx = internal_w > 0 ? display_w / internal_w : 1;
    int sy = internal_h > 0 ? display_h / internal_h : 1;
    int scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    spec_box b;
    b.scale = scale;
    b.w = internal_w * scale;
    b.h = internal_h * scale;
    /* floor division of a possibly negative surplus: a window smaller than one
     * internal pixel per pixel still centres, it just centres negatively */
    int dx = display_w - b.w, dy = display_h - b.h;
    b.x = dx >= 0 ? dx / 2 : -((-dx + 1) / 2);
    b.y = dy >= 0 ? dy / 2 : -((-dy + 1) / 2);
    return b;
}

static int floor_div(int a, int b)
{
    int q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

void spec_to_internal(int px, int py, spec_box box, int internal_w, int internal_h,
                      int *out_x, int *out_y)
{
    int scale = box.scale > 0 ? box.scale : 1;
    int x = floor_div(px - box.x, scale);
    int y = floor_div(py - box.y, scale);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > internal_w - 1) x = internal_w - 1;
    if (y > internal_h - 1) y = internal_h - 1;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}
