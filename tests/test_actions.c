/* tests/test_actions.c — applicable actions (§6.3).
 *
 * The question every client asks about every choice it presents: which of
 * these can be taken right now, and for the ones that cannot, why? Before
 * this, a client answered it by writing a judgment beside each action that
 * mirrored the action's own `requires` — a second copy of the rule, free to
 * drift from the first, and the reason `cellar_pure.story` needed a `cmd`
 * vocabulary at all.
 *
 * What is pinned here is that the engine's answer IS the action's guards, not
 * a paraphrase: block an action by making a guard fail and the blocker it
 * names is that guard, spelled the way the author wrote it, so `world_why` on
 * it prints the argument that refused it.
 */

#include "core/intern.h"
#include "lang/story.h"
#include "logic/dl.h"
#include "state/world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = '\0';
    fclose(f);
    return s;
}

static bool names(intern *sy, const dl_lit *ls, int n, const char *want)
{
    for (int i = 0; i < n; i++)
        if (strcmp(intern_name(sy, ls[i].atom), want) == 0) return true;
    return false;
}

int main(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/cellar_play.story", STORY_DIR);
    char *src = slurp(path);
    CHECK(src != NULL);

    intern *sy = intern_new();
    story_diag di[32];
    story_diags dg = { di, 32, 0, 0 };
    world *w = story_compile(src, "cellar_play.story", sy, &dg);
    CHECK(w != NULL);

    /* ---- the ground action vocabulary, from the world itself ------------- */
    uint32_t acts[128];
    int nacts = world_actions(w, acts, 128);
    CHECK(nacts > 0 && nacts <= 128);
    /* the count is the FULL answer, so a short buffer is a grow-and-retry */
    CHECK(world_actions(w, acts, 2) == nacts);
    nacts = world_actions(w, acts, 128);

    bool saw_hero = false, saw_guard = false, saw_ramif = false;
    for (int i = 0; i < nacts; i++) {
        const char *n = intern_name(sy, acts[i]);
        if (strcmp(n, "force_door(hero)") == 0)  saw_hero = true;
        if (strcmp(n, "force_door(guard)") == 0) saw_guard = true;
        if (strcmp(n, "lying_in") == 0)          saw_ramif = true;
    }
    CHECK(saw_hero && saw_guard);      /* a ground instance per binding */
    CHECK(!saw_ramif);                 /* a ramification is not offered */

    /* ---- nobody can force a locked door ---------------------------------- */
    uint32_t fh = intern_id(sy, "force_door(hero)"),
             fg = intern_id(sy, "force_door(guard)"),
             gh = intern_id(sy, "go_hall(hero)");
    CHECK(!world_action_applies(w, fh));
    CHECK(world_action_status_of(w, fh) == WORLD_ACTION_BLOCKED);
    CHECK(world_action_applies(w, gh));      /* the hero is in the cellar */

    /* an atom no step rule mentions is UNKNOWN, not "blocked" — the
     * difference between a refusal and a typo */
    CHECK(world_action_status_of(w, intern_id(sy, "pick_lock(hero)"))
          == WORLD_ACTION_UNKNOWN);

    /* ---- the blocker IS the guard the author wrote ------------------------ */
    dl_lit bl[8];
    int nb = world_action_blockers(w, fh, bl, 8);
    CHECK(nb > 0);
    CHECK(names(sy, bl, nb, "can_force_door(hero)"));

    /* ...and it is an ordinary literal, so the trace explains the refusal */
    {
        char buf[4096] = { 0 };
        FILE *m = fmemopen(buf, sizeof buf, "w");
        CHECK(m != NULL);
        world_why(w, bl[0], m);
        fclose(m);
        CHECK(strstr(buf, "can_force_door(hero)") != NULL);
    }

    /* ---- a blocker can be a NEGATIVE literal, and reads as one ------------ */
    uint32_t take_key = intern_id(sy, "take(hero,rusty_key,cellar)");
    CHECK(!world_action_applies(w, take_key));   /* the key is right there, in the dark */
    nb = world_action_blockers(w, take_key, bl, 8);
    CHECK(nb == 1 && names(sy, bl, nb, "in_dark(hero)") && bl[0].neg);

    /* ---- and it moves with the world, because it IS the world ------------- */
    char err[128];
    uint32_t go = intern_id(sy, "go_hall(hero)"), unlock = intern_id(sy, "unlock(hero)");
    CHECK(!world_action_applies(w, unlock));          /* wrong room, no key */
    CHECK(world_step(w, &go, 1, err, sizeof err) == 0);

    uint32_t take_torch = intern_id(sy, "take(hero,torch,hall)");
    CHECK(world_action_applies(w, take_torch));
    CHECK(world_step(w, &take_torch, 1, err, sizeof err) == 0);
    uint32_t back = intern_id(sy, "go_cellar(hero)");
    CHECK(world_step(w, &back, 1, err, sizeof err) == 0);
    CHECK(world_action_applies(w, take_key));         /* the torch lit it */
    CHECK(world_step(w, &take_key, 1, err, sizeof err) == 0);
    CHECK(!world_action_applies(w, take_key));        /* ...and it is gone now */
    CHECK(world_step(w, &go, 1, err, sizeof err) == 0);
    CHECK(world_action_applies(w, unlock));           /* right room, key in hand */
    CHECK(world_step(w, &unlock, 1, err, sizeof err) == 0);

    /* the jammed door: the guard can shoulder it, the poisoned hero cannot,
     * and the engine says so without a judgment written per action */
    CHECK(world_action_applies(w, fg));
    CHECK(!world_action_applies(w, fh));
    nb = world_action_blockers(w, fh, bl, 8);
    CHECK(nb == 1 && names(sy, bl, nb, "can_force_door(hero)"));
    CHECK(world_action_blockers(w, fg, bl, 8) == 0);   /* nothing blocks it */

    world_free(w);
    intern_free(sy);
    free(src);
    printf("test_actions: all passed\n");
    return 0;
}
