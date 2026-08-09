/* Golden test for burst cues — the transient `emit` channel (#11, DESIGN.md
 * §12): the presentation interface's one-shot event stream.
 *
 * What is pinned here is the whole contract a client renders against:
 *
 *   - an emission is an OUTPUT of a step, read off the same solve the next
 *     state is (world_emits), in declaration order (I4);
 *   - it is not state: no fact, no inertia, no commit — the buffer holds the
 *     LAST step's cues and nothing else, so a cue fired on tick n is absent on
 *     tick n+1 without anyone retracting it;
 *   - a rejected step (the #119 loud no-op) emits nothing at all;
 *   - a cue is a proposition about the transition, not a count: two rules
 *     firing one cue in a step emit it ONCE;
 *   - defeasibility lives upstream — the cue rides whichever way the judgment
 *     argument settled, and `why` explains the firing like any effect;
 *   - the front end keeps the channel write-only: reading, negating, priming,
 *     or typing a cue is a located compile error.
 *
 * The semantics half runs against examples/cues.story (which test_examples
 * pins the compilation of); the engine half builds a world by hand, as
 * test_world does, so the surface and the API are pinned independently. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    size_t rd = fread(s, 1, (size_t)n, f);
    s[rd] = 0;
    fclose(f);
    return s;
}

/* The step's emission stream, rendered as "a b c" in buffer order. */
static void emit_str(world *w, intern *sy, char *buf, size_t cap)
{
    int n;
    const uint32_t *em = world_emits(w, &n);
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        int k = snprintf(buf + off, cap - off, "%s%s", off ? " " : "",
                         intern_name(sy, em[i]));
        if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
    }
}

/* ---- the surface: examples/cues.story ------------------------------------ */

static int test_cues_story(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/cues.story", STORY_DIR);
    char *src = slurp(path);
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag di[32];
    story_diags d = { di, 32, 0, 0 };
    world *w = story_compile(src, "cues.story", sy, &d);
    if (!w && d.count) fprintf(stderr, "  compile: %s\n", di[0].msg);
    CHECK(w && d.nerrors == 0);

    uint32_t bolt_v   = intern_id(sy, "firebolt(grunk,vera)"),
             bolt_g   = intern_id(sy, "firebolt(vera,grunk)"),
             resist_g = intern_id(sy, "firebolt_resisted(vera,grunk)"),
             mark_g   = intern_id(sy, "hunters_mark(vera,grunk)"),
             clap     = intern_id(sy, "thunderclap(vera)");
    char err[256], got[256];

    /* a plain hit: the cue rides with the damage */
    CHECK(world_step(w, &bolt_v, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "spark(vera)") == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(vera)")) == 9);

    /* NOT state: the next step does not re-emit it (no inertia carries a cue,
     * and nothing had to retract it) */
    CHECK(world_step(w, &clap, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "thunder") == 0);

    /* the resisted arm: two cues from one rule, in declaration order — spark
     * is declared before resisted, and the buffer follows the declaration, not
     * the order the effects were written */
    CHECK(world_step(w, &resist_g, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "spark(grunk) resisted(grunk)") == 0);
    CHECK(world_get_num(w, intern_id(sy, "hp(grunk)")) == 4);

    /* a cue is a proposition, not a count: both actions fire spark(grunk) in
     * ONE step, and it is emitted once */
    uint32_t both[2] = { resist_g, mark_g };
    CHECK(world_step(w, both, 2, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "spark(grunk) resisted(grunk)") == 0);
    CHECK(world_get(w, intern_id(sy, "marked(grunk)")));
    CHECK(world_get_num(w, intern_id(sy, "hp(grunk)")) == 1);

    /* the argument settled the other way now (`marked` burns through
     * resistance): the unresisted arm fires, so no `resisted` cue — and the
     * death ramification's cue rides in the SAME buffer as the hit's, so the
     * client never reconstructs causality from a state diff */
    CHECK(world_step(w, &bolt_g, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "spark(grunk) death_cry(grunk)") == 0);
    CHECK(!world_get(w, intern_id(sy, "alive(grunk)")));

    /* why? over a fired cue reads like any effect — the firing rule, its body,
     * its source span */
    {
        char tmp[4096];
        FILE *f = tmpfile();
        CHECK(f != NULL);
        world_step_why(w, dl_pos(intern_id(sy, "death_cry(grunk)")), true, f);
        rewind(f);
        size_t n = fread(tmp, 1, sizeof tmp - 1, f);
        tmp[n] = '\0';
        fclose(f);
        CHECK(strstr(tmp, "defeasible: PROVED") != NULL);
        CHECK(strstr(tmp, "death_cry(grunk)") != NULL);
        CHECK(strstr(tmp, "cues.story:") != NULL);
    }

    /* a guard that fails is a normal step that emits nothing (grunk is down) */
    CHECK(world_step(w, &bolt_g, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "") == 0);

    /* a REJECTED step emits nothing — the buffer is the last step's, and a
     * step that did not happen has none (#119 loud no-op) */
    CHECK(world_step(w, &clap, 1, err, sizeof err) == 0);   /* prime the buffer */
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "thunder") == 0);
    uint32_t bogus = intern_id(sy, "no_such_action");
    CHECK(world_step(w, &bogus, 1, err, sizeof err) == -1);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "") == 0);

    world_free(w);
    intern_free(sy);
    free(src);
    return 0;
}

/* ---- the engine surface, hand-built -------------------------------------- */

static int test_emit_engine(void)
{
    intern *sy = intern_new();
    uint32_t alive = intern_id(sy, "alive"),
             loud  = intern_id(sy, "loud"),
             cry   = intern_id(sy, "cry"),
             spark = intern_id(sy, "spark"),
             a_hit = intern_id(sy, "act_hit"),
             a_wait = intern_id(sy, "act_wait");

    world *w = world_new(sy);
    world_declare_fluent(w, alive);
    world_declare_fluent(w, loud);
    world_set(w, alive, true);
    world_declare_emit(w, cry);
    world_declare_emit(w, spark);

    CHECK(world_has_emit(w, cry) && world_has_emit(w, spark));
    CHECK(!world_has_emit(w, alive));

    /* hit: kills, sparks. A ramification cries over the corpse — a cue fired
     * by an indirect effect, in the same step. */
    dl_lit hit_eff[] = { dl_neg(alive), dl_pos(spark) };
    world_add_step_rule(w, "hit", a_hit, NULL, 0, hit_eff, 2);
    /* the cry is an EDGE, not a level: alive now, dead next. A ramification
     * conditioned on `~alive'` alone would fire every step thereafter — a cue
     * is momentary, so its condition has to be too. */
    step_cond died[] = { { { alive, false }, false }, { { alive, true }, true } };
    dl_lit cry_eff = dl_pos(cry);
    world_add_step_rule(w, "wail", INTERN_NONE, died, 2, &cry_eff, 1);
    world_add_step_rule(w, "wait", a_wait, NULL, 0, NULL, 0);

    char err[256], got[256];
    int n;
    CHECK(world_emits(w, &n) == NULL || n == 0);          /* nothing yet */

    CHECK(world_step(w, &a_hit, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "cry spark") == 0);                 /* declaration order */
    CHECK(!world_get(w, alive));

    /* an emission is not a fact: it never entered the store, and the next step
     * neither carries it (no inertia) nor has to retract it */
    CHECK(!world_get(w, cry));
    CHECK(world_query(w, dl_pos(cry)) == DL_UNDECIDED);
    CHECK(world_step(w, &a_wait, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "") == 0);

    /* replay determinism (I4): the same state + actions emit the same stream */
    world_set(w, alive, true);
    CHECK(world_step(w, &a_hit, 1, err, sizeof err) == 0);
    emit_str(w, sy, got, sizeof got);
    CHECK(strcmp(got, "cry spark") == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- the front end keeps the channel write-only -------------------------- */

/* Does `src` fail to compile with an error containing `frag`? */
static int fails_with(const char *src, const char *frag)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &d);
    int ok = 0;
    if (!w && d.nerrors > 0) {
        for (int i = 0; i < d.count && i < d.cap; i++)
            if (di[i].sev == STORY_ERROR && strstr(di[i].msg, frag)) ok = 1;
        if (!ok && d.count) fprintf(stderr, "  got: %s\n", di[0].msg);
    } else {
        fprintf(stderr, "  expected an error containing '%s', compiled clean\n", frag);
    }
    if (w) world_free(w);
    intern_free(sy);
    return ok;
}

#define PRE "sort actor\nentity a : actor\nstate hurt(actor)\n" \
            "emit spark(actor)\n"

static int test_emit_errors(void)
{
    /* read in a rule body — the whole point of the discipline */
    CHECK(fails_with(PRE "rule r(X: actor): spark(X) => seen(X)\n",
                     "is an emission"));
    /* read in a `requires` */
    CHECK(fails_with(PRE "action p(X: actor): requires spark(X) causes hurt(X)\n",
                     "is an emission"));
    /* read primed in a ramification body */
    CHECK(fails_with(PRE "rule r(X: actor): spark(X)' causes hurt(X)\n",
                     "is an emission"));
    /* concluded by a judgment rule */
    CHECK(fails_with(PRE "rule r(X: actor): hurt(X) => spark(X)\n",
                     "is an emission"));
    /* set as an init fact */
    CHECK(fails_with(PRE "init spark(a)\n", "spark"));
    /* negated in an effect */
    CHECK(fails_with(PRE "action p(X: actor): requires hurt(X) causes ~spark(X)\n",
                     "fired, never suppressed"));
    /* primed in an effect */
    CHECK(fails_with(PRE "action p(X: actor): requires hurt(X) causes spark(X)'\n",
                     "only ever about this transition"));
    /* a value type on the declaration */
    CHECK(fails_with("sort actor\nemit spark(actor) : int\n",
                     "no value type"));
    /* arity checked like any predicate */
    CHECK(fails_with(PRE "action p(X: actor): requires hurt(X) causes spark(X, X)\n",
                     "takes 1 argument"));
    /* the name is vocabulary: a redeclaration collides */
    CHECK(fails_with(PRE "emit hurt(actor)\n", "already declared"));

    /* and the check that made the cue's own typo loud: an effect target must
     * be state or a cue, not an undeclared name (this used to compile clean
     * and abort inside the step family — EPIC #154) */
    CHECK(fails_with(PRE "action p(X: actor): requires hurt(X) causes sprak(X)\n",
                     "is not declared state"));
    return 0;
}

int main(void)
{
    if (test_cues_story()) return 1;
    if (test_emit_engine()) return 1;
    if (test_emit_errors()) return 1;
    printf("test_emit: all passed\n");
    return 0;
}
