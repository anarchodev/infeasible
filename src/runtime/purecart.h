#ifndef INF_RUNTIME_PURECART_H
#define INF_RUNTIME_PURECART_H

#include "runtime/runtime.h"
#include "runtime/scene.h"

/* The cart that isn't one (DESIGN.md §12, the infeasible cart).
 *
 * `rt_open` wants a cart: init/tick/draw. This supplies one that contains no
 * game. It declares what is focusable, submits the command the world offered,
 * and draws what the world concluded. Point it at a different `.story` and it
 * is a different game with no edit here — which is the whole claim, and the
 * reason this file is short.
 *
 * What is left after this is exactly the residue worth naming: the loop, the
 * layout arithmetic inside the scene, and the ASSETS. Assets are not code — a
 * sprite atlas is pixels, and pixels are not expressible as rules for the same
 * reason a `.png` is not. */

typedef struct purecart purecart;

purecart *purecart_new(world *w, intern *syms, const iface *f, plat *p,
                       const char *sheet);
void      purecart_free(purecart *c);

/* The cart vtable to hand `rt_open`. */
rt_cart purecart_cart(purecart *c);

/* The scene it drives, for a driver that wants to inspect the model (a test,
 * or a debug overlay). */
scene *purecart_scene(purecart *c);

/* The trace the last refused command printed, or "" — the world's own argument
 * about its own guard, never a message this file invented. */
const char *purecart_why(const purecart *c);

#endif
