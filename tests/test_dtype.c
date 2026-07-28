/* Golden test for typed damage: #83 (the operative selector — `as fire` on a
 * contribution) + #84's N=1 commit stage (base → Σ per type → response →
 * clamp).
 *
 * The observable 5e semantics pinned here, in order of importance:
 *  - the response applies AFTER summation (two 3-damage fire hits resisted are
 *    floor(6/2)=3, not floor(3/2)+floor(3/2)=2) and BEFORE the clamp (a huge
 *    resisted hit halves first, then floors at the range);
 *  - halving floors the magnitude (3 resisted → 1);
 *  - resistance and vulnerability COEXIST AND CANCEL; immunity dominates both;
 *  - resistance does not stack: two rules concluding resistant(X, fire) is
 *    just `proved` — idempotent by construction, no merge algebra;
 *  - the response reads ordinary judgments over the PRE-step state, so it
 *    follows `raging`/`soaked` as they change;
 *  - untyped deltas ride along unscaled in the same tick;
 *  - the receipt stays coherent: base + Σ items == the committed value, with
 *    the scaling recorded as a row named after the deciding response atom. */

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

static world *compile_ok(const char *src, intern *sy)
{
    story_diag di[8];
    story_diags d = { di, 8, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    if (!w) fprintf(stderr, "  compile: %s\n", d.count ? d.items[0].msg : "?");
    else if (d.nerrors) fprintf(stderr, "  errors: %s\n", d.items[0].msg);
    return (w && d.nerrors == 0) ? w : NULL;
}

static int stepn(world *w, intern *sy, const char **actions, int n)
{
    uint32_t a[4];
    for (int i = 0; i < n; i++) a[i] = intern_id(sy, actions[i]);
    char err[128];
    int r = world_step(w, a, n, err, sizeof err);
    if (r) fprintf(stderr, "  step: %s\n", err);
    return r;
}

static int step1(world *w, intern *sy, const char *action)
{
    return stepn(w, sy, &action, 1);
}

static long hp(world *w, intern *sy, const char *ent)
{
    char b[32];
    snprintf(b, sizeof b, "hp(%s)", ent);
    return world_get_num(w, intern_id(sy, b));
}

static const char *SRC =
    "sort actor\n"
    "entity ( bran, grik : actor )\n"
    "enum damage_type { fire, cold, poison }\n"
    "state (\n"
    "    raging(actor)\n"
    "    soaked(actor)\n"
    "    hp(actor) : int in 0 .. 30\n"
    ")\n"
    "init ( hp(bran) = 30  hp(grik) = 30 )\n"
    "// the response vocabulary: plain judgments over current state\n"
    "rule rage_fire(X: actor):  raging(X) => resistant(X, fire)\n"
    "rule rage_fire2(X: actor): raging(X) => resistant(X, fire)   // non-stacking\n"
    "rule rage_cold(X: actor):  raging(X) => resistant(X, cold)\n"
    "rule stone(X: actor):      raging(X) => immune(X, poison)\n"
    "rule wet_cold(X: actor):   soaked(X) => vulnerable(X, cold)\n"
    "rule frail(X: actor):      soaked(X) => vulnerable(X, poison)\n"
    "// typed contributions\n"
    "action burn(T: actor):    causes hp(T) -= 3 as fire\n"
    "action burn2(T: actor):   causes hp(T) -= 3 as fire\n"
    "action bigburn(T: actor): causes hp(T) -= 20 as fire\n"
    "action chill(T: actor):   causes hp(T) -= 5 as cold\n"
    "action sting(T: actor):   causes hp(T) -= 6 as poison\n"
    "action slap(T: actor):    causes hp(T) -= 1\n"
    "action weaken(T: actor):  causes hp(T) := 5\n"
    "// an AoE through the set-quantified binder, typed\n"
    "action flamewave: causes for each T : actor : hp(T) -= 4 as fire\n"
    "action enrage(X: actor):  causes raging(X)\n"
    "action calm(X: actor):    causes ~raging(X)\n"
    "action soak(X: actor):    causes soaked(X)\n";

static int test_pipeline(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    /* no response rules fire: typed damage lands raw */
    CHECK(step1(w, sy, "burn(grik)") == 0);
    CHECK(hp(w, sy, "grik") == 27);

    /* sum THEN halve: two 3s resisted are floor(6/2)=3, not 1+1 */
    CHECK(step1(w, sy, "enrage(bran)") == 0);
    const char *two[] = { "burn(bran)", "burn2(bran)" };
    CHECK(stepn(w, sy, two, 2) == 0);
    CHECK(hp(w, sy, "bran") == 27);

    /* halving floors the magnitude: 3 → 1 (two rules conclude resistant —
     * still exactly half, non-stacking by construction) */
    CHECK(step1(w, sy, "burn(bran)") == 0);
    CHECK(hp(w, sy, "bran") == 26);

    /* untyped rides along unscaled in the same tick: 3-as-fire→1, plus 1 */
    const char *mix[] = { "burn(bran)", "slap(bran)" };
    CHECK(stepn(w, sy, mix, 2) == 0);
    CHECK(hp(w, sy, "bran") == 24);

    /* vulnerability doubles */
    CHECK(step1(w, sy, "soak(grik)") == 0);
    CHECK(step1(w, sy, "chill(grik)") == 0);
    CHECK(hp(w, sy, "grik") == 17);                /* 27 - 5*2 */

    /* resistance + vulnerability coexist and cancel: raw 5 */
    CHECK(step1(w, sy, "soak(bran)") == 0);        /* bran: raging AND soaked */
    CHECK(step1(w, sy, "chill(bran)") == 0);
    CHECK(hp(w, sy, "bran") == 19);                /* 24 - 5 */

    /* immunity dominates, even alongside vulnerability (soaked → vulnerable
     * to poison, raging → immune): 0 damage */
    CHECK(step1(w, sy, "sting(bran)") == 0);
    CHECK(hp(w, sy, "bran") == 19);

    /* the response follows state: calm bran, fire lands raw again */
    CHECK(step1(w, sy, "calm(bran)") == 0);
    CHECK(step1(w, sy, "burn(bran)") == 0);
    CHECK(hp(w, sy, "bran") == 16);

    world_free(w);
    intern_free(sy);
    return 0;
}

static int test_response_before_clamp(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "weaken(grik)") == 0);      /* hp(grik) := 5 */
    CHECK(step1(w, sy, "enrage(grik)") == 0);
    CHECK(step1(w, sy, "bigburn(grik)") == 0);     /* 20 → 10; 5-10 → clamp 0 */
    CHECK(hp(w, sy, "grik") == 0);

    /* receipt coherence on a resisted hit: base + Σ items == committed, and
     * the scaling row is named after the deciding response atom */
    long base;
    world_contrib items[8];
    int n = world_num_receipt(w, intern_id(sy, "hp(grik)"), &base, items, 8);
    CHECK(base == 5);
    long sum = base;
    bool saw_resist = false;
    for (int k = 0; k < n; k++) {
        sum += items[k].amount;
        if (items[k].rule && strstr(items[k].rule, "resistant(grik,fire)"))
            saw_resist = true;
    }
    CHECK(saw_resist);
    CHECK(sum == -5);                              /* pre-clamp value the pipeline built */

    world_free(w);
    intern_free(sy);
    return 0;
}

static int test_binder_typed(void)
{
    intern *sy = intern_new();
    world *w = compile_ok(SRC, sy);
    CHECK(w != NULL);

    CHECK(step1(w, sy, "enrage(bran)") == 0);
    CHECK(step1(w, sy, "flamewave") == 0);         /* 4 as fire to everyone */
    CHECK(hp(w, sy, "bran") == 28);                /* resisted: 4 → 2 */
    CHECK(hp(w, sy, "grik") == 26);                /* raw */

    world_free(w);
    intern_free(sy);
    return 0;
}

static int test_errors(void)
{
    static const struct { const char *src, *msg; } BAD[] = {
        { "sort actor\nentity a : actor\nenum damage_type { fire, cold }\n"
          "state hp(actor) : int\n"
          "action x(T: actor): causes hp(T) := 5 as fire\n",
          "not a `:=` assignment" },
        { "sort actor\nentity a : actor\nstate hp(actor) : int\n"
          "action x(T: actor): causes hp(T) -= 5 as bogus\n",
          "not a declared enum value" },
        { "sort actor\nentity a : actor\n"
          "enum damage_type { fire, cold }\nenum school { evocation, abjuration }\n"
          "state hp(actor) : int\n"
          "action x(T: actor): causes hp(T) -= 5 as fire\n"
          "action y(T: actor): causes hp(T) -= 5 as evocation\n",
          "ONE enum per world" },
        { "enum damage_type { fire, cold }\nstate gold : int\n"
          "action x: causes gold -= 5 as fire\n",
          "needs a subject" },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        intern *sy = intern_new();
        story_diag di[16];
        story_diags dg = { di, 16, 0, 0 };
        world *w = story_compile(BAD[i].src, "t.story", sy, &dg);
        if (w != NULL && dg.nerrors == 0) {
            fprintf(stderr, "FAIL %s:%d: case %zu compiled but should not\n",
                    __FILE__, __LINE__, i);
            return 1;
        }
        bool found = false;
        for (int k = 0; k < dg.count && !found; k++)
            found = strstr(dg.items[k].msg, BAD[i].msg) != NULL;
        if (!found) {
            fprintf(stderr, "FAIL %s:%d: case %zu missing \"%s\"; got \"%s\"\n",
                    __FILE__, __LINE__, i, BAD[i].msg,
                    dg.count ? dg.items[0].msg : "(none)");
            return 1;
        }
        if (w) world_free(w);
        intern_free(sy);
    }
    return 0;
}

int main(void)
{
    if (test_pipeline()) return 1;
    if (test_response_before_clamp()) return 1;
    if (test_binder_typed()) return 1;
    if (test_errors()) return 1;
    printf("test_dtype: all passed\n");
    return 0;
}
