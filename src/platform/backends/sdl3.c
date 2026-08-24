#include "platform/display.h"
#include "platform/spec.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The desktop display backend (DESIGN.md §12, §13's console rider).
 *
 * SDL3 is the platform layer, chosen because the console ports of the seam
 * above this file already exist in that shape: same API, the platform half
 * dropped in out of tree at build time under NDA. Everything SDL is asked for
 * here is window, input, gamepad and audio — never a scene graph, never a
 * sprite batcher, never a UI toolkit, because the frozen op set is the whole
 * contract and a backend that reaches past it is a backend the next platform
 * cannot match.
 *
 * TWO THINGS ARE DELIBERATELY OURS RATHER THAN SDL'S.
 *
 * The letterbox: SDL_SetRenderLogicalPresentation does the scaling, but the
 * INVERSE — a window point back to an internal pixel — is computed with
 * `spec_letterbox`/`spec_to_internal` rather than SDL_RenderCoordinatesFrom
 * Window. One arithmetic, in one place, identical on every backend; a pointer
 * that lands on a different pixel per platform is the portability bug §12 puts
 * that code below the line to prevent.
 *
 * The font: §12 freezes text as METRICS, not glyphs, so the cell sizes are the
 * spec's and the letter shapes are this file's business. The 3x5 set below is
 * stretched to the large cell rather than authored twice, which is the whole
 * point of freezing the metric instead of the bitmap.
 *
 * ASSETS ARE NOT CODE, and the format is open (§13). Until it lands, a sheet
 * this backend has never been given is drawn as a generated placeholder atlas
 * — a distinct palette block per index — so a story with sprites is playable
 * before there is a pipeline, and nothing about the op set changes when there
 * is one. */

enum { MAX_SHEETS = 16, BLIP_HZ = 48000, MAX_VOICES = 8 };

typedef struct {
    char         name[32];
    SDL_Texture *tex;
    int          cell, cols;
} sheet;

typedef struct { int freq, left; } voice;   /* a blip, in samples remaining */

struct display {
    SDL_Window   *win;
    SDL_Renderer *ren;
    int           w, h;                     /* the internal resolution */
    sheet         sheets[MAX_SHEETS];
    int           nsheets;
    int           cam_x, cam_y;
    bool          quit;
    SDL_Gamepad  *pad;
    SDL_AudioStream *audio;
    voice         voices[MAX_VOICES];
};

/* ---- the font: 3x5, stretched to the large cell ----------------------------- */

/* Rows are 3 bits, top to bottom. Uppercase only: the presentation vocabulary
 * reads atoms as English and upper-cases them, and a lowercase set would be
 * bytes spent on text this layer never receives. */
static const unsigned char GLYPH[][5] = {
    { 0,0,0,0,0 },                                    /* space */
    { 2,2,2,0,2 },  /* ! */  { 5,5,0,0,0 },  /* " */
    { 5,7,5,7,5 },  /* # */  { 3,6,3,6,3 },  /* $ */
    { 5,1,2,4,5 },  /* % */  { 2,5,2,5,3 },  /* & */
    { 2,2,0,0,0 },  /* ' */  { 1,2,2,2,1 },  /* ( */
    { 4,2,2,2,4 },  /* ) */  { 5,2,7,2,5 },  /* * */
    { 0,2,7,2,0 },  /* + */  { 0,0,0,2,4 },  /* , */
    { 0,0,7,0,0 },  /* - */  { 0,0,0,0,2 },  /* . */
    { 1,1,2,4,4 },  /* / */
    { 7,5,5,5,7 },  /* 0 */  { 2,6,2,2,7 },  /* 1 */
    { 7,1,7,4,7 },  /* 2 */  { 7,1,3,1,7 },  /* 3 */
    { 5,5,7,1,1 },  /* 4 */  { 7,4,7,1,7 },  /* 5 */
    { 7,4,7,5,7 },  /* 6 */  { 7,1,1,1,1 },  /* 7 */
    { 7,5,7,5,7 },  /* 8 */  { 7,5,7,1,7 },  /* 9 */
    { 0,2,0,2,0 },  /* : */  { 0,2,0,2,4 },  /* ; */
    { 1,2,4,2,1 },  /* < */  { 0,7,0,7,0 },  /* = */
    { 4,2,1,2,4 },  /* > */  { 7,1,3,0,2 },  /* ? */
    { 7,5,7,4,3 },  /* @ */
    { 2,5,7,5,5 },  /* A */  { 6,5,6,5,6 },  /* B */
    { 3,4,4,4,3 },  /* C */  { 6,5,5,5,6 },  /* D */
    { 7,4,6,4,7 },  /* E */  { 7,4,6,4,4 },  /* F */
    { 3,4,5,5,3 },  /* G */  { 5,5,7,5,5 },  /* H */
    { 7,2,2,2,7 },  /* I */  { 1,1,1,5,2 },  /* J */
    { 5,5,6,5,5 },  /* K */  { 4,4,4,4,7 },  /* L */
    { 5,7,7,5,5 },  /* M */  { 5,7,5,5,5 },  /* N */
    { 2,5,5,5,2 },  /* O */  { 6,5,6,4,4 },  /* P */
    { 2,5,5,7,3 },  /* Q */  { 6,5,6,5,5 },  /* R */
    { 3,4,2,1,6 },  /* S */  { 7,2,2,2,2 },  /* T */
    { 5,5,5,5,7 },  /* U */  { 5,5,5,5,2 },  /* V */
    { 5,5,7,7,5 },  /* W */  { 5,5,2,5,5 },  /* X */
    { 5,5,2,2,2 },  /* Y */  { 7,1,2,4,7 },  /* Z */
};
enum { GLYPH_FIRST = ' ', GLYPH_LAST = 'Z' };

static void set_color(SDL_Renderer *r, int idx, Uint8 alpha)
{
    uint32_t c = SPEC_PALETTE[idx & (SPEC_NCOLORS - 1)];
    SDL_SetRenderDrawColor(r, (Uint8)(c >> 16), (Uint8)(c >> 8), (Uint8)c, alpha);
}

/* Draw one glyph by stretching the 3x5 cell into the requested box — nearest
 * neighbour, so a large capital is the same letter and not a second font. */
static void glyph(SDL_Renderer *r, int ch, int x, int y, int w, int h)
{
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if (ch < GLYPH_FIRST || ch > GLYPH_LAST) return;
    const unsigned char *g = GLYPH[ch - GLYPH_FIRST];
    for (int gy = 0; gy < 5; gy++)
        for (int gx = 0; gx < 3; gx++) {
            if (!((g[gy] >> (2 - gx)) & 1)) continue;
            SDL_FRect px = { (float)(x + gx * w / 3), (float)(y + gy * h / 5),
                             (float)((w + 2) / 3), (float)((h + 4) / 5) };
            SDL_RenderFillRect(r, &px);
        }
}

/* ---- the frozen draw ops ------------------------------------------------------ */

static void d_cls(void *ctx, int c)
{
    display *d = ctx;
    d->cam_x = d->cam_y = 0;
    set_color(d->ren, c, 255);
    SDL_RenderClear(d->ren);
}

static void d_camera(void *ctx, int x, int y)
{
    display *d = ctx;
    d->cam_x = x;
    d->cam_y = y;
}

static void d_clip(void *ctx, const plat_box *b)
{
    display *d = ctx;
    if (!b) { SDL_SetRenderClipRect(d->ren, NULL); return; }
    SDL_Rect r = { b->x - d->cam_x, b->y - d->cam_y, b->w, b->h };
    SDL_SetRenderClipRect(d->ren, &r);
}

static sheet *find_sheet(display *d, const char *name)
{
    for (int i = 0; i < d->nsheets; i++)
        if (strcmp(d->sheets[i].name, name) == 0) return &d->sheets[i];
    return NULL;
}

/* The placeholder atlas: one palette block per index, with a darker notch so
 * two adjacent indices are visibly different. Replaced wholesale when the
 * asset format lands (§13) — the op set does not change when it does. */
static SDL_Texture *placeholder_atlas(SDL_Renderer *r, int cell, int cols, bool fog)
{
    int w = cell * cols, h = cell;
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, w, h);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, fog ? SDL_BLENDMODE_MOD : SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(r, t);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    for (int i = 0; i < cols; i++) {
        if (fog) {
            /* a dither the composite op multiplies through */
            for (int y = 0; y < cell; y++)
                for (int x = 0; x < cell; x++) {
                    bool on = ((x + y) & 1) == 0;
                    SDL_SetRenderDrawColor(r, on ? 90 : 190, on ? 90 : 190,
                                           on ? 110 : 210, 255);
                    SDL_FRect px = { (float)(i * cell + x), (float)y, 1, 1 };
                    SDL_RenderFillRect(r, &px);
                }
            continue;
        }
        set_color(r, 6 + (i % 8), 255);
        SDL_FRect body = { (float)(i * cell + 2), 2, (float)(cell - 4), (float)(cell - 4) };
        SDL_RenderFillRect(r, &body);
        set_color(r, 1, 255);
        SDL_FRect notch = { (float)(i * cell + 3), 3, (float)(1 + i % 4), 2 };
        SDL_RenderFillRect(r, &notch);
    }
    SDL_SetRenderTarget(r, NULL);
    return t;
}

static sheet *ensure_sheet(display *d, const char *name)
{
    sheet *s = find_sheet(d, name);
    if (s) return s;
    if (d->nsheets >= MAX_SHEETS) return NULL;
    s = &d->sheets[d->nsheets++];
    snprintf(s->name, sizeof s->name, "%s", name);
    bool fog = strstr(name, "_fog") != NULL;
    s->cell = fog ? 8 : 16;
    s->cols = fog ? 1 : 16;
    s->tex = placeholder_atlas(d->ren, s->cell, s->cols, fog);
    return s;
}

static void blit(display *d, const char *name, int index, int x, int y,
                 bool flip_x, bool flip_y, float alpha)
{
    sheet *s = ensure_sheet(d, name);
    if (!s || !s->tex) return;
    SDL_SetTextureAlphaMod(s->tex, (Uint8)(alpha * 255.0f));
    SDL_FRect src = { (float)((index % s->cols) * s->cell), 0,
                      (float)s->cell, (float)s->cell };
    SDL_FRect dst = { (float)(x - d->cam_x), (float)(y - d->cam_y),
                      (float)s->cell, (float)s->cell };
    SDL_FlipMode flip = (SDL_FlipMode)((flip_x ? SDL_FLIP_HORIZONTAL : 0) |
                                       (flip_y ? SDL_FLIP_VERTICAL : 0));
    SDL_RenderTextureRotated(d->ren, s->tex, &src, &dst, 0.0, NULL, flip);
}

static void d_tile(void *ctx, const char *sh, int i, int x, int y)
{
    blit(ctx, sh, i, x, y, false, false, 1.0f);
}

static void d_spr(void *ctx, const char *sh, int i, int x, int y, plat_spr_opts o)
{
    blit(ctx, sh, i, x, y, o.flip_x, o.flip_y, o.alpha);
}

/* THE composite op: the fog tile multiplied over what is already there, which
 * is what makes vision and darkness one op instead of a shader. */
static void d_shade(void *ctx, const char *sh, int i, int x, int y)
{
    blit(ctx, sh, i, x, y, false, false, 1.0f);
}

static void d_print(void *ctx, const char *text, int x, int y, int c, bool big)
{
    display *d = ctx;
    set_color(d->ren, c, 255);
    int cw = big ? SPEC_BIG_GLYPH_W : SPEC_GLYPH_W;
    int ch = big ? SPEC_BIG_GLYPH_H : SPEC_GLYPH_H;
    for (int i = 0; text[i]; i++)
        glyph(d->ren, (unsigned char)text[i], x - d->cam_x + i * cw, y - d->cam_y,
              cw - 1, ch - 1);
}

static void d_line(void *ctx, int x0, int y0, int x1, int y1, int c)
{
    display *d = ctx;
    set_color(d->ren, c, 255);
    SDL_RenderLine(d->ren, (float)(x0 - d->cam_x), (float)(y0 - d->cam_y),
                   (float)(x1 - d->cam_x), (float)(y1 - d->cam_y));
}

static void d_rect(void *ctx, int x, int y, int w, int h, int c)
{
    display *d = ctx;
    set_color(d->ren, c, 255);
    SDL_FRect r = { (float)(x - d->cam_x), (float)(y - d->cam_y), (float)w, (float)h };
    SDL_RenderRect(d->ren, &r);
}

static void d_rectfill(void *ctx, int x, int y, int w, int h, int c)
{
    display *d = ctx;
    set_color(d->ren, c, 255);
    SDL_FRect r = { (float)(x - d->cam_x), (float)(y - d->cam_y), (float)w, (float)h };
    SDL_RenderFillRect(d->ren, &r);
}

/* midpoint circle, by hand: SDL has no circle and a game's circles are two ops,
 * not a reason to link a geometry library */
static void circle(display *d, int cx, int cy, int r, int c, bool fill)
{
    set_color(d->ren, c, 255);
    cx -= d->cam_x;
    cy -= d->cam_y;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (fill) {
            SDL_FRect a = { (float)(cx - x), (float)(cy + y), (float)(2 * x + 1), 1 };
            SDL_FRect b = { (float)(cx - x), (float)(cy - y), (float)(2 * x + 1), 1 };
            SDL_FRect e = { (float)(cx - y), (float)(cy + x), (float)(2 * y + 1), 1 };
            SDL_FRect f = { (float)(cx - y), (float)(cy - x), (float)(2 * y + 1), 1 };
            SDL_RenderFillRect(d->ren, &a); SDL_RenderFillRect(d->ren, &b);
            SDL_RenderFillRect(d->ren, &e); SDL_RenderFillRect(d->ren, &f);
        } else {
            float pts[16][2] = {
                { (float)(cx + x), (float)(cy + y) }, { (float)(cx - x), (float)(cy + y) },
                { (float)(cx + x), (float)(cy - y) }, { (float)(cx - x), (float)(cy - y) },
                { (float)(cx + y), (float)(cy + x) }, { (float)(cx - y), (float)(cy + x) },
                { (float)(cx + y), (float)(cy - x) }, { (float)(cx - y), (float)(cy - x) },
            };
            for (int i = 0; i < 8; i++) SDL_RenderPoint(d->ren, pts[i][0], pts[i][1]);
        }
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void d_circ(void *ctx, int x, int y, int r, int c)     { circle(ctx, x, y, r, c, false); }
static void d_circfill(void *ctx, int x, int y, int r, int c) { circle(ctx, x, y, r, c, true); }

/* ---- audio -------------------------------------------------------------------- */

/* A blip per `sound`, pitched by the id — enough to prove the seam and to hear
 * a cue fire. The asset format is open (§13), so nothing here decodes anything:
 * when samples land, this file gains a decoder and the op set does not move. */
static void mix(void *ctx, SDL_AudioStream *stream, int additional, int total)
{
    display *d = ctx;
    (void)total;
    int frames = additional / (int)sizeof(float);
    if (frames <= 0 || frames > 4096) frames = 512;
    static float buf[4096];
    static int phase;
    for (int i = 0; i < frames; i++) {
        float v = 0;
        for (int k = 0; k < MAX_VOICES; k++) {
            if (d->voices[k].left <= 0) continue;
            int period = BLIP_HZ / (d->voices[k].freq ? d->voices[k].freq : 440);
            v += ((phase + i) % period < period / 2) ? 0.08f : -0.08f;
            d->voices[k].left--;
        }
        buf[i] = v;
    }
    phase += frames;
    SDL_PutAudioStreamData(stream, buf, frames * (int)sizeof(float));
}

static void voice_on(display *d, const char *id)
{
    unsigned h = 0;
    for (const char *p = id; *p; p++) h = h * 31u + (unsigned char)*p;
    for (int k = 0; k < MAX_VOICES; k++) {
        if (d->voices[k].left > 0) continue;
        d->voices[k].freq = 220 + (int)(h % 700);
        d->voices[k].left = BLIP_HZ / 12;
        return;
    }
}

static void d_sound(void *ctx, const char *id, float gain)
{
    if (gain > 0.0f) voice_on(ctx, id);
}
static void d_stop(void *ctx, const char *id)
{
    display *d = ctx;
    (void)id;
    for (int k = 0; k < MAX_VOICES; k++) d->voices[k].left = 0;
}
static void d_music(void *ctx, const char *id, float gain) { (void)ctx; (void)id; (void)gain; }
static void d_music_stop(void *ctx) { (void)ctx; }

/* ---- input --------------------------------------------------------------------- */

/* SDL scancodes -> the frozen key names, by index. Never a raw code past this
 * table: a scancode in a cart is the platform leaking through the interface. */
static const SDL_Scancode KEYMAP[SPEC_NKEYS] = {
    SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E,
    SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J,
    SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
    SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y,
    SDL_SCANCODE_Z,
    SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9,
    SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_TAB,
    SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT,
};

static void d_read_input(void *ctx, plat_raw_input *out)
{
    display *d = ctx;
    memset(out, 0, sizeof *out);

    const bool *keys = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < SPEC_NKEYS; i++)
        if (keys[KEYMAP[i]]) out->keys |= (uint64_t)1u << i;

    float mx = 0, my = 0;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
    /* OUR letterbox inverse, not SDL's: one arithmetic in one place, so a
     * pointer lands on the same internal pixel on every backend */
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(d->win, &ww, &wh);
    spec_box box = spec_letterbox(ww, wh, d->w, d->h);
    spec_to_internal((int)mx, (int)my, box, d->w, d->h, &out->x, &out->y);
    out->buttons[0] = (buttons & SDL_BUTTON_LMASK) != 0;
    out->buttons[1] = (buttons & SDL_BUTTON_MMASK) != 0;
    out->buttons[2] = (buttons & SDL_BUTTON_RMASK) != 0;

    /* the portable half: arrows, a gamepad d-pad or its stick all become the
     * same four intents, and confirm/cancel are one button each */
    if (keys[SDL_SCANCODE_LEFT])  out->nav |= 1u << SPEC_NAV_LEFT;
    if (keys[SDL_SCANCODE_RIGHT]) out->nav |= 1u << SPEC_NAV_RIGHT;
    if (keys[SDL_SCANCODE_UP])    out->nav |= 1u << SPEC_NAV_UP;
    if (keys[SDL_SCANCODE_DOWN])  out->nav |= 1u << SPEC_NAV_DOWN;
    out->confirm = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE];
    out->cancel  = keys[SDL_SCANCODE_ESCAPE];

    if (d->pad) {
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  out->nav |= 1u << SPEC_NAV_LEFT;
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) out->nav |= 1u << SPEC_NAV_RIGHT;
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_DPAD_UP))    out->nav |= 1u << SPEC_NAV_UP;
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  out->nav |= 1u << SPEC_NAV_DOWN;
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_SOUTH)) out->confirm = true;
        if (SDL_GetGamepadButton(d->pad, SDL_GAMEPAD_BUTTON_EAST))  out->cancel = true;
    }
}

/* ---- persistence ---------------------------------------------------------------- */

static void cartdata_path(char *buf, size_t cap)
{
    char *pref = SDL_GetPrefPath("infeasible", "runtime");
    snprintf(buf, cap, "%scartdata.bin", pref ? pref : "");
    if (pref) SDL_free(pref);
}

static void d_load_cartdata(void *ctx, int32_t *cells)
{
    (void)ctx;
    char path[512];
    cartdata_path(path, sizeof path);
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (!data) return;
    if (n > sizeof(int32_t) * SPEC_CARTDATA_CELLS) n = sizeof(int32_t) * SPEC_CARTDATA_CELLS;
    memcpy(cells, data, n);
    SDL_free(data);
}

static void d_save_cartdata(void *ctx, const int32_t *cells)
{
    (void)ctx;
    char path[512];
    cartdata_path(path, sizeof path);
    SDL_SaveFile(path, cells, sizeof(int32_t) * SPEC_CARTDATA_CELLS);
}

/* ---- frame ----------------------------------------------------------------------- */

static void d_begin_frame(void *ctx)
{
    display *d = ctx;
    d->cam_x = d->cam_y = 0;
    SDL_SetRenderClipRect(d->ren, NULL);
}

static void d_present(void *ctx)
{
    display *d = ctx;
    SDL_RenderPresent(d->ren);
}

static void d_define_sheet(void *ctx, const char *name, const void *def)
{
    (void)def;                       /* what an atlas IS belongs to the backend */
    ensure_sheet(ctx, name);
}

/* ---- open / close ------------------------------------------------------------------ */

display *display_open(const char *title, int width, int height, int scale,
                      char *err, size_t errsz)
{
    if (scale < 1) scale = 1;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        if (err) snprintf(err, errsz, "SDL_Init: %s", SDL_GetError());
        return NULL;
    }
    display *d = calloc(1, sizeof *d);
    d->w = width;
    d->h = height;
    if (!SDL_CreateWindowAndRenderer(title ? title : "infeasible",
                                     width * scale, height * scale,
                                     SDL_WINDOW_RESIZABLE, &d->win, &d->ren)) {
        if (err) snprintf(err, errsz, "SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        free(d);
        SDL_Quit();
        return NULL;
    }
    /* the letterbox, in the renderer: a wider window shows bars, never more
     * map — under lockstep that is a fairness rule, not taste */
    SDL_SetRenderLogicalPresentation(d->ren, width, height,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderDrawBlendMode(d->ren, SDL_BLENDMODE_BLEND);

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, BLIP_HZ };
    d->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix, d);
    if (d->audio) SDL_ResumeAudioStreamDevice(d->audio);

    int npads = 0;
    SDL_JoystickID *pads = SDL_GetGamepads(&npads);
    if (pads && npads > 0) d->pad = SDL_OpenGamepad(pads[0]);
    if (pads) SDL_free(pads);
    return d;
}

void display_close(display *d)
{
    if (!d) return;
    for (int i = 0; i < d->nsheets; i++)
        if (d->sheets[i].tex) SDL_DestroyTexture(d->sheets[i].tex);
    if (d->pad) SDL_CloseGamepad(d->pad);
    if (d->audio) SDL_DestroyAudioStream(d->audio);
    SDL_DestroyRenderer(d->ren);
    SDL_DestroyWindow(d->win);
    SDL_Quit();
    free(d);
}

void display_pump(display *d)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) d->quit = true;
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !d->pad)
            d->pad = SDL_OpenGamepad(e.gdevice.which);
    }
}

bool display_quit_requested(const display *d) { return d->quit; }

bool display_capture(display *d, const char *path)
{
    SDL_Surface *shot = SDL_RenderReadPixels(d->ren, NULL);
    if (!shot) return false;
    bool ok = SDL_SaveBMP(shot, path);
    SDL_DestroySurface(shot);
    return ok;
}
const char *display_name(void) { return "sdl3"; }

plat_backend display_backend(display *d)
{
    plat_backend be;
    memset(&be, 0, sizeof be);
    be.ctx = d;
    be.width = d->w;
    be.height = d->h;
    be.cls = d_cls;           be.camera = d_camera;     be.clip = d_clip;
    be.tile = d_tile;         be.spr = d_spr;           be.shade = d_shade;
    be.print = d_print;       be.line = d_line;         be.rect = d_rect;
    be.rectfill = d_rectfill; be.circ = d_circ;         be.circfill = d_circfill;
    be.sound = d_sound;       be.stop = d_stop;
    be.music = d_music;       be.music_stop = d_music_stop;
    be.read_input = d_read_input;
    be.begin_frame = d_begin_frame;
    be.present = d_present;
    be.define_sheet = d_define_sheet;
    be.load_cartdata = d_load_cartdata;
    be.save_cartdata = d_save_cartdata;
    return be;
}
