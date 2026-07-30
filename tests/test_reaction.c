/* Golden test for the EPIC #117 probe (#118): examples/reaction5e.story — one
 * full 5e combat round with turn PHASES as an ordinary MV fluent, driven by a
 * scripted deterministic "player" (I4: the decision list is fixed, the seed is
 * pinned, replay is exact). Both branches of the reaction window are pinned:
 * the wizard casts Shield (the locked attack roll retroactively misses), and
 * the wizard passes (the same roll lands).
 *
 * The acceptance proof for "phase advancement came from rules" is structural:
 * the story defines no action that writes `phase`, and the phase landmarks
 * asserted below advance across EMPTY steps (no action submitted at all) —
 * the clock is ramifications, the host only ticks.
 *
 * ---------------------------------------------------------------------------
 * Probe write-up (#118 acceptance): what the decomposed-step reaction felt like
 *
 * The protocol authored cleanly ONCE the attack-in-flight was reified: the
 * step boundary is "the largest unit nothing can legally interject into", so
 * declare/react/resolve are separate steps and the intermediate state
 * (`pending`, `atk_die`, `atk_mod`) is ordinary fluents — MTG's stack, one
 * level deep. Shield's retroactivity then costs nothing: the locked die is
 * re-judged against the LIVE `ac` value each tick, so "the hit becomes a
 * miss" is just a judgment flipping, with the why-trace showing the Shield
 * layer doing the work. Phase advancement as ramifications was entirely
 * unproblematic — one writer per step, `causal beats inertia` does the rest.
 * The window-that-never-opens case (goblin has no reaction) auto-advances by
 * RULE off closed-world base facts (`~has_shield`), needing no log entry.
 *
 * What pushed back — filed as gap issues off #118:
 *   - #129: rolls are per-tick, so a multi-step protocol MUST snapshot dice
 *     and modifiers into fluents at declaration (`atk_die := roll(20)`); #82
 *     named values re-draw across ticks and cannot carry the locked roll.
 *   - #130: an expression guard cannot START with a numeric fluent read —
 *     the parser needs a leading value/roll/int/paren, so the natural
 *     `atk_die(A) + atk_mod(A) >= ac(T)` requires parentheses.
 *   - #131: window entry must key on BASE facts (`has_shield`): opening the
 *     react phase only when the attack would actually hit needs the hit
 *     judgment read NEXT-state at declaration (primed judgment — the §5.8
 *     case), and closing an entered-but-empty window needs NAF over the
 *     derived `can_react`.
 *   - #132: the expression guard renders as an opaque `eg…` marker in the
 *     why-trace — no comparison, no operand values, no view of the value
 *     layers behind it (the Shield layer must be queried via its marker
 *     judgment, as below).
 * ------------------------------------------------------------------------- */

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

/* The pinned seed: found by `./test_reaction find` (a linear scan re-run if
 * the roll-stream keying ever changes). Under it, in tick order:
 *   t1  grunk's d20 hits vera's base AC 14 but misses Shielded 19,
 *   t5  vera's d20 is a natural 20 (crit), and
 *   t6  the doubled dagger dice fell the 7-hp goblin outright. */
#define SEED 202u

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

/* Capture world_why(q) into a fixed buffer (truncating; asserts are substring
 * checks, the head of the trace is what they pin). */
static void why_into(world *w, dl_lit q, char *dst, size_t cap)
{
    char *buf = NULL; size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    world_why(w, q, m);
    fclose(m);
    snprintf(dst, cap, "%s", buf ? buf : "");
    free(buf);
}

/* Everything one scripted round observes; play() fills it, the golden tests
 * assert on it. Seed-independent structure is not asserted inside play() —
 * the find mode reuses it over arbitrary seeds. */
typedef struct {
    int  compile_ok;
    int  steps_ok;           /* protocol steps that committed (max 7) */
    int  phases_ok;          /* every phase landmark matched, incl. via empty steps */
    long die_g, mod_g;       /* grunk's locked roll, turn 1 */
    long die_v, mod_v;       /* vera's locked roll, turn 2 */
    int  hit_at_window;      /* incoming_hit(grunk,vera) at the react tick */
    int  window_open;        /* can_react(vera) at the react tick */
    int  hit_after_react;    /* same judgment, after the reaction step */
    int  crit_v;             /* crit(vera) at the turn-2 resolve tick */
    long hp_vera_final, hp_grunk_final, bless_final;
    int  grunk_alive_final, grunk_down_final;
    int  vera_blessed_mid, vera_blessed_final, shield_dropped;
    char why_window[4096];   /* can_react(vera), at the open window */
    char why_blocked[4096];  /* incoming_hit(grunk,vera), post-reaction */
    char why_layer[4096];    /* ac_shield(vera) — the layer's marker judgment */
} obs;

static int phase_is(world *w, intern *sy, const char *v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "phase=%s", v);
    return world_query(w, dl_pos(intern_id(sy, buf))) == DL_PROVED;
}

/* Run the fixed two-turn protocol. `cast` picks the scripted player's answer
 * at the reaction window: 1 = cast_shield, 0 = pass. Tick structure (and so
 * the keyed roll draws) is identical either way:
 *   t1 strike(grunk,vera)   t2 cast_shield(vera)|pass(vera)
 *   t3 (empty: resolve)     t4 (empty: cleanup)
 *   t5 strike(vera,grunk)   t6 (empty: resolve)   t7 (empty: cleanup) */
static void play(uint64_t seed, int cast, obs *o)
{
    memset(o, 0, sizeof *o);

    char *src = read_file(STORY_DIR "/reaction5e.story");
    if (!src) return;
    intern *sy = intern_new();
    story_diag di[16];
    story_diags d = { di, 16, 0, 0 };
    world *w = story_compile(src, "reaction5e.story", sy, &d);
    if (!w) {
        fprintf(stderr, "compile failed: %s\n", d.count ? di[0].msg : "?");
        intern_free(sy); free(src);
        return;
    }
    if (d.nerrors == 0) o->compile_ok = 1;
    world_set_seed(w, seed);

    uint32_t strike_gv = intern_id(sy, "strike(grunk,vera)"),
             strike_vg = intern_id(sy, "strike(vera,grunk)"),
             shield_v  = intern_id(sy, "cast_shield(vera)"),
             pass_v    = intern_id(sy, "pass(vera)"),
             inc_gv    = intern_id(sy, "incoming_hit(grunk,vera)"),
             canr_v    = intern_id(sy, "can_react(vera)");
    char err[256];
    int ph = 1;

    /* t1 — grunk declares; the die and modifier lock, the window opens */
    if (world_step(w, &strike_gv, 1, err, sizeof err) != 0) goto done;
    o->steps_ok = 1;
    ph &= phase_is(w, sy, "react");
    o->die_g = world_get_num(w, intern_id(sy, "atk_die(grunk)"));
    o->mod_g = world_get_num(w, intern_id(sy, "atk_mod(grunk)"));
    o->hit_at_window = world_query(w, dl_pos(inc_gv)) == DL_PROVED;
    o->window_open   = world_query(w, dl_pos(canr_v)) == DL_PROVED;
    why_into(w, dl_pos(canr_v), o->why_window, sizeof o->why_window);

    /* t2 — the scripted player answers the window */
    if (world_step(w, cast ? &shield_v : &pass_v, 1, err, sizeof err) != 0) goto done;
    o->steps_ok = 2;
    ph &= phase_is(w, sy, "resolve");
    o->hit_after_react = world_query(w, dl_pos(inc_gv)) == DL_PROVED;
    why_into(w, dl_pos(inc_gv), o->why_blocked, sizeof o->why_blocked);
    why_into(w, dl_pos(intern_id(sy, "ac_shield(vera)")),
             o->why_layer, sizeof o->why_layer);

    /* t3 — empty step: the attack resolves (or fizzles against Shield) */
    if (world_step(w, NULL, 0, err, sizeof err) != 0) goto done;
    o->steps_ok = 3;
    ph &= phase_is(w, sy, "cleanup");

    /* t4 — empty step: cleanup ticks durations, the clock returns */
    if (world_step(w, NULL, 0, err, sizeof err) != 0) goto done;
    o->steps_ok = 4;
    ph &= phase_is(w, sy, "declare");
    o->vera_blessed_mid = world_get(w, intern_id(sy, "blessed(vera)"));
    o->shield_dropped   = !world_get(w, intern_id(sy, "shielded(vera)"));

    /* t5 — vera declares; grunk has no reaction, so the window is skipped by
     * rule (phase lands straight on resolve, nothing logged) */
    if (world_step(w, &strike_vg, 1, err, sizeof err) != 0) goto done;
    o->steps_ok = 5;
    ph &= phase_is(w, sy, "resolve");
    o->die_v  = world_get_num(w, intern_id(sy, "atk_die(vera)"));
    o->mod_v  = world_get_num(w, intern_id(sy, "atk_mod(vera)"));
    o->crit_v = world_query(w, dl_pos(intern_id(sy, "crit(vera)"))) == DL_PROVED;

    /* t6 — empty step: the crit lands; death cascades in the same step */
    if (world_step(w, NULL, 0, err, sizeof err) != 0) goto done;
    o->steps_ok = 6;
    ph &= phase_is(w, sy, "cleanup");

    /* t7 — empty step: Bless runs out */
    if (world_step(w, NULL, 0, err, sizeof err) != 0) goto done;
    o->steps_ok = 7;
    ph &= phase_is(w, sy, "declare");

    o->phases_ok         = ph;
    o->hp_vera_final     = world_get_num(w, intern_id(sy, "hp(vera)"));
    o->hp_grunk_final    = world_get_num(w, intern_id(sy, "hp(grunk)"));
    o->bless_final       = world_get_num(w, intern_id(sy, "bless_left(vera)"));
    o->grunk_alive_final = world_get(w, intern_id(sy, "alive(grunk)"));
    o->grunk_down_final  = world_query(w, dl_pos(intern_id(sy, "down(grunk)"))) == DL_PROVED;
    o->vera_blessed_final = world_get(w, intern_id(sy, "blessed(vera)"));

done:
    world_free(w);
    intern_free(sy);
    free(src);
}

/* A seed is golden when the round exercises every mechanic at once: the
 * turn-1 roll hits base AC but misses through Shield, the turn-2 roll is a
 * natural 20, and the doubled dice drop the goblin. */
static int seed_is_golden(const obs *o)
{
    return o->steps_ok == 7 && o->phases_ok &&
           o->hit_at_window && o->window_open && !o->hit_after_react &&
           o->die_v == 20 && !o->grunk_alive_final;
}

/* --- the Shield branch: the locked hit retroactively misses ---------------- */
static int test_shield_branch(void)
{
    obs o;
    play(SEED, 1, &o);
    CHECK(o.compile_ok);
    CHECK(o.steps_ok == 7);
    CHECK(o.phases_ok);          /* incl. advancement across EMPTY steps */

    /* turn 1: the locked d20 (11+4=15) hits AC 14 — the window opens */
    CHECK(o.die_g == 11 && o.mod_g == 4);
    CHECK(o.hit_at_window);
    CHECK(o.window_open);
    /* Shield layers +5 while the die stays locked: 15 < 19 — a miss now */
    CHECK(!o.hit_after_react);
    CHECK(o.hp_vera_final == 15);          /* not a scratch, either branch turn 2 */

    /* the why at the window shows the phase guard doing the gating… */
    CHECK(strstr(o.why_window, "can_react(vera)") != NULL);
    CHECK(strstr(o.why_window, "phase=react") != NULL);
    /* …and the post-reaction traces show the hit REFUTED with the Shield AC
     * layer's marker judgment applicable (the +5 doing the work; the expr
     * guard itself renders as an opaque `eg…` marker — #132) */
    CHECK(strstr(o.why_blocked, "incoming_hit(grunk,vera)") != NULL);
    CHECK(strstr(o.why_blocked, "REFUTED") != NULL);
    CHECK(strstr(o.why_layer, "shielded(vera)") != NULL);
    CHECK(strstr(o.why_layer, "applicable") != NULL);

    /* cleanup dropped Shield, ticked Bless (2 -> 1, still on) */
    CHECK(o.shield_dropped);
    CHECK(o.vera_blessed_mid);

    /* turn 2: natural 20, Blessed modifier 5+d4, tested twice — AC and crit */
    CHECK(o.die_v == 20 && o.mod_v == 9);
    CHECK(o.crit_v);
    /* doubled dagger dice + 3 felled the 7-hp goblin; death cascaded in-step */
    CHECK(o.hp_grunk_final == 0);
    CHECK(o.grunk_down_final);
    CHECK(!o.grunk_alive_final);

    /* the duration expired: bless_left 1 -> 0 and the mark cleared, by rule */
    CHECK(o.bless_final == 0);
    CHECK(!o.vera_blessed_final);
    return 0;
}

/* --- the pass branch: same seed, the declined window is a LOGGED choice ---- */
static int test_pass_branch(void)
{
    obs o;
    play(SEED, 0, &o);
    CHECK(o.compile_ok);
    CHECK(o.steps_ok == 7);
    CHECK(o.phases_ok);

    /* the identical locked roll now lands: 1d4+2 off vera's 15 */
    CHECK(o.die_g == 11);
    CHECK(o.hit_at_window && o.window_open);
    CHECK(o.hit_after_react);              /* no Shield — still a hit */
    CHECK(o.hp_vera_final == 12);

    /* turn 2 is byte-identical to the Shield branch (same tick keys) */
    CHECK(o.die_v == 20 && o.crit_v);
    CHECK(o.hp_grunk_final == 0 && !o.grunk_alive_final);
    CHECK(o.bless_final == 0 && !o.vera_blessed_final);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "find") == 0) {
        for (uint64_t s = 1; s < 100000; s++) {
            obs o;
            play(s, 1, &o);
            if (!o.compile_ok) return 1;
            if (seed_is_golden(&o)) {
                printf("seed %llu: die_g=%ld mod_g=%ld die_v=%ld mod_v=%ld "
                       "hp_v=%ld hp_g=%ld bless=%ld\n",
                       (unsigned long long)s, o.die_g, o.mod_g, o.die_v,
                       o.mod_v, o.hp_vera_final, o.hp_grunk_final,
                       o.bless_final);
                printf("--- why can_react(vera) at the window ---\n%s\n"
                       "--- why incoming_hit(grunk,vera) post-shield ---\n%s\n"
                       "--- why ac_shield(vera) post-shield ---\n%s\n",
                       o.why_window, o.why_blocked, o.why_layer);
                obs p;
                play(s, 0, &p);
                printf("pass branch: hp_v=%ld hp_g=%ld\n",
                       p.hp_vera_final, p.hp_grunk_final);
                return 0;
            }
        }
        printf("no golden seed under 100000\n");
        return 1;
    }

    if (test_shield_branch()) return 1;
    if (test_pass_branch())   return 1;
    printf("test_reaction: all passed\n");
    return 0;
}
