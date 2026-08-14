/* Every `.story` in examples/ must compile with zero error-severity diagnostics.
 *
 * The examples double as the surface language's documentation, so a grammar or
 * grounder change that breaks one is a real regression — but most of them were
 * referenced by no test at all, which meant rot in an example was invisible and
 * looked exactly like a file that was never meant to compile.
 *
 * The list below is explicit rather than a directory scan: a new example has to
 * be added here, which is the point. A file that cannot compile yet does not
 * silently sit outside the net — either it is listed and must compile, or its
 * absence is a deliberate, reviewable omission.
 *
 * Files whose SEMANTICS are pinned (not just their compilation) live in their
 * own tests — test_prov, test_spatial, test_reaction, test_binder, test_taxprobe
 * and test_probe5e. This is the floor under all of them. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

static const char *EXAMPLES[] = {
    "cellar.story",
    "cellar_ground.story",
    "cellar_play.story",
    "cellar_pure.story",
    "cellar_prop.story",
    "combat5e.story",
    "combat_srd.story",
    "cues.story",
    "duel_pure.story",
    "encounter5e.story",
    "fireball5e.story",
    "mv_door.story",
    "numeric_combat.story",
    "numeric_hp.story",
    "patrol.story",
    "ramif_cellar.story",
    "reaction5e.story",
    "spellbook5e.story",
    "srd_probe.story",
    "srd_probe2.story",
    "tactics.story",
    "taxonomy5e.story",
};
enum { NEXAMPLES = (int)(sizeof EXAMPLES / sizeof EXAMPLES[0]) };

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    size_t rd = fread(s, 1, (size_t)n, f);
    s[rd] = 0;
    fclose(f);
    return s;
}

int main(void)
{
    int bad = 0, warned = 0;
    for (int i = 0; i < NEXAMPLES; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", STORY_DIR, EXAMPLES[i]);
        char *src = slurp(path);
        if (!src) {
            fprintf(stderr, "FAIL %s: cannot open %s\n", EXAMPLES[i], path);
            bad++;
            continue;
        }
        intern *sy = intern_new();
        story_diag di[64];
        story_diags dg = { di, 64, 0, 0 };
        world *w = story_compile(src, EXAMPLES[i], sy, &dg);
        if (!w || dg.nerrors) {
            fprintf(stderr, "FAIL %s: %d error(s), first: %s\n", EXAMPLES[i],
                    dg.nerrors, dg.count ? di[0].msg : "(none reported)");
            bad++;
        } else if (dg.count) {
            /* warnings do not fail a compile (orphan/typo detection), but an
             * example carrying one is worth seeing in the log */
            fprintf(stderr, "note %s: %d warning(s), first: %s\n", EXAMPLES[i],
                    dg.count, di[0].msg);
            warned++;
        }
        if (w) world_free(w);
        intern_free(sy);
        free(src);
    }
    if (bad) {
        fprintf(stderr, "test_examples: %d/%d failed\n", bad, NEXAMPLES);
        return 1;
    }
    printf("test_examples: all passed (%d examples compile, %d with warnings)\n",
           NEXAMPLES, warned);
    return 0;
}
