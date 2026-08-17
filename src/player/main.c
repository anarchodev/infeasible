/* infeasible — the player (DESIGN.md §12).
 *
 *   infeasible <story.story> [--headless N] [--scale N] [--save FILE]
 *              [--load FILE] [--ticks N]
 *
 * The runtime installed once that runs any cart: it compiles the `.story`,
 * reads the §6.3 interface artifact back, opens the display backend this build
 * was configured with, and hands both to the pure cart — which contains no
 * game. Point it at a different story and it is a different game.
 *
 * THE PLAYER OWNS THE CLOCK, and it is the only thing here that does. A TICK is
 * one `world_step` with input sampled once at its boundary; a FRAME is a
 * repaint that can never change a fact; and the runtime itself reads no clock,
 * so the accumulator below is the seat a network tick schedule takes under
 * lockstep with nothing else about a cart changing. */

#include "core/intern.h"
#include "lang/story.h"
#include "platform/display.h"
#include "platform/platform.h"
#include "runtime/iface.h"
#include "runtime/purecart.h"
#include "runtime/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void usage(void)
{
    fprintf(stderr,
        "infeasible — the runtime (DESIGN.md §12)\n\n"
        "  infeasible <story.story> [options]\n\n"
        "  --headless        play with no window (the display this build has: %s)\n"
        "  --ticks N         stop after N ticks\n"
        "  --scale N         window scale over the internal resolution\n"
        "  --load FILE       replay a save (an action log) instead of playing\n"
        "  --save FILE       write the action log on exit\n"
        "  --shot FILE       write the last frame as a BMP (a display build only)\n",
        display_name());
}

int main(int argc, char **argv)
{
    const char *story = NULL, *load_path = NULL, *save_path = NULL, *shot_path = NULL;
    bool headless_only = false;
    int scale = 2, max_ticks = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--headless") == 0) headless_only = true;
        else if (strcmp(a, "--ticks") == 0 && i + 1 < argc) max_ticks = atoi(argv[++i]);
        else if (strcmp(a, "--scale") == 0 && i + 1 < argc) scale = atoi(argv[++i]);
        else if (strcmp(a, "--load") == 0 && i + 1 < argc) load_path = argv[++i];
        else if (strcmp(a, "--save") == 0 && i + 1 < argc) save_path = argv[++i];
        else if (strcmp(a, "--shot") == 0 && i + 1 < argc) shot_path = argv[++i];
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else if (a[0] == '-') { usage(); return 2; }
        else story = a;
    }
    if (!story) { usage(); return 2; }

    char *src = slurp(story);
    if (!src) { fprintf(stderr, "infeasible: cannot read %s\n", story); return 1; }

    /* The compile is the load: source is authoritative and the theory is a
     * cache, so a player opens a `.story` the way an interpreter opens a
     * script — no build step between editing and playing. */
    intern *syms = intern_new();
    story_diag di[64];
    story_diags dg = { di, 64, 0, 0 };
    char *artifact = NULL;
    world *w = story_compile_iface(src, story, syms, &dg, &artifact);
    for (int i = 0; i < dg.count; i++)
        fprintf(stderr, "%s:%d:%d: %s: %s\n", story, di[i].line, di[i].col,
                di[i].sev == STORY_ERROR ? "error" : "warning", di[i].msg);
    if (!w) { fprintf(stderr, "infeasible: %s did not compile\n", story); return 1; }

    char err[256];
    iface *f = iface_parse(artifact, err, sizeof err);
    if (!f) { fprintf(stderr, "infeasible: %s\n", err); return 1; }

    /* the display this build was configured with, or none — a build with no
     * backend still plays, which is how the test suite plays a cart */
    display *disp = NULL;
    plat_backend be;
    headless *hl = NULL;
    if (!headless_only) {
        disp = display_open("infeasible", SPEC_DEFAULT_W, SPEC_DEFAULT_H, scale,
                            err, sizeof err);
        if (!disp) {
            fprintf(stderr, "infeasible: %s\n", err);
            headless_only = true;
        }
    }
    if (disp) be = display_backend(disp);
    else {
        hl = headless_new(SPEC_DEFAULT_W, SPEC_DEFAULT_H);
        be = headless_backend(hl);
    }

    plat *p = plat_open(&be, err, sizeof err);
    if (!p) { fprintf(stderr, "infeasible: %s\n", err); return 1; }

    static const char *const SHEETS[] = { "main", "main_fog" };
    purecart *cart = purecart_new(w, syms, f, p, "main");
    rt_cart vt = purecart_cart(cart);
    vt.sheets = SHEETS;
    vt.nsheets = 2;
    rt *r = rt_open(p, w, syms, vt, story);

    if (load_path) {
        char *log = slurp(load_path);
        if (!log) { fprintf(stderr, "infeasible: cannot read %s\n", load_path); return 1; }
        int n = rt_load(r, log, err, sizeof err);
        if (n < 0) { fprintf(stderr, "infeasible: %s\n", err); return 1; }
        fprintf(stderr, "infeasible: replaying %d ticks from %s\n", n, load_path);
        if (!max_ticks) max_ticks = n;
        free(log);
    }

    /* The two clocks. The wall clock decides HOW MANY ticks elapse and never
     * WHAT a tick contains. */
    const double tps = 20.0;
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    double acc = 0;
    int ticks = 0;

    for (;;) {
        if (max_ticks && ticks >= max_ticks) break;
        if (disp) {
            display_pump(disp);
            if (display_quit_requested(disp)) break;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (double)(now.tv_sec - last.tv_sec) +
                        (double)(now.tv_nsec - last.tv_nsec) / 1e9;
            last = now;
            if (dt > 0.25) dt = 0.25;         /* a long pause is not 200 ticks */
            acc += dt;
            while (acc >= 1.0 / tps) { acc -= 1.0 / tps; rt_step(r); ticks++; }
            rt_render(r);
        } else {
            rt_step(r);
            rt_render(r);
            ticks++;
            if (!max_ticks) break;            /* headless with no budget: one tick */
        }
        const char *complaint = plat_take_error(p);
        if (complaint) fprintf(stderr, "infeasible: %s\n", complaint);
    }

    if (shot_path) {
        if (disp && display_capture(disp, shot_path))
            fprintf(stderr, "infeasible: wrote %s\n", shot_path);
        else
            fprintf(stderr, "infeasible: this build cannot capture a frame\n");
    }

    if (save_path) {
        FILE *out = fopen(save_path, "w");
        if (!out) fprintf(stderr, "infeasible: cannot write %s\n", save_path);
        else {
            int n = rt_save(r, out);
            fclose(out);
            fprintf(stderr, "infeasible: wrote %d ticks to %s\n", n, save_path);
        }
    }

    printf("infeasible: %s played %d ticks on the %s backend\n",
           story, ticks, disp ? display_name() : "headless");

    rt_close(r);
    purecart_free(cart);
    plat_close(p);
    if (disp) display_close(disp);
    if (hl) headless_free(hl);
    iface_free(f);
    world_free(w);
    intern_free(syms);
    free(artifact);
    free(src);
    return 0;
}
