/* Golden test for sort union — a declared COVER (#231, §12 friction 3).
 *
 * A predicate is monomorphic in its argument sorts, so "everything placed on
 * the map" needed one predicate per sort and a renderer that took the union at
 * read time in JS. `sort thing union actor, item` makes the cover a declared
 * thing the compiler knows, and the tests below are about the two ways that
 * could go quietly wrong.
 *
 * A cover that admits too LITTLE is loud: the compile fails. A cover that
 * enumerates too little is SILENT — a rule over `thing` that grounds only the
 * actors concludes nothing about items, the rule looks live, and no diagnostic
 * fires. That is the always-false failure this codebase keeps finding, so most
 * of what follows checks the ground set rather than the compile.
 *
 * A cover is not inheritance. It admits its members' entities and adds none of
 * its own, which is why an entity declared OF a cover is an error: it would
 * have no member sort, so no position and no home. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static intern *SY;

static world *compile_ok(const char *src, const char *what)
{
    story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "u.story", SY, &dg);
    if (!w || dg.nerrors) {
        fprintf(stderr, "FAIL %s: %s\n", what, dg.count ? di[0].msg : "(no message)");
        return NULL;
    }
    return w;
}

/* A story must FAIL, and say why in words containing `needle`. */
static int expect_error(const char *src, const char *needle, const char *what)
{
    intern *sy = intern_new();
    story_diag di[16]; story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "u.story", sy, &dg);
    int bad = 0;
    if (w && !dg.nerrors) {
        fprintf(stderr, "FAIL %s: compiled, should not have\n", what);
        bad = 1;
    } else {
        bool found = false;
        for (int i = 0; i < dg.count; i++)
            if (strstr(di[i].msg, needle)) found = true;
        if (!found) {
            fprintf(stderr, "FAIL %s: no diagnostic mentioning '%s' (first: %s)\n",
                    what, needle, dg.count ? di[0].msg : "none");
            bad = 1;
        }
    }
    if (w) world_free(w);
    intern_free(sy);
    return bad;
}

static bool proved(world *w, const char *atom)
{
    return world_query(w, dl_pos(intern_id(SY, atom))) == DL_PROVED;
}

int main(void)
{
    SY = intern_new();

    /* ---- the cover admits and ENUMERATES both members --------------------- */
    static const char SRC[] =
        "sort actor\n"
        "sort item\n"
        "sort thing union actor, item\n"
        "entity ( hero, guard : actor )\n"
        "entity ( key, torch : item )\n"
        "state (\n"
        "  shown(thing)\n"                    /* one predicate over the cover */
        "  carried(item)\n"
        "  awake(actor)\n"
        ")\n"
        "init ( shown(hero) shown(key) shown(torch) awake(hero) carried(key) )\n"
        /* a variable typed by the cover must range over EVERY member */
        "rule vis(T: thing): shown(T) => visible(T)\n"
        /* a member-typed variable read at a cover position: subsumption */
        "rule lit(I: item): carried(I) & shown(I) => held_up(I)\n"
        "rule up(A: actor): awake(A) & shown(A) => alert(A)\n";

    world *w = compile_ok(SRC, "the cover story");
    CHECK(w != NULL);

    /* THE test: a rule over the cover concluded about members of BOTH sorts.
     * Grounding only the actors would leave `visible(key)` unconcluded and
     * nothing would say so. */
    CHECK(proved(w, "visible(hero)"));
    CHECK(proved(w, "visible(key)"));
    CHECK(proved(w, "visible(torch)"));
    CHECK(!proved(w, "visible(guard)"));        /* not shown: correctly absent */
    printf("  a rule over the cover grounds over every member sort\n");

    /* the cover position accepts a member-typed variable in both directions */
    CHECK(proved(w, "held_up(key)"));
    CHECK(proved(w, "alert(hero)"));
    printf("  a member-sorted variable is admitted at a cover position\n");

    /* and a member-typed predicate still refuses the other member */
    CHECK(!proved(w, "held_up(hero)"));
    printf("  ...while a member-typed predicate stays monomorphic\n");
    world_free(w);

    /* ---- entity order across the cover is member-declaration order -------- */
    {
        static const char S2[] =
            "sort a\nsort b\nsort both union a, b\n"
            "entity ( a1, a2 : a )\nentity ( b1 : b )\n"
            "state ( m(both) )\ninit ( m(a1) m(b1) )\n"
            "rule r(X: both): m(X) => seen(X)\n";
        world *w2 = compile_ok(S2, "two-member cover");
        CHECK(w2 != NULL);
        CHECK(proved(w2, "seen(a1)") && proved(w2, "seen(b1)") && !proved(w2, "seen(a2)"));
        printf("  a cover with uneven members enumerates all of them\n");
        world_free(w2);
    }

    /* ---- what a cover refuses, and says so -------------------------------- */
    int bad = 0;
    bad |= expect_error("sort actor\nsort thing union nosuch\n",
                        "undeclared sort", "unknown member");
    bad |= expect_error("sort thing union thing\n",
                        "cannot union itself", "self-union");
    bad |= expect_error("sort a\nsort b\nsort m1 union a\nsort m2 union m1, b\n",
                        "itself a union", "cover of a cover");
    bad |= expect_error("sort actor\ndomain cell\nsort thing union actor, cell\n",
                        "opaque domain", "cover over a domain");
    bad |= expect_error("enum school { fire, ice }\nsort actor\n"
                        "sort thing union actor, school\n",
                        "an enum", "cover over an enum");
    bad |= expect_error("sort a\nsort b\nsort t union a, a\n",
                        "twice", "duplicate member");
    /* an entity OF a cover has no member sort, so no position and no home */
    bad |= expect_error("sort actor\nsort item\nsort thing union actor, item\n"
                        "entity ( x : thing )\n",
                        "declared of the union", "entity of a cover");
    /* the sort check still bites where no cover relates the two */
    bad |= expect_error("sort actor\nsort item\nentity ( hero : actor )\n"
                        "state ( carried(item) )\ninit ( carried(hero) )\n",
                        "expects sort", "unrelated sorts still refused");
    CHECK(bad == 0);
    printf("  six malformed covers and an unrelated sort are refused, each by name\n");

    intern_free(SY);
    printf("test_sortunion: all passed\n");
    return 0;
}
