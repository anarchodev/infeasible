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
    char err[256] = "";
    story_warning witems[8];
    story_warnings warn = { witems, 8, 0 };
    world *w = story_compile(src, sy, err, sizeof err, &warn);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n", err);
        free(src);
        intern_free(sy);
        return 1;
    }
    /* the reference cellar is clean: no orphan atoms */
    if (warn.count != 0)
        fprintf(stderr, "unexpected warning: %s\n", warn.items[0].msg);
    CHECK(warn.count == 0);

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

/* Bad inputs fail fast with a located, non-empty message and no world. */
static int expect_error(const char *src)
{
    intern *sy = intern_new();
    char err[256] = "";
    world *w = story_compile(src, sy, err, sizeof err, NULL);
    int ok = (w == NULL) && (err[0] != '\0');
    if (!ok)
        fprintf(stderr, "FAIL expected error for <<%s>> (err=\"%s\")\n", src, err);
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
        char err[256] = "";
        story_warning wi[8];
        story_warnings wn = { wi, 8, 0 };
        world *wl = story_compile("state holding\nrule r: hodling => weak",
                                  sy, err, sizeof err, &wn);
        CHECK(wl != NULL);
        CHECK(wn.count == 1);
        CHECK(strstr(wn.items[0].msg, "hodling") != NULL);
        world_free(wl);
        intern_free(sy);
    }
    /* a head concluded later in the file must not be reported as an orphan */
    {
        intern *sy = intern_new();
        char err[256] = "";
        story_warning wi[8];
        story_warnings wn = { wi, 8, 0 };
        world *wl = story_compile(
            "state y\nrule a: derived => x\nrule b: y => derived",
            sy, err, sizeof err, &wn);
        CHECK(wl != NULL);
        if (wn.count) fprintf(stderr, "unexpected warning: %s\n", wn.items[0].msg);
        CHECK(wn.count == 0);
        world_free(wl);
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_lexer())             return 1;
    if (test_cellar_from_story()) return 1;
    if (test_errors())            return 1;
    if (test_orphan())            return 1;
    printf("test_parse: all passed\n");
    return 0;
}
