/* Cardinality guard (§5.2 item 5 / §5.6, EPIC #20 / #19). Pins the anchor-aware
 * large-cross-product warning: a *safe* rule (every var bound) whose variables
 * split into two or more groups that no positive body atom joins ranges over
 * their cross product. When that product is large it is a compile-time WARNING
 * with the estimated count — never a silent nᵏ blow-up. A rule joined by a sparse
 * anchor (a provider or a binary fluent) is one group and never warns; a small
 * product never warns.
 *
 * This is the honest successor to the old size-only warning, which claimed "no
 * sparse anchor" without checking. It sits beside check_safety (which flags an
 * *unbound* var); here the vars are bound but un-anchored. The warning lives in
 * the semantic pass, so it is grounding-path independent — this test compiles via
 * the join matcher (no init facts => it grounds nothing) to keep it fast while
 * still exercising the check.
 */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "e0, e1, …, e{n-1}" — enough entities to push a 2-var rule past the 100k
 * cross-product threshold (320^2 = 102400). */
static void gen_entities(char *buf, size_t cap, int n)
{
    size_t off = 0;
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(buf + off, cap - off, "%se%d", i ? ", " : "", i);
}

/* Compile `src` via the matcher; assert success, exactly `nwarn` diagnostics (all
 * warnings, no errors), and — when `needle` is non-NULL — that some diag has it. */
static int expect(const char *src, int nwarn, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile_matched(src, "card.story", sy, &d);
    int rc = 0;
    if (!w) { fprintf(stderr, "compile FAILED: %s\n", d.count ? di[0].msg : "?"); rc = 1; }
    else if (d.nerrors != 0) { fprintf(stderr, "unexpected error: %s\n", di[0].msg); rc = 1; }
    else if (d.count != nwarn) {
        fprintf(stderr, "want %d diags, got %d:\n", nwarn, d.count);
        for (int i = 0; i < d.count; i++) fprintf(stderr, "  [%d] %s\n", i, di[i].msg);
        rc = 1;
    } else if (needle) {
        int found = 0;
        for (int i = 0; i < d.count; i++) if (strstr(di[i].msg, needle)) found = 1;
        if (!found) {
            fprintf(stderr, "no diag contains '%s':\n", needle);
            for (int i = 0; i < d.count; i++) fprintf(stderr, "  [%d] %s\n", i, di[i].msg);
            rc = 1;
        }
    }
    if (w) world_free(w);
    intern_free(sy);
    return rc;
}

#define NBIG 320                       /* 320^2 = 102400 > CARD_WARN (100000) */
static char ENTS[8192];

int main(void)
{
    gen_entities(ENTS, sizeof ENTS, NBIG);
    char src[16384];

    /* --- warns: a large, un-anchored cross product (X and Y never co-occur) --- */
    snprintf(src, sizeof src,
        "sort actor\n"
        "state ( awake(actor) )\n"
        "entity ( %s : actor )\n"
        "rule pair(X: actor, Y: actor): awake(X) & awake(Y) => paired(X, Y)\n",
        ENTS);
    if (expect(src, 1, "un-anchored cross product")) return 1;

    /* --- no warn: same size, but a binary fluent joins X and Y (one group) --- */
    snprintf(src, sizeof src,
        "sort actor\n"
        "state ( enemy(actor) knows(actor, actor) )\n"
        "entity ( %s : actor )\n"
        "rule alert(X: actor, Y: actor): enemy(X) & knows(X, Y) => alerted(Y)\n",
        ENTS);
    if (expect(src, 0, NULL)) return 1;

    /* --- no warn: un-anchored but the product is small (below threshold) --- */
    if (expect(
        "sort actor\n"
        "state ( awake(actor) )\n"
        "entity ( a0, a1, a2 : actor )\n"
        "rule pair(X: actor, Y: actor): awake(X) & awake(Y) => paired(X, Y)\n",
        0, NULL)) return 1;

    /* --- no warn: a single variable is a linear scan, not a cross product --- */
    snprintf(src, sizeof src,
        "sort actor\n"
        "state ( awake(actor) )\n"
        "entity ( %s : actor )\n"
        "rule flag(X: actor): awake(X) => flagged(X)\n",
        ENTS);
    if (expect(src, 0, NULL)) return 1;

    printf("test_cardinality: all passed\n");
    return 0;
}
