#include "platform/display.h"

#include <stdio.h>

/* The display backend for a build that has none (DESIGN.md §12).
 *
 * The default build compiles the whole engine, the platform tier and every
 * test without needing a window — so `display_open` has to exist and answer
 * honestly rather than fail to link. A player built this way still plays: it
 * runs on the headless backend, which implements the same frozen ops and draws
 * nothing, and that is exactly how the test suite plays a cart. */

display *display_open(const char *title, int width, int height, int scale,
                      char *err, size_t errsz)
{
    (void)title; (void)width; (void)height; (void)scale;
    if (err)
        snprintf(err, errsz,
                 "this build has no display backend — configure with "
                 "-DINF_SDL=ON, or run headless");
    return NULL;
}

void display_close(display *d) { (void)d; }

plat_backend display_backend(display *d)
{
    (void)d;
    plat_backend be;
    for (size_t i = 0; i < sizeof be; i++) ((char *)&be)[i] = 0;
    return be;
}

void display_pump(display *d) { (void)d; }
bool display_capture(display *d, const char *path)
{
    (void)d; (void)path;
    return false;
}
bool display_quit_requested(const display *d) { (void)d; return true; }
const char *display_name(void) { return "none"; }
