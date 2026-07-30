/* Golden test for `exclusive` groups (#159, §5.13 / EPIC #154).
 *
 * A declared group is a CHECKED action-exclusivity protocol: named variables
 * form the group's key (matched by name across members; `_` never
 * constrains), and a step may contain at most one member instance per key
 * tuple. What used to be an unverifiable host promise ("we never co-submit
 * east and west for one guard") becomes:
 *
 *  - a step-time rejection: world_step refuses a violating action set
 *    PRE-SOLVE, state untouched — host-protocol class, same posture as the
 *    #119 unknown-action check, retired for bound hosts by the §6.3 binding;
 *  - a #98 exclusivity source: a conflictable pair (or self-collision) whose
 *    collision FORCES key equality is covered by the group and stops
 *    warning; a collision that leaves a key free (an arity-0 fluent) is NOT
 *    covered — instances with different keys still contest, and the
 *    unchanged warning plus the runtime contested error prove it. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

/* Does the source FAIL to compile with an error containing `frag`? */
static int nerr(const char *src, const char *frag)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int hit = 0;
    if (w == NULL && d.nerrors > 0) {
        for (int i = 0; i < d.count && i < d.cap; i++)
            if (strstr(di[i].msg, frag)) hit = 1;
    }
    if (w) world_free(w);
    intern_free(sy);
    return hit;
}

/* count diagnostics containing `frag`; -1 = compile error */
static int nwarn(const char *src, const char *frag)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int n = -1;
    if (w && d.nerrors == 0) {
        n = 0;
        for (int i = 0; i < d.count && i < d.cap; i++)
            if (strstr(di[i].msg, frag)) n++;
    } else if (d.count) {
        fprintf(stderr, "  compile: %s\n", di[0].msg);
    }
    if (w) world_free(w);
    intern_free(sy);
    return n;
}

static int step2(world *w, intern *sy, const char *a1, const char *a2,
                 char *err, size_t errsz)
{
    uint32_t as[2] = { intern_id(sy, a1), a2 ? intern_id(sy, a2) : 0 };
    return world_step(w, as, a2 ? 2 : 1, err, errsz);
}

static long num(world *w, intern *sy, const char *atom)
{
    return world_get_num(w, intern_id(sy, atom));
}

static const char *EW_SRC =
    "sort actor\n"
    "entity ( g1 : actor  g2 : actor )\n"
    "state pos(actor) : int\n"
    "action east(G: actor): causes pos(G) := 1\n"
    "action west(G: actor): causes pos(G) := 9\n"
    "exclusive east(G: actor), west(G: actor)\n";

/* --- the step-time rejection: same key rejects, distinct keys pass,
 *     state untouched on rejection --- */
static int test_step_rejection(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(EW_SRC, sy);
    CHECK(w != NULL);
    char err[192] = "";

    CHECK(step2(w, sy, "east(g1)", NULL, err, sizeof err) == 0);
    CHECK(num(w, sy, "pos(g1)") == 1);

    CHECK(step2(w, sy, "east(g1)", "west(g1)", err, sizeof err) == -1);
    CHECK(strstr(err, "declared exclusive") != NULL);
    CHECK(strstr(err, "east/west") != NULL);       /* the group's label */
    CHECK(num(w, sy, "pos(g1)") == 1);             /* rejection is atomic */

    CHECK(step2(w, sy, "east(g1)", "west(g2)", err, sizeof err) == 0);
    CHECK(num(w, sy, "pos(g1)") == 1);             /* distinct keys pass */
    CHECK(num(w, sy, "pos(g2)") == 9);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- the wildcard / self-exclusive form: one strike per attacker per
 *     step, whoever the target is; the same atom twice is one action --- */
static int test_self_exclusive(void)
{
    static const char *src =
        "sort actor\n"
        "entity ( a1 : actor  a2 : actor  t1 : actor  t2 : actor )\n"
        "state die(actor) : int\n"
        "action strike(A: actor, T: actor): causes die(A) := roll(20)\n"
        "exclusive strike(A, _)\n";
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    char err[192] = "";

    CHECK(step2(w, sy, "strike(a1,t1)", "strike(a1,t2)", err, sizeof err) == -1);
    CHECK(strstr(err, "declared exclusive") != NULL);
    CHECK(step2(w, sy, "strike(a1,t1)", "strike(a2,t2)", err, sizeof err) == 0);
    CHECK(step2(w, sy, "strike(a1,t1)", "strike(a1,t1)", err, sizeof err) == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- arity-0 members: the key is (), any two distinct members conflict --- */
static int test_arity0_group(void)
{
    static const char *src =
        "state door : { open, closed }\n"
        "init door = closed\n"
        "action jam_open:   causes door = open\n"
        "action jam_closed: causes door = closed\n"
        "exclusive jam_open, jam_closed\n";
    intern *sy = intern_new();
    world *w = compile_ok(src, sy);
    CHECK(w != NULL);
    char err[192] = "";
    CHECK(step2(w, sy, "jam_open", "jam_closed", err, sizeof err) == -1);
    CHECK(strstr(err, "declared exclusive") != NULL);
    CHECK(step2(w, sy, "jam_open", NULL, err, sizeof err) == 0);
    world_free(w);
    intern_free(sy);
    return 0;
}

/* --- #98 coverage: a group whose key the collision FORCES suppresses the
 *     warning; one it does not (arity-0 fluent) leaves it — and the runtime
 *     contested error remains reachable exactly there --- */
static int test_warning_coverage(void)
{
    /* covered: pos(G) collision forces G equal, the group keys G */
    CHECK(nwarn(EW_SRC, "both assign") == 0);
    /* the same pair WITHOUT the declaration is a #160 error (control) */
    static const char *NO_DECL =
        "sort actor\n"
        "entity ( g1 : actor  g2 : actor )\n"
        "state pos(actor) : int\n"
        "action east(G: actor): causes pos(G) := 1\n"
        "action west(G: actor): causes pos(G) := 9\n";
    CHECK(nerr(NO_DECL, "both assign") == 1);

    /* NOT covered: the mode collision leaves G free — wake(g1)+sleep(g2)
     * would pass the group and contest at commit, so since #160 the keyed
     * group does NOT rescue the compile: still an error */
    static const char *MODE_KEYED =
        "sort actor\n"
        "entity ( g1 : actor  g2 : actor )\n"
        "state mode : { calm, alert }\n"
        "init mode = calm\n"
        "action wake(G: actor):  causes mode = alert\n"
        "action sleep(G: actor): causes mode = calm\n"
        "exclusive wake(G), sleep(G)\n";
    CHECK(nerr(MODE_KEYED, "conflicting effects on 'mode'") == 1);
    /* an all-wildcard group forbids ANY co-submission: covered */
    static const char *MODE_WILD =
        "sort actor\n"
        "entity ( g1 : actor  g2 : actor )\n"
        "state mode : { calm, alert }\n"
        "init mode = calm\n"
        "action wake(G: actor):  causes mode = alert\n"
        "action sleep(G: actor): causes mode = calm\n"
        "exclusive wake(_), sleep(_)\n";
    CHECK(nwarn(MODE_WILD, "conflicting effects") == 0);

    /* self-collision covered by the wildcard form */
    static const char *STRIKE =
        "sort actor\n"
        "entity ( a1 : actor  t1 : actor )\n"
        "state die(actor) : int\n"
        "action strike(A: actor, T: actor): causes die(A) := roll(20)\n"
        "exclusive strike(A, _)\n";
    CHECK(nwarn(STRIKE, "more than once in one step") == 0);

    /* an uncovered BINDER var collides within ONE submitted action — no
     * protocol group can forbid it; since #160 that is an error */
    static const char *BINDER =
        "sort actor\n"
        "entity ( a1 : actor  a2 : actor )\n"
        "state ( marked(actor)  tally : int  hp(actor) : int )\n"
        "action tally_up: causes for each T: actor where marked(T):\n"
        "    tally := hp(T)\n"
        "exclusive tally_up\n";
    CHECK(nerr(BINDER, "more than once in one step") == 1);

    return 0;
}

/* --- misuse is a located error; the no-op shape warns --- */
static int test_errors(void)
{
    static const char *HDR =
        "sort ( actor  item )\n"
        "entity ( g1 : actor  i1 : item )\n"
        "state ( pos(actor) : int  p )\n"
        "action east(G: actor): causes pos(G) := 1\n"
        "action west(G: actor): causes pos(G) := 9\n"
        "action strike(A: actor, T: actor): causes pos(A) := 2\n"
        "rule ram: p causes ~p\n";
    static const struct { const char *extra, *msg; } BAD[] = {
        { "exclusive nosuch(G)\n",           "not a declared action" },
        { "exclusive ram\n",                 "is a ramification" },
        { "exclusive east(G, X)\n",          "lists 2" },
        { "exclusive east(G), west(_)\n",    "missing from member" },
        { "exclusive east(G), east(G)\n",    "appears twice" },
        { "exclusive strike(A, A)\n",        "repeats within one member" },
        { "exclusive east(G: item), west(G: item)\n", "annotates it" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char src[1024];
        snprintf(src, sizeof src, "%s%s", HDR, BAD[i].extra);
        intern *sy = intern_new();
        story_diag di[8];
        story_diags d = { di, 8, 0, 0 };
        world *w = story_compile(src, "t.story", sy, &d);
        int hit = 0;
        for (int k = 0; k < d.count && k < d.cap; k++)
            if (strstr(di[k].msg, BAD[i].msg)) hit = 1;
        if (w != NULL || d.nerrors == 0 || !hit) {
            fprintf(stderr, "FAIL %s:%d: case %zu (wanted \"%s\", got \"%s\")\n",
                    __FILE__, __LINE__, i, BAD[i].msg,
                    d.count ? di[0].msg : "");
            return 1;
        }
        intern_free(sy);
    }
    /* single member keying every position forbids nothing: a warning (on a
     * minimal source — HDR's east/west pair is itself a #160 error now) */
    CHECK(nwarn("sort actor\n"
                "entity g1 : actor\n"
                "state pos(actor) : int\n"
                "action strike(A: actor, T: actor): causes pos(A) := 2\n"
                "exclusive strike(A, T)\n",
                "keys every argument") == 1);
    return 0;
}

int main(void)
{
    if (test_step_rejection()) return 1;
    if (test_self_exclusive()) return 1;
    if (test_arity0_group()) return 1;
    if (test_warning_coverage()) return 1;
    if (test_errors()) return 1;
    printf("test_excl: all passed\n");
    return 0;
}
