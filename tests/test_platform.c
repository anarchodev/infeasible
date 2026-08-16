/* Golden test for the native platform tier (DESIGN.md §12).
 *
 * §12's claim is that presentation is a frozen, PICO-8-sized swap surface, so
 * a second runtime is a weekend and not a rewrite. This file is the first
 * place that claim is load-bearing rather than aspirational: the same freeze
 * now has two implementations in two languages, and the interesting failure is
 * no longer "does it draw" but "do the two agree".
 *
 * So the first case reads `web/platform/spec.mjs` and compares it, name by
 * name, against `src/platform/spec.c`. Neither is generated from the other —
 * they are different languages in different builds — and a freeze copied into
 * two files is a freeze that drifts. What keeps them honest is that
 * disagreeing fails here.
 *
 * The rest pins the parts §12 puts BELOW the line on purpose, because they are
 * what every cart would otherwise reimplement slightly differently: the
 * letterbox and its inverse, once-per-tick input sampling with edge detection,
 * and focus navigation (geometric, rect-to-rect, ties broken by declaration
 * order — which is why target order is semantics, I4).
 *
 * The last case is the one that makes it a runtime rather than a library: the
 * native loop replays `examples/cellar_play.log` — the save the browser cart
 * produced by clicking — and lands in the world `tests/test_secondclient.c`
 * asserts. Three clients, one save file. */

#include "platform/platform.h"
#include "platform/spec.h"
#include "runtime/runtime.h"
#include "lang/story.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif
#ifndef WEB_DIR
#define WEB_DIR "web"
#endif

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)sz + 1);
    size_t rd = fread(s, 1, (size_t)sz, f); s[rd] = 0;
    fclose(f);
    return s;
}

/* ---- the freeze, in two languages ------------------------------------------ */

/* Pull the single-quoted strings out of `export const NAME = [ … ]`. The
 * parsing is deliberately dumb: the point is to read the OTHER spelling of the
 * freeze, not to understand JavaScript. */
static int js_list(const char *src, const char *name, char out[][32], int cap)
{
    char needle[64];
    snprintf(needle, sizeof needle, "export const %s = [", name);
    const char *p = strstr(src, needle);
    if (!p) return -1;
    p += strlen(needle);
    int n = 0;
    for (; *p && *p != ']'; p++) {
        if (*p != '\'') continue;
        const char *end = strchr(p + 1, '\'');
        if (!end) break;
        if (n < cap && end - p - 1 < 31) {
            memcpy(out[n], p + 1, (size_t)(end - p - 1));
            out[n][end - p - 1] = 0;
            n++;
        }
        p = end;
    }
    return n;
}

static long js_int(const char *src, const char *name)
{
    char needle[64];
    snprintf(needle, sizeof needle, "export const %s = ", name);
    const char *p = strstr(src, needle);
    if (!p) return -1;
    return strtol(p + strlen(needle), NULL, 10);
}

static int check_spec_drift(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/platform/spec.mjs", WEB_DIR);
    char *js = slurp(path);
    CHECK(js != NULL);

    char names[80][32];
    int n = js_list(js, "DRAW_OPS", names, 80);
    CHECK(n == SPEC_NDRAW_OPS);
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], SPEC_DRAW_OPS[i].name) != 0) {
            fprintf(stderr, "FAIL draw op %d: C says '%s', spec.mjs says '%s'\n",
                    i, SPEC_DRAW_OPS[i].name, names[i]);
            return 1;
        }

    n = js_list(js, "AUDIO_OPS", names, 80);
    CHECK(n == SPEC_NAUDIO_OPS);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_AUDIO_OPS[i].name) == 0);

    n = js_list(js, "INPUT_OPS", names, 80);
    CHECK(n == SPEC_NINPUT_OPS);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_INPUT_OPS[i]) == 0);

    n = js_list(js, "FOCUS_OPS", names, 80);
    CHECK(n == SPEC_NFOCUS_OPS);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_FOCUS_OPS[i]) == 0);

    n = js_list(js, "DATA_OPS", names, 80);
    CHECK(n == SPEC_NDATA_OPS);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_DATA_OPS[i]) == 0);

    n = js_list(js, "NAV_DIRS", names, 80);
    CHECK(n == SPEC_NNAV);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_NAV_DIRS[i]) == 0);

    n = js_list(js, "KEYS", names, 80);
    CHECK(n == SPEC_NKEYS);
    for (int i = 0; i < n; i++) CHECK(strcmp(names[i], SPEC_KEYS[i]) == 0);

    /* the palette, which two backends must agree on to the byte or the same
     * cart looks like a different game */
    n = js_list(js, "PALETTE", names, 80);
    CHECK(n == SPEC_NCOLORS);
    for (int i = 0; i < n; i++) {
        CHECK(names[i][0] == '#');
        CHECK((uint32_t)strtoul(names[i] + 1, NULL, 16) == SPEC_PALETTE[i]);
    }

    CHECK(js_int(js, "CARTDATA_CELLS") == SPEC_CARTDATA_CELLS);
    CHECK(js_int(js, "BUTTONS") == SPEC_BUTTONS);
    CHECK(js_int(js, "GLYPH_W") == SPEC_GLYPH_W);
    CHECK(js_int(js, "GLYPH_H") == SPEC_GLYPH_H);
    CHECK(js_int(js, "BIG_GLYPH_W") == SPEC_BIG_GLYPH_W);
    CHECK(js_int(js, "BIG_GLYPH_H") == SPEC_BIG_GLYPH_H);

    free(js);
    printf("  the freeze agrees across two languages: %d draw + %d audio ops, "
           "%d keys, %d colours\n", SPEC_NDRAW_OPS, SPEC_NAUDIO_OPS,
           SPEC_NKEYS, SPEC_NCOLORS);
    return 0;
}

/* ---- a cart, for the runtime cases ------------------------------------------ */

typedef struct {
    plat     *p;
    uint32_t  want[8];       /* what to propose, one action per tick */
    int       nwant, at;
    int       draws;
} scripted;

static int scripted_tick(void *ctx, rt *r, uint32_t *out, int cap)
{
    (void)r; (void)cap;
    scripted *s = ctx;
    if (s->at >= s->nwant) return 0;
    out[0] = s->want[s->at++];
    return 1;
}

static void scripted_draw(void *ctx, rt *r)
{
    scripted *s = ctx;
    (void)r;
    s->draws++;
    plat_cls(s->p, 0);
    plat_print(s->p, "cellar", 4, 4, 5, false);
}

int main(void)
{
    if (check_spec_drift()) return 1;

    /* ---- the letterbox, and its inverse ----------------------------------- */
    {
        spec_box b = spec_letterbox(1920, 1080, 640, 360);
        CHECK(b.scale == 3 && b.w == 1920 && b.h == 1080 && b.x == 0 && b.y == 0);
        b = spec_letterbox(1600, 1000, 640, 360);
        CHECK(b.scale == 2 && b.w == 1280 && b.h == 720);
        CHECK(b.x == 160 && b.y == 140);
        /* the round trip: a display point inside the box comes back as the
         * internal pixel it was drawn from */
        int ix, iy;
        spec_to_internal(b.x + 2 * 100 + 1, b.y + 2 * 50 + 1, b, 640, 360, &ix, &iy);
        CHECK(ix == 100 && iy == 50);
        /* and outside it clamps rather than reporting a pixel that is not
         * there — every cart computing this itself is every cart getting the
         * edges wrong */
        spec_to_internal(0, 0, b, 640, 360, &ix, &iy);
        CHECK(ix == 0 && iy == 0);
        spec_to_internal(5000, 5000, b, 640, 360, &ix, &iy);
        CHECK(ix == 639 && iy == 359);
        /* every blessed resolution integer-scales into 1080p */
        for (int i = 0; i < SPEC_NRESOLUTIONS; i++) {
            spec_box r = spec_letterbox(1920, 1080, SPEC_RESOLUTIONS[i].w,
                                        SPEC_RESOLUTIONS[i].h);
            CHECK(r.w == 1920 && r.h == 1080);
        }
        CHECK(spec_text_width("trace", false) == 5 * SPEC_GLYPH_W);
        CHECK(spec_text_width("trace", true) == 5 * SPEC_BIG_GLYPH_W);
        printf("  letterbox: integer scale, centred, and the inverse clamps\n");
    }

    /* ---- a backend missing an op is refused, by name ---------------------- */
    {
        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        be.circfill = NULL;
        char err[160] = "";
        CHECK(plat_open(&be, err, sizeof err) == NULL);
        CHECK(strstr(err, "circfill") != NULL);
        headless_free(h);
        printf("  a partially-implemented backend is refused: %s\n", err);
    }

    /* ---- draw: validate, then forward ------------------------------------- */
    {
        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        char err[160];
        plat *p = plat_open(&be, err, sizeof err);
        CHECK(p != NULL);

        plat_begin_frame(p);
        plat_cls(p, 1);
        plat_camera(p, 8, 4);
        plat_print(p, "why?", 2, 2, 5, false);
        plat_print(p, "HP 12", 2, 12, 6, true);
        plat_rectfill(p, 0, 0, 10, 10, 3);
        plat_circ(p, 5, 5, 2, 4);
        CHECK(headless_op_count(h) == 6);
        CHECK(strcmp(headless_op(h, 0), "cls 1") == 0);
        CHECK(strcmp(headless_op(h, 1), "camera 8 4") == 0);
        CHECK(strcmp(headless_text(h), "why?\nHP 12") == 0);
        CHECK(headless_count_of(h, "print") == 2);

        /* a cart argument error is a bug in the cart: the op is SKIPPED rather
         * than drawn wrong, and it says so */
        plat_cls(p, 99);
        CHECK(headless_op_count(h) == 6);
        const char *e = plat_take_error(p);
        CHECK(e && strstr(e, "palette index") != NULL);
        CHECK(plat_take_error(p) == NULL);          /* reading clears it */

        /* audio is recorded, never played — and never asked about */
        plat_sound(p, "clunk", 1.0f);
        plat_music(p, "cellar", 0.5f);
        CHECK(headless_audio_count(h) == 2);
        CHECK(strcmp(headless_audio(h, 0), "sound clunk 1.00") == 0);

        /* persistence is one small blob, and it is not the save */
        plat_data_set(p, 3, 42);
        CHECK(plat_data_get(p, 3) == 42);
        plat_data_get(p, 999);
        CHECK(plat_take_error(p) != NULL);

        plat_close(p);
        headless_free(h);
        printf("  draw: ops forwarded in order, a bad argument refused and named\n");
    }

    /* ---- input is sampled once per tick, and edges are the difference ------ */
    {
        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        char err[160];
        plat *p = plat_open(&be, err, sizeof err);
        CHECK(p != NULL);

        const char *keys[] = { "space" };
        headless_press(h, keys, 1);
        plat_sample_input(p);
        CHECK(plat_key(p, "space") && plat_keyp(p, "space"));
        /* held: still down, no longer an edge */
        plat_sample_input(p);
        CHECK(plat_key(p, "space") && !plat_keyp(p, "space"));
        headless_press(h, NULL, 0);
        plat_sample_input(p);
        CHECK(!plat_key(p, "space"));
        /* a key outside the frozen set is a cart bug, not a false */
        CHECK(!plat_key(p, "F13"));
        CHECK(plat_take_error(p) != NULL);

        bool down[3] = { true, false, false };
        headless_point(h, 20, 30, down, 3);
        plat_sample_input(p);
        int x, y;
        plat_pointer(p, &x, &y);
        CHECK(x == 20 && y == 30);
        CHECK(plat_button(p, 0) && plat_pressed(p, 0));
        plat_sample_input(p);
        CHECK(plat_button(p, 0) && !plat_pressed(p, 0));

        plat_close(p);
        headless_free(h);
        printf("  input: one snapshot per tick, edges from the previous one\n");
    }

    /* ---- focus: the portable model, driven with no pointer at all ---------- */
    {
        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        char err[160];
        plat *p = plat_open(&be, err, sizeof err);
        CHECK(p != NULL);

        /* three buttons in a row, and a wide one below them. The wide one is
         * the case that decides the algorithm: its CENTRE is far from every
         * button in the row, while its edge is directly under each of them, so
         * a centre-to-centre metric navigates down to the wrong place and a
         * rect-to-rect one does not. */
        const plat_target row[] = {
            { "take",  10, 10, 40, 12 },
            { "drop",  60, 10, 40, 12 },
            { "look", 110, 10, 40, 12 },
            { "wide",  10, 40, 140, 12 },
        };

        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(strcmp(plat_focus_current(p), "take") == 0);   /* first by default */

        spec_nav right = SPEC_NAV_RIGHT;
        headless_pad(h, &right, 1, false, false);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(strcmp(plat_focus_current(p), "drop") == 0);

        /* held, not re-pressed: nav is edge-triggered like keyp, so focus
         * stays put rather than sliding across the row */
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(strcmp(plat_focus_current(p), "drop") == 0);

        headless_pad(h, NULL, 0, false, false);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        spec_nav down = SPEC_NAV_DOWN;
        headless_pad(h, &down, 1, false, false);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(strcmp(plat_focus_current(p), "wide") == 0);

        /* confirm, with no pointer anywhere near it — a console can play this */
        headless_pad(h, NULL, 0, true, false);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(plat_focus_confirmed(p) && strcmp(plat_focus_confirmed(p), "wide") == 0);
        /* held confirm does not fire twice */
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(plat_focus_confirmed(p) == NULL);

        headless_pad(h, NULL, 0, false, true);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(plat_focus_cancelled(p));

        /* a focused thing that stops existing hands focus to the first
         * remaining target: a d-pad user with no focus has no way back in */
        headless_pad(h, NULL, 0, false, false);
        plat_sample_input(p);
        plat_focus_targets(p, row, 3);
        CHECK(strcmp(plat_focus_current(p), "take") == 0);

        /* the pointer path reaches the same surface: a click sets focus AND
         * confirms, so one cart serves both devices */
        bool click[3] = { true, false, false };
        headless_point(h, 70, 15, click, 3);
        plat_sample_input(p);
        plat_focus_targets(p, row, 4);
        CHECK(strcmp(plat_focus_current(p), "drop") == 0);
        CHECK(plat_focus_confirmed(p) && strcmp(plat_focus_confirmed(p), "drop") == 0);

        plat_close(p);
        headless_free(h);
        printf("  focus: navigated by pad, confirmed with no pointer, and by click\n");
    }

    /* ---- THE RUNTIME: a save the browser cart wrote, replayed natively ----- */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/cellar_play.story", STORY_DIR);
        char *src = slurp(path);
        CHECK(src != NULL);
        snprintf(path, sizeof path, "%s/cellar_play.log", STORY_DIR);
        char *log = slurp(path);
        CHECK(log != NULL);

        intern *syms = intern_new();
        story_diag di[32];
        story_diags dg = { di, 32, 0, 0 };
        world *w = story_compile(src, "cellar_play.story", syms, &dg);
        CHECK(w != NULL && dg.nerrors == 0);

        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        char err[160];
        plat *p = plat_open(&be, err, sizeof err);
        CHECK(p != NULL);

        scripted s = { .p = p };
        rt_cart cart = { .ctx = &s, .tick = scripted_tick, .draw = scripted_draw };
        rt *r = rt_open(p, w, syms, cart, "examples/cellar_play.story");
        CHECK(r != NULL);

        int ticks = rt_load(r, log, err, sizeof err);
        CHECK(ticks == 10);
        rt_advance(r, ticks);
        CHECK(rt_tick_count(r) == 10);
        CHECK(!rt_replaying(r));
        CHECK(s.draws == 10);              /* a frame per tick, and no fact moved */

        /* the world the OTHER two clients reached (tests/test_secondclient.c) */
        CHECK(world_query(w, dl_pos(intern_id(syms, "at(hero)=vault"))) == DL_PROVED);
        CHECK(world_query(w, dl_pos(intern_id(syms, "door=open"))) == DL_PROVED);
        CHECK(world_get_num(w, intern_id(syms, "hp(guard)")) == 10);
        CHECK(world_query(w, dl_pos(intern_id(syms, "weakened(hero)"))) == DL_REFUTED);
        printf("  the runtime replays the cart's own save and lands in its world\n");

        /* and writes one back in the form the others read */
        char tmp[] = "/tmp/inf_rt_save_XXXXXX";
        int fd = mkstemp(tmp);
        CHECK(fd >= 0);
        FILE *out = fdopen(fd, "w+");
        CHECK(out != NULL);
        CHECK(rt_save(r, out) == 10);
        fflush(out);
        rewind(out);
        char *written = slurp(tmp);
        CHECK(written != NULL);
        CHECK(strstr(written, "story examples/cellar_play.story") != NULL);
        CHECK(strstr(written, "take(hero,antidote,vault)") != NULL);
        fclose(out);
        remove(tmp);

        /* THE PROPERTY: a save is an action log, so replaying what we just
         * wrote into a FRESH world lands in the same place. */
        {
            intern *s2 = intern_new();
            story_diag d2[32];
            story_diags g2 = { d2, 32, 0, 0 };
            world *w2 = story_compile(src, "cellar_play.story", s2, &g2);
            CHECK(w2 != NULL && g2.nerrors == 0);
            headless *h2 = headless_new(640, 360);
            plat_backend be2 = headless_backend(h2);
            plat *p2 = plat_open(&be2, err, sizeof err);
            scripted s2c = { .p = p2 };
            rt_cart c2 = { .ctx = &s2c, .tick = scripted_tick, .draw = scripted_draw };
            rt *r2 = rt_open(p2, w2, s2, c2, "examples/cellar_play.story");
            CHECK(rt_load(r2, written, err, sizeof err) == 10);
            rt_advance(r2, 10);
            CHECK(world_query(w2, dl_pos(intern_id(s2, "at(hero)=vault"))) == DL_PROVED);
            CHECK(world_get_num(w2, intern_id(s2, "hp(guard)")) == 10);

            /* a log is a history from genesis, so it may not be loaded into a
             * world that already has one */
            CHECK(rt_load(r2, written, err, sizeof err) == -1);
            CHECK(strstr(err, "fresh world") != NULL);
            rt_close(r2); plat_close(p2); headless_free(h2);
            world_free(w2); intern_free(s2);
        }

        /* a save from another story is refused rather than replayed into
         * atoms that mean something else now */
        {
            intern *s3 = intern_new();
            story_diag d3[32];
            story_diags g3 = { d3, 32, 0, 0 };
            world *w3 = story_compile(src, "cellar_play.story", s3, &g3);
            headless *h3 = headless_new(640, 360);
            plat_backend be3 = headless_backend(h3);
            plat *p3 = plat_open(&be3, err, sizeof err);
            scripted s3c = { .p = p3 };
            rt_cart c3 = { .ctx = &s3c, .tick = scripted_tick, .draw = scripted_draw };
            rt *r3 = rt_open(p3, w3, s3, c3, "examples/cellar_play.story");
            CHECK(rt_load(r3, "story examples/duel_pure.story\n", err, sizeof err) == -1);
            CHECK(strstr(err, "duel_pure.story") != NULL);
            rt_close(r3); plat_close(p3); headless_free(h3);
            world_free(w3); intern_free(s3);
        }

        free(written);
        rt_close(r);
        plat_close(p);
        headless_free(h);
        world_free(w);
        intern_free(syms);
        free(src);
        free(log);
    }

    /* ---- a LIVE cart's proposals become the log --------------------------- */
    {
        char path[512];
        snprintf(path, sizeof path, "%s/cellar_play.story", STORY_DIR);
        char *src = slurp(path);
        CHECK(src != NULL);
        intern *syms = intern_new();
        story_diag di[32];
        story_diags dg = { di, 32, 0, 0 };
        world *w = story_compile(src, "cellar_play.story", syms, &dg);
        CHECK(w != NULL);

        headless *h = headless_new(640, 360);
        plat_backend be = headless_backend(h);
        char err[160];
        plat *p = plat_open(&be, err, sizeof err);
        scripted s = { .p = p, .nwant = 2 };
        s.want[0] = intern_id(syms, "go_hall(hero)");
        s.want[1] = intern_id(syms, "force_door(hero)");   /* the hero cannot */
        rt_cart cart = { .ctx = &s, .tick = scripted_tick, .draw = scripted_draw };
        rt *r = rt_open(p, w, syms, cart, "examples/cellar_play.story");

        rt_advance(r, 1);
        CHECK(world_query(w, dl_pos(intern_id(syms, "at(hero)=hall"))) == DL_PROVED);
        /* An action whose guard fails is not a rejected step: the world moved
         * nothing and said so quietly, which is a different thing from a
         * contested one — and it is still a tick that happened. */
        rt_advance(r, 1);
        CHECK(rt_rejected(r) == NULL);
        CHECK(world_query(w, dl_pos(intern_id(syms, "door=open"))) == DL_REFUTED);

        char tmp[] = "/tmp/inf_rt_live_XXXXXX";
        int fd = mkstemp(tmp);
        FILE *out = fdopen(fd, "w+");
        CHECK(rt_save(r, out) == 2);       /* both ticks are in the log */
        fclose(out);
        remove(tmp);

        rt_close(r); plat_close(p); headless_free(h);
        world_free(w); intern_free(syms); free(src);
        printf("  a live cart's proposals are the log a save is made of\n");
    }

    printf("test_platform: all passed\n");
    return 0;
}
