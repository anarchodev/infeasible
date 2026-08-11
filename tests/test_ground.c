/* Golden test for the M1 grounder (DESIGN.md §5.2, §11): typed variables over
 * declared sorts, boolean predicate fluents grounded closed-world, per-instance
 * `unless` defeaters, and superiority grounded over the SHARED variable.
 *
 * Compiles examples/cellar_ground.story and pins that the two actors reach
 * opposite verdicts through the same rules. The superiority check is the point:
 * grounding `too_weak > can_force` as a cross product would let too_weak(hero)
 * (which fires) defeat can_force(guard) and wrongly refute can_force_door(guard).
 * Also checks the new declarations parse and that grounding errors are caught. */

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

/* The main event: compile the grounded cellar and pin every ground verdict. */
static int test_cellar_ground(void)
{
    char *src = read_file(STORY_DIR "/cellar_ground.story");
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag ditems[16];
    story_diags diags = { ditems, 16, 0, 0 };
    world *w = story_compile(src, NULL, sy, &diags);
    if (!w) {
        fprintf(stderr, "FAIL compile: %s\n",
                diags.count ? diags.items[0].msg : "(no message)");
        free(src); intern_free(sy);
        return 1;
    }
    if (diags.count)
        fprintf(stderr, "unexpected diagnostic: %s\n", diags.items[0].msg);
    CHECK(diags.count == 0);

    /* ground atoms intern as "pred(entity)" — the same id a host would query */
    uint32_t w_hero  = intern_id(sy, "weakened(hero)"),
             w_guard = intern_id(sy, "weakened(guard)"),
             cf_hero = intern_id(sy, "can_force_door(hero)"),
             cf_guard= intern_id(sy, "can_force_door(guard)"),
             closed  = intern_id(sy, "door_closed");

    /* the hero has no antidote: weakened, so the exception refutes can_force */
    CHECK(world_query(w, dl_pos(w_hero))  == DL_PROVED);
    CHECK(world_query(w, dl_pos(cf_hero)) == DL_REFUTED);

    /* the guard holds the antidote: the per-actor defeater reinstates the norm,
     * so weakened(guard) is refuted and can_force(guard) wins. This only holds
     * if superiority grounded per-actor, not across the cross product. */
    CHECK(world_query(w, dl_pos(w_guard))  == DL_REFUTED);
    CHECK(world_query(w, dl_pos(cf_guard)) == DL_PROVED);

    /* the hero's force_door can't fire (condition refuted); the door stays shut */
    uint32_t a_hero = intern_id(sy, "force_door(hero)");
    char err[256];
    CHECK(world_step(w, &a_hero, 1, err, sizeof err) == 0);
    CHECK(world_get(w, closed));

    /* the guard's force_door fires: the shared arity-0 fluent flips */
    uint32_t a_guard = intern_id(sy, "force_door(guard)");
    CHECK(world_step(w, &a_guard, 1, err, sizeof err) == 0);
    CHECK(!world_get(w, closed));

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

/* Grounder-specific error paths: unknown sort, unknown variable/entity in an
 * argument, and arity mismatch against a declared fluent. */
static int expect_error(const char *src, const char *needle)
{
    intern *sy = intern_new();
    story_diag ditems[8];
    story_diags diags = { ditems, 8, 0, 0 };
    world *w = story_compile(src, NULL, sy, &diags);
    int ok = (w == NULL) && (diags.nerrors >= 1) &&
             (diags.items[0].sev == STORY_ERROR);
    if (ok && needle && diags.count)
        ok = strstr(diags.items[0].msg, needle) != NULL;
    if (!ok)
        fprintf(stderr, "FAIL expected error<<%s>> for <<%s>> (nerrors=%d, msg=%s)\n",
                needle ? needle : "", src, diags.nerrors,
                diags.count ? diags.items[0].msg : "");
    if (w) world_free(w);
    intern_free(sy);
    return ok ? 0 : 1;
}

static int test_ground_errors(void)
{
    /* variable typed over a sort that was never declared */
    if (expect_error("rule r(X: ghost): p(X) => q(X)", "unknown sort")) return 1;
    /* an argument that is neither a bound variable nor a declared entity */
    if (expect_error("sort actor\nrule r(X: actor): near(X, nobody) => q(X)",
                     "neither a bound variable nor a declared entity")) return 1;
    /* arity mismatch: holding is declared with two args, used with one */
    if (expect_error("sort actor, item\nstate holding(actor, item)\n"
                     "rule r(X: actor): holding(X) => q(X)", "argument")) return 1;
    /* a sort mismatch between a variable and a fluent's declared argument */
    if (expect_error("sort actor, item\nstate strong(actor)\n"
                     "rule r(T: item): strong(T) => q(T)", "sort")) return 1;
    /* #205: a judgment has no declaration site, so its argument sorts come from
     * the rules concluding it — and a predicate has ONE signature. Keeping the
     * first sort silently would publish a smaller world than the engine has. */
    if (expect_error("sort actor, item\nentity hero : actor\nentity key : item\n"
                     "state showing\n"
                     "rule a: showing => shows(hero)\n"
                     "rule b: showing => shows(key)\n", "one signature")) return 1;
    /* the variable case is the same check: it is the SORT that must agree */
    if (expect_error("sort actor, item\nstate ( awake(actor)  broken(item) )\n"
                     "rule a(X: actor): awake(X)  => shows(X)\n"
                     "rule b(T: item):  broken(T) => shows(T)\n", "one signature")) return 1;
    /* #217: the READ side of that signature. `shows` is concluded over actors,
     * so `shows(K)` with `K : item` names an atom no rule can conclude — the
     * rule silently never fires, which is #205's failure pointing the other
     * way, at the author instead of at the client. */
    if (expect_error("sort actor, item\nstate ( awake(actor)  on_floor(item) )\n"
                     "rule lit(X: actor): awake(X) => shows(X)\n"
                     "rule draw(K: item): on_floor(K) & shows(K) => glow(K)\n",
                     "expects sort 'actor'")) return 1;
    /* the read may precede the rule that settles the signature — the check is
     * deferred, not order-dependent */
    if (expect_error("sort actor, item\nstate ( awake(actor)  on_floor(item) )\n"
                     "rule draw(K: item): on_floor(K) & shows(K) => glow(K)\n"
                     "rule lit(X: actor): awake(X) => shows(X)\n",
                     "expects sort 'actor'")) return 1;
    /* an entity read in a `requires` is the same check in another scope */
    if (expect_error("sort actor, item\nentity ( hero : actor  key : item )\n"
                     "state ( awake(actor)  hp(actor) : int in 0..10 )\n"
                     "rule lit(X: actor): awake(X) => shows(X)\n"
                     "action poke(X: actor): requires shows(key) causes hp(X) -= 1\n",
                     "expects sort 'actor'")) return 1;
    /* and so is a test(…) reifying the judgment inside an effect expression */
    if (expect_error("sort actor, item\nentity ( hero : actor  key : item )\n"
                     "state ( awake(actor)  hp(actor) : int in 0..10 )\n"
                     "rule lit(X: actor): awake(X) => shows(X)\n"
                     "action poke(X: actor): causes hp(X) := 10 - test(shows(key))\n",
                     "expects sort 'actor'")) return 1;
    return 0;
}

/* #217: a judgment NOTHING concludes has no signature to check a read against,
 * and that case belongs to the orphan pass — which warns rather than failing
 * the compile, because a condition may legitimately await a rule. */
static int test_head_read_orphan(void)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(
        "sort actor, item\nstate awake(actor)\n"
        "rule r(K: item): awake(hero) & unheard_of(K) => noticed(K)\n"
        "entity hero : actor\n",
        NULL, sy, &d);
    CHECK(w != NULL);
    CHECK(d.nerrors == 0);
    CHECK(d.count == 1 && d.items[0].sev == STORY_WARNING);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* #205/#217, the other side: rules that agree on a judgment's sorts compile
 * clean — where it is concluded and where it is read alike, whether each
 * argument arrives as a typed parameter or as a ground entity. */
static int test_head_sorts_agree(void)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(
        "sort actor, item\n"
        "entity ( hero, guard : actor  key : item )\n"
        "state ( showing  holding(actor, item) )\n"
        "rule a(X: actor, K: item): holding(X, K) => visible(X, K)\n"
        "rule b: showing => visible(hero, key)\n"
        "rule c(X: actor, K: item): visible(X, K) => noticed(X)\n"
        "rule d: visible(hero, key) => alerted\n",
        NULL, sy, &d);
    CHECK(w != NULL);
    CHECK(d.nerrors == 0);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* Grounding an empty sort produces no ground rules and no crash. */
static int test_empty_sort(void)
{
    intern *sy = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    /* sort actor is declared but has no entities: holding grounds to nothing */
    world *w = story_compile(
        "sort actor\nstate alive(actor)\nrule r(X: actor): alive(X) => up(X)",
        NULL, sy, &d);
    CHECK(w != NULL);
    CHECK(d.nerrors == 0);
    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_cellar_ground())    return 1;
    if (test_ground_errors())    return 1;
    if (test_head_sorts_agree()) return 1;
    if (test_head_read_orphan()) return 1;
    if (test_empty_sort())       return 1;
    printf("test_ground: all passed\n");
    return 0;
}
