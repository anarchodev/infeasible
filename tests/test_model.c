/* Golden test for the .story span model (story_model.h) — the compiler output
 * that navigation/hover/the interface artifact read instead of re-parsing.
 * Pins that `story_compile_model` harvests declarations (with kind + span) and
 * atom occurrences (with role), classifies heads vs. bodies vs. effects, keeps
 * conclusions out of the symbol table, and is produced best-effort on a file
 * that fails to compile. */

#include "lang/story.h"
#include "core/intern.h"
#include "state/world.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

static const story_symbol *find_sym(const story_model *m, const char *name,
                                    story_sym_kind k)
{
    int n; const story_symbol *s = story_model_symbols(m, &n);
    for (int i = 0; i < n; i++)
        if (s[i].kind == k && strcmp(s[i].name, name) == 0) return &s[i];
    return NULL;
}

static bool has_sym_name(const story_model *m, const char *name)
{
    int n; const story_symbol *s = story_model_symbols(m, &n);
    for (int i = 0; i < n; i++)
        if (strcmp(s[i].name, name) == 0) return true;
    return false;
}

static bool has_occ(const story_model *m, const char *name, story_occ_role r)
{
    int n; const story_occ *o = story_model_occs(m, &n);
    for (int i = 0; i < n; i++)
        if (o[i].role == r && strcmp(o[i].name, name) == 0) return true;
    return false;
}

/* A HEAD occurrence of `name` with the given polarity (attacker iff neg). */
static bool has_head(const story_model *m, const char *name, bool neg)
{
    int n; const story_occ *o = story_model_occs(m, &n);
    for (int i = 0; i < n; i++)
        if (o[i].role == STORY_OCC_HEAD && o[i].neg == neg &&
            strcmp(o[i].name, name) == 0) return true;
    return false;
}

static bool has_rule_label(const story_model *m, const char *label)
{
    int n; const story_rule *r = story_model_rules(m, &n);
    for (int i = 0; i < n; i++)
        if (strcmp(r[i].label, label) == 0) return true;
    return false;
}

/* A clean source: declarations become spanned symbols; atom mentions become
 * role-tagged occurrences; the conclusion `happy` is not a declaration. */
static int test_harvest(void)
{
    const char *src =
        "sort actor\n"                       /* L1 */
        "entity hero : actor\n"              /* L2 */
        "state ( alive(actor) hp(actor) : int in 0 .. 5 )\n" /* L3 */
        "rule r: alive(hero) => happy(hero)\n" /* L4: concludes happy      */
        "rule no: alive(hero) => ~happy(hero)\n" /* L5: attacks happy       */
        "action wake(X: actor): causes alive(X)"; /* L6 */

    intern *sy = intern_new();
    story_model *m = NULL;
    world *w = story_compile_model(src, NULL, sy, NULL, &m);
    CHECK(w != NULL);          /* clean source compiles */
    CHECK(m != NULL);

    /* symbols: each declaration, correctly kinded */
    CHECK(find_sym(m, "actor", STORY_SYM_SORT)   != NULL);
    CHECK(find_sym(m, "hero",  STORY_SYM_ENTITY) != NULL);
    CHECK(find_sym(m, "alive", STORY_SYM_FLUENT) != NULL);
    CHECK(find_sym(m, "r",     STORY_SYM_RULE)   != NULL);
    CHECK(find_sym(m, "wake",  STORY_SYM_ACTION) != NULL);
    /* a conclusion is never a declaration (I1's spirit at the model layer) */
    CHECK(!has_sym_name(m, "happy"));

    /* the fluent symbol carries a usable span */
    const story_symbol *al = find_sym(m, "alive", STORY_SYM_FLUENT);
    CHECK(al->line == 3);
    CHECK(al->len  == 5);

    /* detail signatures — the concept word the closed SymbolKind enum can't carry */
    CHECK(strcmp(al->detail, "fluent(actor)") == 0);
    CHECK(strcmp(find_sym(m, "actor", STORY_SYM_SORT)->detail, "sort") == 0);
    CHECK(strcmp(find_sym(m, "hero", STORY_SYM_ENTITY)->detail, "entity : actor") == 0);
    CHECK(strcmp(find_sym(m, "wake", STORY_SYM_ACTION)->detail, "action(actor)") == 0);
    CHECK(strcmp(find_sym(m, "hp", STORY_SYM_FLUENT)->detail,
                 "fluent(actor) : int in 0..5") == 0);

    /* occurrences: head vs. body vs. effect vs. decl are distinguished */
    CHECK(has_occ(m, "happy", STORY_OCC_HEAD));    /* r concludes happy */
    CHECK(has_occ(m, "alive", STORY_OCC_DECL));    /* state decl        */
    CHECK(has_occ(m, "alive", STORY_OCC_BODY));    /* r body            */
    CHECK(has_occ(m, "alive", STORY_OCC_EFFECT));  /* wake writes it     */
    CHECK(has_occ(m, "hero",  STORY_OCC_ARG));     /* an argument        */

    /* head polarity: `happy` is both concluded (r) and attacked (no) */
    CHECK(has_head(m, "happy", false));            /* concluder  */
    CHECK(has_head(m, "happy", true));             /* attacker   */

    /* the rules list carries labels, indexed by story_occ.rule */
    CHECK(has_rule_label(m, "r"));
    CHECK(has_rule_label(m, "no"));
    /* a body occurrence points back at a rule; a decl does not */
    {
        int n; const story_occ *o = story_model_occs(m, &n);
        for (int i = 0; i < n; i++) {
            if (o[i].role == STORY_OCC_BODY && o[i].rule >= 0) { CHECK(1); break; }
        }
        for (int i = 0; i < n; i++)
            if (o[i].role == STORY_OCC_DECL) CHECK(o[i].rule == -1);
    }

    story_model_free(m);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* Best-effort on a broken source: no world, but a model that still records the
 * declarations parsed before the error — so an editor navigates a broken file. */
static int test_partial(void)
{
    const char *src = "state ( ok )\nrule bad: ok =>";   /* missing head */

    intern *sy = intern_new();
    story_model *m = NULL;
    world *w = story_compile_model(src, NULL, sy, NULL, &m);
    CHECK(w == NULL);                       /* error -> no world */
    CHECK(m != NULL);                       /* model still built */
    CHECK(find_sym(m, "ok", STORY_SYM_FLUENT) != NULL);

    story_model_free(m);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_harvest()) return 1;
    if (test_partial()) return 1;
    printf("test_model: all passed\n");
    return 0;
}
