/* Golden test for the EPIC #123 probe (#127): examples/taxonomy5e.story —
 * the SRD d20 space authored end-to-end with kinds-as-rules, compiled and
 * driven for real. The header of the .story file carries the write-up (which
 * PHB sentences it states, and the filed gaps #143/#144/#145); this test
 * pins that the claims hold:
 *
 *  - two levels of selection: Bless (saves ∪ attacks, a derived kind) hits
 *    saves and every attack but NOT checks; Halfling Luck (all of d20)
 *    floors the check-only initiative that Bless ignores;
 *  - product cross-cuts: Rage takes the sword but not the thrown dagger;
 *    Bracers take the bow AND the dagger (ranged weapon, by defeat);
 *    the wand takes only the firebolt;
 *  - negation: the ward caps nonmagical slashing damage, leaves magical
 *    fire alone;
 *  - the build-time why: `d20(initiative)` traces through ONE membership
 *    fact added at the bottom of the file (the program-union claim), and
 *    the thrown dagger's melee membership shows the defeat;
 *  - the runtime why: an expanded modifier's dotted label
 *    (`bless.spell_save`) renders with its provenance span. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = '\0';
    fclose(f);
    return buf;
}

static int step1(world *w, intern *sy, const char *action)
{
    uint32_t a = intern_id(sy, action);
    char err[128];
    int r = world_step(w, &a, 1, err, sizeof err);
    if (r) fprintf(stderr, "  step %s: %s\n", action, err);
    return r;
}

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

static char *why_str(world *w, intern *sy, const char *atom)
{
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(w, dl_pos(intern_id(sy, atom)), m);
    fclose(m);
    return buf;
}

int main(void)
{
    char *src = read_file(STORY_DIR "/taxonomy5e.story");
    CHECK(src != NULL);

    /* ---- compile: clean, not even a warning (the taxonomy is total) ---- */
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "taxonomy5e.story", sy, &d);
    if (!w || d.count)
        fprintf(stderr, "  compile: %s\n", d.count ? di[0].msg : "?");
    CHECK(w != NULL && d.count == 0);
    world_set_seed(w, 5);

    /* ---- unbuffed: bases everywhere, initiative at its base 1 ---- */
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(step1(w, sy, "snap_ini(bran)") == 0);
    CHECK(num(w, sy, "ss(bran)") == 10);
    CHECK(num(w, sy, "ath(bran)") == 10);
    CHECK(num(w, sy, "ini(bran)") == 1);

    /* ---- everything on: the whole taxonomy exercised at once ---- */
    static const char *BUFFS[] = { "b(bran)", "l(bran)", "r(bran)",
                                   "a(bran)", "wc(bran)", "w(bran)" };
    for (size_t i = 0; i < sizeof BUFFS / sizeof *BUFFS; i++)
        CHECK(step1(w, sy, BUFFS[i]) == 0);
    CHECK(step1(w, sy, "snap(bran)") == 0);
    CHECK(step1(w, sy, "snap_ini(bran)") == 0);

    /* exact under seed 5 — each blessed value rolls its own d4 (#82 clones) */
    CHECK(num(w, sy, "ss(bran)") == 12);   /* save: 10 + d4(2); Luck no-op    */
    CHECK(num(w, sy, "cs(bran)") == 12);   /* save: its own die               */
    CHECK(num(w, sy, "ath(bran)") == 10);  /* check: Bless does NOT cover it  */
    CHECK(num(w, sy, "swa(bran)") == 12);  /* attack: d4 only — Rage moved to
                                            * the DAMAGE roll (#143)          */
    CHECK(num(w, sy, "bwa(bran)") == 13);  /* attack: d4 + Bracers (rgd wpn)  */
    CHECK(num(w, sy, "fba(bran)") == 14);  /* attack: d4 + wand (`_`, spell)  */
    /* the thrown dagger: ranged-weapon BY DEFEAT — Bracers yes, Rage NO */
    CHECK(num(w, sy, "dta(bran)") == 14);  /* 10 + d4(2) + 2, and not +2 more */
    /* damage: Rage's +2 lands on the LINKED sword_dmg (#143), and the
     * brutal ward's cap applies above it (blanket-ordered): min(10+2, 8) */
    CHECK(num(w, sy, "swd(bran)") == 8);
    CHECK(num(w, sy, "fbd(bran)") == 10);
    /* initiative: a check — Bless ignores it, Luck floors it (1 -> 2) */
    CHECK(num(w, sy, "ini(bran)") == 2);

    /* ---- runtime why: the expanded dotted label, with provenance ---- */
    char *t = why_str(w, sy, "bless.spell_save(bran)");
    CHECK(t != NULL);
    CHECK(strstr(t, "bless.spell_save(bran)") != NULL);
    CHECK(strstr(t, "blessed(bran)") != NULL);
    CHECK(strstr(t, "taxonomy5e.story:") != NULL);
    CHECK(strstr(t, "applicable") != NULL);
    free(t);

    world_free(w);
    intern_free(sy);

    /* ---- build-time why #1: the program-union claim — one fact at the
     * bottom of the file makes initiative a d20 via the check route ---- */
    sy = intern_new();
    story_diags d2 = { di, 16, 0, 0 };
    char *buf = NULL;
    size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    w = story_compile_kinds_why(src, "taxonomy5e.story", sy, &d2,
                                "d20(initiative)", m);
    fclose(m);
    CHECK(w != NULL && d2.nerrors == 0 && buf != NULL);
    CHECK(strstr(buf, "d20(initiative)") != NULL);
    CHECK(strstr(buf, "PROVED") != NULL);
    CHECK(strstr(buf, "checks_roll_d20[V=initiative") != NULL);
    CHECK(strstr(buf, "check(initiative,dex)") != NULL);
    free(buf);
    world_free(w);
    intern_free(sy);

    /* ---- build-time why #2: the thrown dagger's defeat ---- */
    sy = intern_new();
    story_diags d3 = { di, 16, 0, 0 };
    buf = NULL; n = 0;
    m = open_memstream(&buf, &n);
    w = story_compile_kinds_why(src, "taxonomy5e.story", sy, &d3,
                                "attack(dagger_throw,melee,weapon)", m);
    fclose(m);
    CHECK(w != NULL && d3.nerrors == 0 && buf != NULL);
    CHECK(strstr(buf, "REFUTED") != NULL);
    CHECK(strstr(buf, "melee_by_weapon[V=dagger_throw]") != NULL);
    CHECK(strstr(buf, "thrown_not_melee[V=dagger_throw]") != NULL);
    free(buf);
    world_free(w);
    intern_free(sy);

    free(src);
    printf("test_taxprobe: all passed\n");
    return 0;
}
