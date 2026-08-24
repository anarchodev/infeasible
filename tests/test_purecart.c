/* THE INFEASIBLE CART, NATIVELY (DESIGN.md §12).
 *
 * A cart written entirely in `.story`, with no host code anywhere, played by
 * the native runtime: `examples/cellar_pure.story` carries its own presentation
 * as ordinary judgments over enum constants, `src/runtime/scene.c` draws
 * whatever a story concluded in that vocabulary, and `src/runtime/purecart.c`
 * is the driver — which contains no game.
 *
 * Two things make this a test rather than a demo.
 *
 * The renderer must never learn a game word. Every assertion below is about
 * what the RULES concluded (a panel, a caption, a menu row, a shaded region),
 * never about a cellar — and the walkthrough at the end solves the game
 * through the same surface a player uses, which is the only way to find out
 * whether a generic renderer is actually generic.
 *
 * And the driver plays it on `plat_focus` alone: no pointer, no key. That is
 * §12's portability claim under test — given a set of focusable regions a
 * d-pad walks them, a tap hit-tests them, and a cart that never reads a
 * pointer runs on a console. */

#include "runtime/purecart.h"
#include "runtime/runtime.h"
#include "runtime/iface.h"
#include "runtime/scene.h"
#include "platform/platform.h"
#include "lang/story.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
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

/* the menu as one string, blocked rows starred — the cheapest way to say what
 * the engine is offering right now */
static void labels(scene *sc, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < scene_menu_count(sc); i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s%s", used ? " | " : "",
                 scene_menu_label(sc, i), scene_menu_ok(sc, i) ? "" : "*");
    }
}

static headless *H;
static plat     *P;
static rt       *R;
static purecart *C;

/* Drive focus onto a target with the D-PAD ALONE, the way a console would:
 * step in the direction of the wanted region until focus lands on it. The
 * platform refuses to wrap at the edges by design, so a walker has to steer
 * rather than cycle — which is also the honest test of geometric navigation. */
static int focus_to(const char *want)
{
    scene *sc = purecart_scene(C);
    plat_target ts[PLAT_MAX_TARGETS];
    int n = scene_targets(sc, ts, PLAT_MAX_TARGETS);
    plat_target goal = { NULL, 0, 0, 0, 0 };
    bool found = false;
    for (int i = 0; i < n; i++)
        if (strcmp(ts[i].id, want) == 0) { goal = ts[i]; goal.id = NULL; found = true; }
    if (!found) return -1;

    for (int guard = 0; guard < 96; guard++) {
        /* COPIED, not aliased: the id points into the platform's own storage
         * and moves when focus does, so a saved pointer compares with itself */
        char cur[160] = "";
        if (plat_focus_current(P)) snprintf(cur, sizeof cur, "%s", plat_focus_current(P));
        if (strcmp(cur, want) == 0) return 0;
        n = scene_targets(sc, ts, PLAT_MAX_TARGETS);
        plat_target at = { NULL, 0, 0, 0, 0 };
        for (int i = 0; i < n; i++)
            if (strcmp(ts[i].id, cur) == 0) at = ts[i];
        spec_nav dir;
        if (goal.y >= at.y + at.h)      dir = SPEC_NAV_DOWN;
        else if (goal.y + goal.h <= at.y) dir = SPEC_NAV_UP;
        else if (goal.x > at.x)         dir = SPEC_NAV_RIGHT;
        else                            dir = SPEC_NAV_LEFT;
        headless_pad(H, &dir, 1, false, false);
        rt_advance(R, 1);
        headless_pad(H, NULL, 0, false, false);
        rt_advance(R, 1);
        if (plat_focus_current(P) && strcmp(plat_focus_current(P), cur) == 0) {
            /* that direction had nothing: try the other axis rather than
             * pressing into a wall forever */
            spec_nav alt = (dir == SPEC_NAV_DOWN || dir == SPEC_NAV_UP)
                               ? SPEC_NAV_RIGHT : SPEC_NAV_DOWN;
            headless_pad(H, &alt, 1, false, false);
            rt_advance(R, 1);
            headless_pad(H, NULL, 0, false, false);
            rt_advance(R, 1);
        }
    }
    return -1;
}

static int confirm_focused(void)
{
    headless_pad(H, NULL, 0, true, false);
    rt_advance(R, 1);
    headless_pad(H, NULL, 0, false, false);
    rt_advance(R, 1);
    return 0;
}

/* Confirm a menu row by its label — focus it with the pad, then press confirm.
 * The pointer is never touched anywhere in this file. */
static int confirm_label(const char *label)
{
    scene *sc = purecart_scene(C);
    int idx = -1;
    for (int i = 0; i < scene_menu_count(sc); i++)
        if (strcmp(scene_menu_label(sc, i), label) == 0) idx = i;
    if (idx < 0) return -1;
    char want[160];
    snprintf(want, sizeof want, "cmd:%s", scene_menu_term(sc, idx));
    if (focus_to(want) != 0) return -1;
    return confirm_focused();
}

static int confirm_entity(const char *entity)
{
    char want[160];
    snprintf(want, sizeof want, "ent:%s", entity);
    if (focus_to(want) != 0) return -1;
    return confirm_focused();
}

int main(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/cellar_pure.story", STORY_DIR);
    char *src = slurp(path);
    CHECK(src != NULL);

    intern *syms = intern_new();
    story_diag di[32];
    story_diags dg = { di, 32, 0, 0 };
    char *artifact = NULL;
    world *w = story_compile_iface(src, "cellar_pure.story", syms, &dg, &artifact);
    CHECK(w != NULL && dg.nerrors == 0);
    CHECK(artifact != NULL);

    char err[192];
    iface *f = iface_parse(artifact, err, sizeof err);
    if (!f) fprintf(stderr, "  iface: %s\n", err);
    CHECK(f != NULL);

    H = headless_new(640, 360);
    plat_backend be = headless_backend(H);
    P = plat_open(&be, err, sizeof err);
    CHECK(P != NULL);
    /* assets are not code: naming an atlas is below the durability line,
     * deciding what one IS belongs to the backend */
    static const char *const SHEETS[] = { "main", "main_fog" };

    C = purecart_new(w, syms, f, P, "main");
    rt_cart cart = purecart_cart(C);
    cart.sheets = SHEETS;
    cart.nsheets = 2;
    R = rt_open(P, w, syms, cart, "examples/cellar_pure.story");
    CHECK(R != NULL);

    rt_advance(R, 1);
    scene *sc = purecart_scene(C);

    /* ---- drawn from conclusions alone ------------------------------------- */
    {
        scene_pair panels[32];
        CHECK(scene_pairs(sc, "panel", panels, 32) == 5);
        const char *text = headless_text(H);
        CHECK(strstr(text, "THE CELLAR") != NULL);
        CHECK(strstr(text, "VAULT") != NULL);
        CHECK(headless_count_of(H, "spr") >= 3);
        CHECK(headless_count_of(H, "shade") > 0);        /* the dark cellar */
        printf("  the frame is panels, captions and sprites the world concluded\n");
    }

    /* THE MENU IS THE ENGINE'S ANSWER — no `offers`/`blocked` judgments, no
     * `cmd` enum: this list is what the world says about its own actions. */
    {
        char m[512];
        labels(sc, m, sizeof m);
        CHECK(strcmp(m, "GO HALL | ENTER VAULT* | TAKE KEY* | UNLOCK* | FORCE DOOR*") == 0);
        /* ...and every greyed row is refused by a JUDGMENT, so `why` has
         * something to say about it */
        for (int i = 0; i < scene_menu_count(sc); i++) {
            if (scene_menu_ok(sc, i)) continue;
            scene_hit hit;
            char id[160];
            snprintf(id, sizeof id, "cmd:%s", scene_menu_term(sc, i));
            CHECK(scene_target(sc, id, &hit) && hit.has_blocker);
        }
        printf("  the menu: %s\n", m);
    }

    /* ---- playing it, on focus alone --------------------------------------- */
    {
        CHECK(confirm_label("TAKE KEY") == 0);           /* refused: it is dark */
        CHECK(strstr(purecart_why(C), "in_dark") != NULL);
        printf("  a refused command printed the guard that refused it\n");

        CHECK(confirm_label("GO HALL") == 0);
        CHECK(world_query(w, dl_pos(intern_id(syms, "at(hero)=hall"))) == DL_PROVED);
        CHECK(headless_audio_count(H) > 0);
        bool stepped = false;
        for (int i = 0; i < headless_audio_count(H); i++)
            if (strstr(headless_audio(H, i), "snd_step")) stepped = true;
        CHECK(stepped);                       /* the cue table the STORY wrote */
        printf("  a confirm moved the hero, and the story's cue played\n");

        char m[512];
        labels(sc, m, sizeof m);
        /* a verb grounds once per binding, but no verb is listed twice */
        for (int i = 0; i < scene_menu_count(sc); i++)
            for (int j = i + 1; j < scene_menu_count(sc); j++)
                CHECK(strcmp(scene_menu_label(sc, i), scene_menu_label(sc, j)) != 0);

        CHECK(confirm_label("TAKE TORCH") == 0);
        /* and the one DROP TORCH kept is the one that applies */
        for (int i = 0; i < scene_menu_count(sc); i++)
            if (strcmp(scene_menu_label(sc, i), "DROP TORCH") == 0)
                CHECK(strcmp(scene_menu_term(sc, i), "drop_torch(hero,hall)") == 0);

        CHECK(confirm_label("GO CELLAR") == 0);
        CHECK(headless_count_of(H, "shade") == 0);       /* the torch lit it */
        printf("  the fetched torch lit the room and the fog is gone\n");

        CHECK(confirm_label("TAKE KEY") == 0);
        CHECK(confirm_label("GO HALL") == 0);
        CHECK(confirm_label("UNLOCK") == 0);
        CHECK(world_query(w, dl_pos(intern_id(syms, "door=jammed"))) == DL_PROVED);

        labels(sc, m, sizeof m);
        CHECK(strstr(m, "FORCE DOOR*") != NULL);   /* refused for the poisoned hero */

        /* clicking an actor selects them — an ACTION, because selection is
         * state and per-viewer state has no home until scopes exist */
        CHECK(confirm_entity("guard") == 0);
        CHECK(world_query(w, dl_pos(intern_id(syms, "selected(guard)"))) == DL_PROVED);
        CHECK(world_query(w, dl_pos(intern_id(syms, "selected(hero)"))) == DL_REFUTED);
        labels(sc, m, sizeof m);
        CHECK(strstr(m, "FORCE DOOR") != NULL && strstr(m, "FORCE DOOR*") == NULL);
        printf("  the menu is now the guard's, and the door is his to force\n");

        CHECK(confirm_label("FORCE DOOR") == 0);
        CHECK(world_query(w, dl_pos(intern_id(syms, "door=open"))) == DL_PROVED);
        CHECK(world_get_num(w, intern_id(syms, "hp(guard)")) == 10);

        CHECK(confirm_entity("hero") == 0);
        CHECK(confirm_label("ENTER VAULT") == 0);
        CHECK(confirm_label("TAKE ANTIDOTE") == 0);
        CHECK(confirm_label("DRINK") == 0);
        CHECK(world_query(w, dl_pos(intern_id(syms, "at(hero)=vault"))) == DL_PROVED);
        CHECK(world_query(w, dl_pos(intern_id(syms, "poisoned(hero)"))) == DL_REFUTED);
        printf("  the cellar is solved with zero game code, on a d-pad\n");
    }

    /* ---- and the playthrough is a save ------------------------------------ */
    {
        char tmp[] = "/tmp/inf_pure_XXXXXX";
        int fd = mkstemp(tmp);
        CHECK(fd >= 0);
        FILE *out = fdopen(fd, "w+");
        int ticks = rt_save(R, out);
        /* Ten commands and TWO SELECTIONS: picking an actor is an action here,
         * because per-viewer state has no home until scopes exist — which §12
         * lists as a friction, and which the log makes visible rather than
         * hiding. Every tick that changed the world is in it, and nothing
         * else: navigating focus wrote nothing. */
        CHECK(ticks == 12);
        fclose(out);
        char *saved = slurp(tmp);
        remove(tmp);
        CHECK(saved != NULL);

        /* replay it into a fresh world with NO cart driving at all: the log is
         * the whole history, and the cart is only how it was produced */
        intern *s2 = intern_new();
        story_diag d2[32];
        story_diags g2 = { d2, 32, 0, 0 };
        world *w2 = story_compile(src, "cellar_pure.story", s2, &g2);
        CHECK(w2 != NULL);
        headless *h2 = headless_new(640, 360);
        plat_backend be2 = headless_backend(h2);
        plat *p2 = plat_open(&be2, err, sizeof err);
        rt_cart silent;
        memset(&silent, 0, sizeof silent);
        rt *r2 = rt_open(p2, w2, s2, silent, "examples/cellar_pure.story");
        CHECK(rt_load(r2, saved, err, sizeof err) == ticks);
        rt_advance(r2, ticks);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "at(hero)=vault"))) == DL_PROVED);
        CHECK(world_query(w2, dl_pos(intern_id(s2, "poisoned(hero)"))) == DL_REFUTED);
        printf("  ...and the playthrough replays from its log alone\n");

        rt_close(r2); plat_close(p2); headless_free(h2);
        world_free(w2); intern_free(s2); free(saved);
    }

    /* ---- THE SAME RENDERER, A DIFFERENT GAME ------------------------------
     *
     * One story is a renderer that fits one game. `examples/duel_pure.story` is
     * structurally distant from the cellar — a card duel, not rooms and items —
     * and it is drawn by the code above with no edit anywhere in it. That is
     * the difference between a primitive and one game's furniture. */
    {
        snprintf(path, sizeof path, "%s/duel_pure.story", STORY_DIR);
        char *dsrc = slurp(path);
        CHECK(dsrc != NULL);
        intern *ds = intern_new();
        story_diag dd[32];
        story_diags dgs = { dd, 32, 0, 0 };
        char *dart = NULL;
        world *dw = story_compile_iface(dsrc, "duel_pure.story", ds, &dgs, &dart);
        CHECK(dw != NULL && dgs.nerrors == 0);
        iface *df = iface_parse(dart, err, sizeof err);
        CHECK(df != NULL);

        headless *dh = headless_new(640, 360);
        plat_backend dbe = headless_backend(dh);
        plat *dp = plat_open(&dbe, err, sizeof err);
        purecart *dc = purecart_new(dw, ds, df, dp, "main");
        rt_cart dcart = purecart_cart(dc);
        dcart.sheets = SHEETS;
        dcart.nsheets = 2;
        rt *dr = rt_open(dp, dw, ds, dcart, "examples/duel_pure.story");
        rt_advance(dr, 1);

        scene *dsc = purecart_scene(dc);
        CHECK(headless_count_of(dh, "spr") > 0);
        CHECK(scene_menu_count(dsc) > 0);
        CHECK(headless_count_of(dh, "print") > 0);
        /* and it is a DIFFERENT game: nothing the cellar offered is here */
        for (int i = 0; i < scene_menu_count(dsc); i++)
            CHECK(strcmp(scene_menu_label(dsc, i), "GO HALL") != 0);
        printf("  a second story draws through the same renderer, unedited: %d rows\n",
               scene_menu_count(dsc));

        rt_close(dr); purecart_free(dc); plat_close(dp); headless_free(dh);
        iface_free(df); world_free(dw); intern_free(ds);
        free(dart); free(dsrc);
    }

    rt_close(R);
    purecart_free(C);
    plat_close(P);
    headless_free(H);
    iface_free(f);
    world_free(w);
    intern_free(syms);
    free(artifact);
    free(src);

    printf("test_purecart: all passed\n");
    return 0;
}
