#ifndef INF_PLATFORM_SPEC_H
#define INF_PLATFORM_SPEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The presentation interface, as data (DESIGN.md §12) — the native half.
 *
 * §12 fixes the SIZE of the presentation swap surface: a PICO-8-sized frozen
 * API, so porting the client is a weekend and not a rewrite. This file is that
 * freeze written down for the runtime the same way `web/platform/spec.mjs`
 * writes it down for the browser backend:
 *
 *   - the surfaces a cart may call (draw, input, focus, audio, persistence)
 *     and their exact op names;
 *   - the constants those ops are defined against — the blessed resolution
 *     set, the 16-entry palette, the two text cells, the key set;
 *   - the letterbox arithmetic and ITS INVERSE, which live below the line
 *     because every cart computing them independently is every cart getting
 *     the edges wrong.
 *
 * TWO SPELLINGS OF ONE FREEZE. The op lists below are the same lists
 * `spec.mjs` carries, and a freeze copied into two files is a freeze that
 * drifts — so `test_platform` reads that file and compares, name by name.
 * Neither side is generated from the other (they are different languages in
 * different builds); what keeps them honest is that disagreeing fails a test. */

/* Internal resolutions: the 1080-divisor set, so every choice integer-scales
 * cleanly to 1080p and 4K forever. A game picks one; the choice is *within*
 * the frozen contract, never a widening of it. */
typedef struct { int w, h; } spec_res;
extern const spec_res SPEC_RESOLUTIONS[];
enum { SPEC_NRESOLUTIONS = 4 };
#define SPEC_DEFAULT_W 640
#define SPEC_DEFAULT_H 360

/* The 16-entry palette. Every colour argument in the draw ops is an index into
 * this, never a colour value — a fixed palette is what makes pre-baked
 * recolored atlas variants an asset-pipeline product instead of a renderer op,
 * and what lets two backends match each other exactly. Stored as 0xRRGGBB. */
enum { SPEC_NCOLORS = 16 };
extern const uint32_t SPEC_PALETTE[SPEC_NCOLORS];

/* The op lists. Each name here is a function pointer on `plat_backend`, and
 * `plat_check_backend` walks these tables rather than a hand-written sequence
 * of NULL tests — so a backend cannot quietly grow a thirteenth op that a cart
 * then depends on, and cannot quietly lack one either. */
typedef struct { const char *name; size_t off; } spec_op;
extern const spec_op SPEC_DRAW_OPS[];
enum { SPEC_NDRAW_OPS = 12 };
extern const spec_op SPEC_AUDIO_OPS[];
enum { SPEC_NAUDIO_OPS = 4 };

/* The surfaces that are not backend ops: `input` and `focus` are implemented
 * by the platform over whatever raw state a backend can supply, and `data` is
 * one fixed blob. Named here anyway, because the freeze is the whole cart-
 * facing vocabulary and not only the part that reaches a renderer. */
extern const char *const SPEC_INPUT_OPS[];
enum { SPEC_NINPUT_OPS = 5 };            /* pointer button pressed key keyp */
extern const char *const SPEC_FOCUS_OPS[];
enum { SPEC_NFOCUS_OPS = 4 };            /* targets current confirmed cancelled */
extern const char *const SPEC_DATA_OPS[];
enum { SPEC_NDATA_OPS = 2 };             /* get set */

/* Directional navigation intent, edge-triggered like `keyp`. A backend maps
 * its own d-pad, stick or arrow keys onto these; never raw axes, for the same
 * reason the key set is never raw scancodes. */
typedef enum { SPEC_NAV_LEFT, SPEC_NAV_RIGHT, SPEC_NAV_UP, SPEC_NAV_DOWN } spec_nav;
enum { SPEC_NNAV = 4 };
extern const char *const SPEC_NAV_DIRS[SPEC_NNAV];

/* Persistence is not storage: one small fixed-size numeric blob for cross-run
 * NON-game state (settings, cosmetic unlocks), explicitly outside the save and
 * explicitly not readable by rules. The save itself is
 * (engine-hash, game-hash, action-log) and the runtime owns it. */
enum { SPEC_CARTDATA_CELLS = 64 };

/* Three pointer buttons — left, middle, right. */
enum { SPEC_BUTTONS = 3 };

/* The frozen named key set, as an index space: a key is a bit, so a tick's
 * keyboard is one uint64 and edge detection is an AND-NOT. Never raw codes —
 * scancodes disagree across platforms, and a raw code is the platform leaking
 * into the cart. A backend maps its own codes onto these names. */
enum { SPEC_NKEYS = 47 };
extern const char *const SPEC_KEYS[SPEC_NKEYS];
/* The key's index, or -1 if it is not a frozen name. */
int spec_key_id(const char *name);

/* The text cells. Frozen as METRICS, not as glyphs: a backend may draw its
 * letters however it likes, but `print` advances the cell width per character
 * and a line is the cell height everywhere. Freezing layout and not shapes is
 * what lets a backend substitute its own bitmap font without every cart's UI
 * shifting by a pixel.
 *
 * There are TWO cells because one density cannot serve both jobs: at 640×360
 * the small cell gives 160 columns (a proof trace reads unwrapped) but a
 * capital is 1.4% of screen height, which is a terminal, not a game UI. So
 * `print` takes a `big` flag the way `spr` takes flip and alpha: a second SIZE
 * on an existing op, never a thirteenth op. */
enum { SPEC_GLYPH_W = 4, SPEC_GLYPH_H = 6, SPEC_BIG_GLYPH_W = 6, SPEC_BIG_GLYPH_H = 8 };

/* Width of a string in internal pixels — derived, so it is not an op. */
int spec_text_width(const char *s, bool big);

/* The upscale: nearest-neighbour INTEGER scale with letterboxing. A wider
 * window must never reveal more map — under lockstep that is a fairness rule,
 * not taste — so the internal resolution is fixed and the surplus becomes
 * bars. */
typedef struct { int scale, x, y, w, h; } spec_box;
spec_box spec_letterbox(int display_w, int display_h, int internal_w, int internal_h);

/* The inverse: a display-space point back to internal coordinates, clamped to
 * the surface. Below the line for the same reason the letterbox is. */
void spec_to_internal(int px, int py, spec_box box, int internal_w, int internal_h,
                      int *out_x, int *out_y);

#endif
