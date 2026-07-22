/* Golden test for the .story front half (M1, first slice): the lexer plus the
 * propositional cellar-subset grammar. Compiles examples/cellar_prop.story and
 * runs the SAME assertions tests/test_world.c's test_cellar() makes against the
 * hand-built world — so this pins that the parser reproduces that world through
 * the public surface. Also checks lexer tokenisation and a few error paths. */

#include "lang/story.h"
#include "lang/lexer.h"
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

/* The lexer emits the right token kinds, disambiguates ~/~> and =/=>, and
 * skips both comment forms. */
static int test_lexer(void)
{
    const char *src =
        "state ( a )  // line comment\n"
        "rule r: a => ~b\n"
        "/* block */ x > y  z ~> w  p -> q";
    tok_kind want[] = {
        TK_STATE, TK_LPAREN, TK_IDENT, TK_RPAREN,
        TK_RULE, TK_IDENT, TK_COLON, TK_IDENT, TK_FATARROW, TK_TILDE, TK_IDENT,
        TK_IDENT, TK_GT, TK_IDENT,
        TK_IDENT, TK_SQARROW, TK_IDENT,
        TK_IDENT, TK_ARROW, TK_IDENT,
        TK_EOF,
    };
    lexer lx;
    lexer_init(&lx, src);
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        token t = lexer_next(&lx);
        if (t.kind != want[i]) {
            fprintf(stderr, "FAIL lexer token %zu: want %s, got %s\n",
                    i, tok_kind_name(want[i]), tok_kind_name(t.kind));
            return 1;
        }
    }
    return 0;
}

/* The main event: compile the propositional cellar and pin its semantics
 * exactly as test_world.c's test_cellar() does against the hand-built world. */
static int test_cellar_from_story(void)
{
    char *src = read_file(STORY_DIR "/cellar_prop.story");
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag ditems[8];
    story_diags diags = { ditems, 8, 0, 0 };
    world *w = story_compile(src, sy, &diags);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n",
                diags.count ? diags.items[0].msg : "(no message)");
        free(src);
        intern_free(sy);
        return 1;
    }
    /* the reference cellar is clean: no diagnostics at all */
    if (diags.count != 0)
        fprintf(stderr, "unexpected diagnostic: %s\n", diags.items[0].msg);
    CHECK(diags.count == 0);

    uint32_t weakened  = intern_id(sy, "weakened"),
             can_force = intern_id(sy, "can_force_door"),
             antidote  = intern_id(sy, "has_antidote"),
             closed    = intern_id(sy, "door_closed"),
             open      = intern_id(sy, "door_open"),
             a_force   = intern_id(sy, "force_door");

    /* poisoned, no antidote: the exception beats the norm */
    CHECK(world_query(w, dl_pos(weakened))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(can_force)) == DL_REFUTED);

    /* forcing the door does nothing (condition unprovable, inertia holds) */
    char serr[256];
    CHECK(world_step(w, &a_force, 1, serr, sizeof serr) == 0);
    CHECK(world_get(w, closed) && !world_get(w, open));

    /* antidote picked up: defeater blocks 'weakened', the norm is reinstated */
    world_set(w, antidote, true);
    CHECK(world_query(w, dl_pos(weakened))  == DL_REFUTED);
    CHECK(world_query(w, dl_pos(can_force)) == DL_PROVED);

    CHECK(world_step(w, &a_force, 1, serr, sizeof serr) == 0);
    CHECK(!world_get(w, closed) && world_get(w, open));

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

/* Bad inputs produce at least one error, a located message, and no world. */
static int expect_error(const char *src)
{
    intern *sy = intern_new();
    story_diag ditems[8];
    story_diags diags = { ditems, 8, 0, 0 };
    world *w = story_compile(src, sy, &diags);
    int ok = (w == NULL) && (diags.nerrors >= 1) &&
             (diags.items[0].sev == STORY_ERROR) && (diags.items[0].line >= 1);
    if (!ok)
        fprintf(stderr, "FAIL expected error for <<%s>> (nerrors=%d)\n",
                src, diags.nerrors);
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_errors(void)
{
    if (expect_error("rule r: a =>"))                 return 1; /* missing head */
    if (expect_error("state ( hp(actor) )"))          return 1; /* args unsupported */
    if (expect_error("state ( hp : int )"))           return 1; /* typed unsupported */
    if (expect_error("init undeclared"))              return 1; /* not a fluent */
    if (expect_error("rule r: a => b\n r > ghost"))   return 1; /* unknown label */
    if (expect_error("rule r a => b"))                return 1; /* missing colon */
    if (expect_error("wat"))                          return 1; /* bare junk */
    return 0;
}

/* Orphan/typo detection (§6.1 Tier-1): a condition atom that is neither a
 * declared fluent nor concluded anywhere is a non-fatal warning, not an error;
 * a head concluded later in the file is not a false positive. */
static int test_orphan(void)
{
    /* a typo'd condition atom is flagged (and compilation still succeeds) */
    {
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *wl = story_compile("state holding\nrule r: hodling => weak",
                                  sy, &d);
        CHECK(wl != NULL);
        CHECK(d.nerrors == 0);
        CHECK(d.count == 1);
        CHECK(d.items[0].sev == STORY_WARNING);
        CHECK(strstr(d.items[0].msg, "hodling") != NULL);
        world_free(wl);
        intern_free(sy);
    }
    /* a head concluded later in the file must not be reported as an orphan */
    {
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *wl = story_compile(
            "state y\nrule a: derived => x\nrule b: y => derived", sy, &d);
        CHECK(wl != NULL);
        if (d.count) fprintf(stderr, "unexpected diagnostic: %s\n", d.items[0].msg);
        CHECK(d.count == 0);
        world_free(wl);
        intern_free(sy);
    }
    return 0;
}

/* Panic-mode recovery (§10): independent errors in separate declarations are
 * all reported — one bad declaration does not mask the rest of the file — and
 * valid declarations between them still compile. */
static int test_recovery(void)
{
    const char *src =
        "rule bad1: a =>\n"              /* err: missing head           */
        "rule ok:   strong => weak\n"    /* valid — must not be skipped */
        "action bad2: causes\n"          /* err: missing effect         */
        "state ( strong )\n";            /* valid                       */

    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, sy, &d);

    CHECK(w == NULL);                    /* errors present -> no world  */
    CHECK(d.nerrors == 2);               /* both, not just the first    */
    CHECK(d.items[0].sev == STORY_ERROR);
    CHECK(d.items[1].sev == STORY_ERROR);
    CHECK(d.items[0].line < d.items[1].line);   /* recovery moved forward */

    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_lexer())             return 1;
    if (test_cellar_from_story()) return 1;
    if (test_errors())            return 1;
    if (test_orphan())            return 1;
    if (test_recovery())          return 1;
    printf("test_parse: all passed\n");
    return 0;
}
