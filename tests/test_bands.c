/* Golden test for named priority bands (issue #3; DESIGN.md §6.2).
 *
 * Bands are pure compile-time sugar over pairwise `>`: a ladder compiles to
 * superiority edges between the banded judgment rules that actually conflict.
 * This pins that meaning behaviourally — the classic Tweety/penguin triangle,
 * resolved by a ladder instead of a hand-written `>` (cf. test_dl's superiority
 * case) — plus the two decisions that give bands their safety: comparability
 * only within one ladder (§6.2), and no silent default band. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* A ladder makes the more-specific rule beat the general one on the conflict,
 * with zero hand-written superiority edges: opus (bird + penguin) does not fly,
 * tweety (bird only) does. */
static int test_ladder_resolves(void)
{
    const char *src =
        "sort actor\n"
        "entity ( tweety, opus : actor )\n"
        "state ( bird(actor) penguin(actor) )\n"
        "bands taxonomy: general < specific\n"
        "init ( bird(tweety) bird(opus) penguin(opus) )\n"
        "rule r_flies(X: actor):    bird(X)    => flies(X)   @general\n"
        "rule r_grounded(X: actor): penguin(X) => ~flies(X)  @specific\n";

    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    /* the head atom is the ground term "flies(<ent>)" (build_term form) */
    char buf[64];
    snprintf(buf, sizeof buf, "flies(%s)", intern_name(sy, intern_id(sy, "tweety")));
    uint32_t flies_tweety = intern_id(sy, buf);
    snprintf(buf, sizeof buf, "flies(%s)", intern_name(sy, intern_id(sy, "opus")));
    uint32_t flies_opus = intern_id(sy, buf);

    /* tweety is a bird, not a penguin: only the @general rule applies, unopposed */
    CHECK(world_query(w, dl_pos(flies_tweety)) == DL_PROVED);
    /* opus is both: the ladder makes @specific (~flies) beat @general (flies).
     * Ambiguity-blocking rejects the loser outright, so +flies is REFUTED — but
     * that alone can't tell "resolved" from "contested" (an unresolved conflict
     * also REFUTES both sides). The discriminator is the WINNER: ~flies is
     * defeasibly PROVED here, which happens only because the band edge fired. */
    CHECK(world_query(w, dl_pos(flies_opus)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(flies_opus)) == DL_PROVED);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* Comparability only within one ladder: the SAME two rules, but with their
 * bands split across two ladders, are incomparable — so the conflict stays
 * unresolved (neither side wins), exactly as it would be with no bands at all.
 * Bands do not leak comparability across ladders. */
static int test_cross_ladder_incomparable(void)
{
    const char *src =
        "sort actor\n"
        "entity ( opus : actor )\n"
        "state ( bird(actor) penguin(actor) )\n"
        "bands taxo_a: general < mid\n"
        "bands taxo_b: low < specific\n"
        "init ( bird(opus) penguin(opus) )\n"
        "rule r_flies(X: actor):    bird(X)    => flies(X)   @general\n"
        "rule r_grounded(X: actor): penguin(X) => ~flies(X)  @specific\n";

    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    char buf[64];
    snprintf(buf, sizeof buf, "flies(%s)", intern_name(sy, intern_id(sy, "opus")));
    uint32_t flies_opus = intern_id(sy, buf);

    /* no edge across ladders: neither side wins, so BOTH are rejected. Contrast
     * the single-ladder case above, where ~flies is PROVED. */
    CHECK(world_query(w, dl_pos(flies_opus)) == DL_REFUTED);
    CHECK(world_query(w, dl_neg(flies_opus)) == DL_REFUTED);

    world_free(w);
    intern_free(sy);
    return 0;
}

static int expect_error_msg(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int ok = (w == NULL) && d.nerrors >= 1 &&
             d.items[0].sev == STORY_ERROR && d.items[0].line >= 1 &&
             (needle == NULL || strstr(d.items[0].msg, needle) != NULL);
    if (!ok)
        fprintf(stderr, "FAIL expected error (needle=%s) for <<%s>>: got %s\n",
                needle ? needle : "(any)", src,
                d.nerrors ? d.items[0].msg : "(no error)");
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_errors(void)
{
    /* @band naming no declared band — no silent default (§6.2) */
    if (expect_error_msg(
            "state a\nrule r: a => b @nope\n", "unknown band"))          return 1;
    /* a band may belong to only one ladder (comparability unambiguous) */
    if (expect_error_msg(
            "bands l1: a < b\nbands l2: b < c\n", "both ladders"))       return 1;
    /* a band listed twice in one ladder */
    if (expect_error_msg(
            "bands l: a < b < a\n", "twice"))                            return 1;
    /* missing band name after '@' */
    if (expect_error_msg(
            "state a\nrule r: a => b @\n", "band name after"))           return 1;
    /* a non-identifier band in a ladder */
    if (expect_error_msg(
            "bands l: base < 3\n", "band name"))                         return 1;
    return 0;
}

int main(void)
{
    if (test_ladder_resolves())           return 1;
    if (test_cross_ladder_incomparable()) return 1;
    if (test_errors())                    return 1;
    printf("test_bands: all passed\n");
    return 0;
}
