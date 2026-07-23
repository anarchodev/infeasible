/* Golden test for the flat `scene NAME` header (issue #2; DESIGN.md §4.1/§6.4).
 *
 * Pins the meaning of the first slice of nested scopes: a single top-level
 * `scene NAME` is accepted and is semantically INVISIBLE — the compiled world
 * is identical to the same file without the header (a flat scene is one
 * partition, §4.1, so no atom is qualified). The nested (`scene S in M`) and
 * module-extension (`module`/`extend`) forms are M4 (§5.5/§6.4) and must fail
 * LOUDLY with a located "not yet", never be silently swallowed. Placement is a
 * header: first declaration, at most once. */

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

/* Compile `src`; expect success with zero diagnostics; return the world (or
 * NULL on failure, having reported it). Caller owns the world and intern. */
static world *compile_clean(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w)
        fprintf(stderr, "  compile failed: %s\n",
                d.count ? d.items[0].msg : "(no message)");
    else if (d.count != 0)
        fprintf(stderr, "  unexpected diagnostic: %s\n", d.items[0].msg);
    return (w && d.count == 0) ? w : NULL;
}

/* A flat `scene NAME` header parses, and the resulting world is byte-for-byte
 * behaviourally identical to the header-less file: same query verdict. */
static int test_flat_scene_invisible(void)
{
    /* `weak` is a derived judgment head, NOT a base fluent — a closed-world
     * fluent would assert ~weak and defeat the rule (cf. cellar_prop). */
    const char *with =
        "scene skirmish\n"
        "state ( strong )\n"
        "init strong\n"
        "rule r: strong => weak\n";
    const char *without =
        "state ( strong )\n"
        "init strong\n"
        "rule r: strong => weak\n";

    intern *s1 = intern_new(), *s2 = intern_new();
    world *a = compile_clean(with, s1);
    world *b = compile_clean(without, s2);
    CHECK(a != NULL);
    CHECK(b != NULL);

    /* invisibility: identical atom vocabulary — the header interns nothing and
     * qualifies no atom with a `skirmish:` prefix (§4.1, single partition).
     * Compare counts BEFORE the queries below add any new names. */
    CHECK(intern_count(s1) == intern_count(s2));

    /* the header changes nothing: `weak` is defeasibly proved in both */
    CHECK(world_query(a, dl_pos(intern_id(s1, "weak"))) == DL_PROVED);
    CHECK(world_query(b, dl_pos(intern_id(s2, "weak"))) == DL_PROVED);

    world_free(a); world_free(b);
    intern_free(s1); intern_free(s2);
    return 0;
}

/* Compile `src`, expect ERROR-severity failure, and require `needle` in the
 * first message so the located "not yet" / placement diagnostics stay honest. */
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

/* The M4 forms fail loudly and located; placement is enforced. */
static int test_rejected_forms(void)
{
    /* nested scope — the M4 half of §5.5 */
    if (expect_error_msg("scene fight in world\nstate x\n", "nested")) return 1;
    /* module extension verbs — M4 half of §6.4 */
    if (expect_error_msg("module world\nstate x\n", "not implemented")) return 1;
    if (expect_error_msg("extend world\nstate x\n", "not implemented")) return 1;
    /* header must be first */
    if (expect_error_msg("state x\nscene late\n", "first declaration")) return 1;
    /* at most once */
    if (expect_error_msg("scene a\nscene b\n", "duplicate"))            return 1;
    /* a name is required */
    if (expect_error_msg("scene\nstate x\n", "scene name"))            return 1;
    return 0;
}

int main(void)
{
    if (test_flat_scene_invisible()) return 1;
    if (test_rejected_forms())       return 1;
    printf("test_scene: all passed\n");
    return 0;
}
