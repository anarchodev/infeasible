/* Golden test for `set of` transient action params (issue #5; DESIGN.md §13).
 *
 * A `set of SORT` action parameter is a transient, host-answered membership
 * relation (§5.6 providers): the host supplies the set per cast, and
 * `T in P` / `T not in P` in a binder guard read it. This is the faithful
 * "half on a save" shape — Fireball where the failed-save set is host-decided,
 * full damage to its members and half to the rest — distinct from the
 * state-guarded binder (`where in_blast(T)`) that already worked. */

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

/* Host oracle: grik, gnok, thorn are in the blast; grik and gnok failed the
 * save. Both the geometry and the save outcome arrive through the one provider
 * callback — the engine stores neither (I1). */
static intern *SY;
static uint32_t IN_BLAST, SAVE_FAILED, GRIK, GNOK, THORN;

static bool prov(void *ctx, uint32_t pred, const uint32_t *args, int n)
{
    (void)ctx; (void)n;
    if (pred == IN_BLAST)    return args[0] == GRIK || args[0] == GNOK || args[0] == THORN;
    if (pred == SAVE_FAILED) return args[0] == GRIK || args[0] == GNOK;
    return false;
}

static long hp(world *w, const char *ent)
{
    char b[32];
    snprintf(b, sizeof b, "hp(%s)", ent);
    return world_get_num(w, intern_id(SY, b));
}

static int test_save(void)
{
    const char *src =
        "sort actor\n"
        "provider in_blast(actor)\n"
        "entity ( vera, grik, gnok, thorn : actor )\n"
        "state ( hp(actor) : int in 0 .. 60 )\n"
        "init ( hp(grik)=14 hp(gnok)=14 hp(thorn)=30 )\n"
        "action fireball(C: actor, save_failed: set of actor):\n"
        "    causes for each T: actor where in_blast(T): {\n"
        "        hp(T) -= 8 when T in save_failed,\n"
        "        hp(T) -= 4 when T not in save_failed\n"
        "    }\n";

    SY = intern_new();
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", SY, &d);
    if (!w) { fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?"); return 1; }
    CHECK(d.nerrors == 0);

    IN_BLAST = intern_id(SY, "in_blast");  SAVE_FAILED = intern_id(SY, "save_failed");
    GRIK = intern_id(SY, "grik");  GNOK = intern_id(SY, "gnok");  THORN = intern_id(SY, "thorn");
    world_set_provider_fn(w, prov, NULL);

    uint32_t act = intern_id(SY, "fireball(vera)");
    char err[128];
    CHECK(world_step(w, &act, 1, err, sizeof err) == 0);

    CHECK(hp(w, "grik")  == 6);    /* in blast, failed save: full 8 */
    CHECK(hp(w, "gnok")  == 6);    /* in blast, failed save: full 8 */
    CHECK(hp(w, "thorn") == 26);   /* in blast, saved:       half 4 */

    world_free(w);
    intern_free(SY);
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
    /* `set of` is only for actions, not rules */
    if (expect_error_msg(
            "sort actor\nrule r(s: set of actor): a => b\n",
            "only allowed on actions"))
        return 1;
    /* `not` without `in` */
    if (expect_error_msg(
            "sort actor\nentity ( a : actor )\nstate ( p(actor) )\n"
            "action go(x: set of actor): requires a not p causes p(a)\n",
            "`in` after `not`"))
        return 1;
    return 0;
}

int main(void)
{
    if (test_save())   return 1;
    if (test_errors()) return 1;
    printf("test_setparam: all passed\n");
    return 0;
}
