#ifndef INF_PLATFORM_DISPLAY_H
#define INF_PLATFORM_DISPLAY_H

#include "platform/platform.h"

/* A DISPLAY backend: a window, the frozen ops drawn into it, and whatever raw
 * input the machine has (DESIGN.md §12, §13).
 *
 * THE SEAM IS A BUILD-TIME BOUNDARY, NOT A RUNTIME ONE. Exactly one file
 * implementing `display_open` is compiled into a build, chosen by the build
 * system. That is not a style preference: a per-cart export is statically
 * linked with no loader, so a runtime plugin interface would break the very
 * artifact the export model exists to produce — and a console's platform layer
 * is under NDA and cannot appear in a public tree at all, so it has to be a
 * file dropped in beside these rather than a branch inside one.
 *
 * A backend therefore implements this header and nothing else, and the port a
 * new platform needs is one file plus one line of build system.
 *
 * `src/platform/backends/sdl3.c`  desktop: window, input, gamepad, audio
 * `src/platform/backends/none.c`  a build with no display at all — the default,
 *                                 so the whole test suite needs no window
 *
 * The headless backend (`headless_new`, platform.h) is not a display: it
 * implements the same frozen ops with no drawing, which is what makes the
 * op-set testable and is the cheapest possible proof that the freeze holds. */

typedef struct display display;

/* Open a window `scale`× the internal resolution, or NULL with `err` filled —
 * including in a build with no display backend, which is an ordinary answer
 * rather than a link error. */
display *display_open(const char *title, int width, int height, int scale,
                      char *err, size_t errsz);
void     display_close(display *d);

/* The frozen-op implementation to hand `plat_open`. */
plat_backend display_backend(display *d);

/* Pump the platform's own event queue. Called once per tick, at the tick
 * boundary, by the player — never by a cart, which has no idea a queue exists. */
void display_pump(display *d);

/* Has the user asked to close the window? The one thing a display knows that
 * the world does not. */
bool display_quit_requested(const display *d);

/* Is this build's display backend a real one? */
const char *display_name(void);

/* Write the frame just presented to `path` (BMP). The native twin of a
 * screenshot check: a backend that executes every op and paints nothing looks
 * exactly like a working one from the op log, and this is the only question
 * that tells them apart. False when the backend cannot read its own pixels. */
bool display_capture(display *d, const char *path);

#endif
