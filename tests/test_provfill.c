/* The batched provider boundary (#229): a run of ground atoms answered in one
 * host call instead of one indirect call each.
 *
 * The whole content of the feature is that it changes NOTHING. Batching alters
 * when the host is asked, never what it answers (I4), so the pins here are
 * differential: the same world played with the fill callback and without it must
 * agree on every judgment, every step, and every verdict world_providers_check
 * compares — and the oracle itself must have teeth, which is what the lying
 * provider at the end is for.
 *
 * The second thing it pins is that batching actually HAPPENS: a run has to be
 * found in registration order (the grounder's nested loop is the run), and a
 * unary provider has to run over slot 0 while a binary one runs over slot 1.
 * A regression that quietly answered every atom per-atom would still be correct,
 * and would still be the bug this feature exists to fix. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

/* ---- the host's index (the engine owns none of this) --------------------- */

/* Six actors on a line; `near` is adjacency over their positions, `hostile` is
 * a unary host relation. Both are answered from the same array, per-atom and in
 * bulk, so a disagreement can only come from the batching itself. */
#define NACT 6

typedef struct {
    int pos[NACT];
    long atom_calls, fill_calls, fill_atoms;   /* what the boundary cost */
    int  max_run, run_slot_seen[4];            /* what the runs looked like */
    bool lie;                                  /* the oracle's test subject */
} host;

static intern *SY;
static uint32_t NEAR, HOSTILE;

static int actor_of(uint32_t ent)             /* "a3" -> 3 */
{
    const char *s = intern_name(SY, ent);
    return s && s[0] == 'a' ? atoi(s + 1) : -1;
}

static bool holds(const host *h, uint32_t pred, const uint32_t *args, int nargs)
{
    if (pred == NEAR && nargs == 2) {
        int i = actor_of(args[0]), j = actor_of(args[1]);
        if (i < 0 || j < 0 || i == j) return false;
        int d = h->pos[i] - h->pos[j];
        return (d < 0 ? -d : d) <= 1;
    }
    if (pred == HOSTILE && nargs == 1) {
        int i = actor_of(args[0]);
        return i >= 0 && i % 2 == 1;
    }
    return false;
}

static bool prov(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    host *h = ctx;
    h->atom_calls++;
    return holds(h, pred, args, nargs);
}

/* The batched form: one call for a whole run. A real host would walk its index
 * once here; this one just answers the run, which is enough to pin the contract.
 * `args[slot]` is ents[0] and carries nothing, so it is deliberately overwritten
 * rather than read — a host that trusted it would still pass, and should not. */
static void fill(void *ctx, uint32_t pred, const uint32_t *args, int nargs,
                 int slot, const uint32_t *ents, int nents, uint64_t *out)
{
    host *h = ctx;
    h->fill_calls++;
    h->fill_atoms += nents;
    if (nents > h->max_run) h->max_run = nents;
    if (slot >= 0 && slot < 4) h->run_slot_seen[slot]++;
    uint32_t a[4];
    for (int k = 0; k < nargs && k < 4; k++) a[k] = args[k];
    for (int i = 0; i < nents; i++) {
        a[slot] = ents[i];
        bool v = holds(h, pred, a, nargs);
        if (h->lie && pred == NEAR && i == 1) v = !v;   /* the oracle's bait */
        if (v) out[i / 64] |= 1ull << (i % 64);
    }
}

/* ---- the world ----------------------------------------------------------- */

static const char *SRC =
    "sort actor\n"
    "provider near(actor, actor)\n"
    "provider hostile(actor)\n"
    "state ( guard(actor)  alarmed(actor) )\n"
    "entity ( a0, a1, a2, a3, a4, a5 : actor )\n"
    "init ( guard(a0) guard(a3) )\n"
    "rule spot(X: actor, Y: actor): guard(X) & near(X, Y) => sees(X, Y)\n"
    "rule threat(X: actor): hostile(X) => dangerous(X)\n"
    "action shout(X: actor): requires guard(X)\n"
    "    causes for each Y: actor where near(X, Y): alarmed(Y)\n";

static world *build(intern *sy)
{
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(SRC, "provfill.story", sy, &dg);
    if (!w) {
        fprintf(stderr, "compile failed: %s\n", dg.count ? di[0].msg : "?");
        return NULL;
    }
    return w;
}

static void host_init(host *h)
{
    memset(h, 0, sizeof *h);
    for (int i = 0; i < NACT; i++) h->pos[i] = i;     /* a line: 0..5 */
}

/* Every judgment this world can conclude, in one string — the picture two runs
 * must agree on down to the byte. */
static void picture(world *w, intern *sy, char *buf, size_t cap)
{
    size_t o = 0;
    char name[32];
    for (int i = 0; i < NACT; i++) {
        snprintf(name, sizeof name, "dangerous(a%d)", i);
        o += (size_t)snprintf(buf + o, cap - o, "%d",
                              world_query(w, dl_pos(intern_id(sy, name))));
        for (int j = 0; j < NACT; j++) {
            snprintf(name, sizeof name, "sees(a%d,a%d)", i, j);
            o += (size_t)snprintf(buf + o, cap - o, "%d",
                                  world_query(w, dl_pos(intern_id(sy, name))));
        }
        snprintf(name, sizeof name, "alarmed(a%d)", i);
        o += (size_t)snprintf(buf + o, cap - o, "%d",
                              world_get(w, intern_id(sy, name)));
    }
}

/* A shout from a0, then the same picture again — the step path loads providers
 * too (the binder's `where near(X, Y)` is a step condition). */
static int play(world *w, intern *sy, char *buf, size_t cap)
{
    picture(w, sy, buf, cap / 2);
    uint32_t act = intern_id(sy, "shout(a0)");
    char err[128] = "";
    if (world_step(w, &act, 1, err, sizeof err) < 0) {
        fprintf(stderr, "step rejected: %s\n", err);
        return 1;
    }
    picture(w, sy, buf + strlen(buf), cap - strlen(buf));
    return 0;
}

/* ---- cases --------------------------------------------------------------- */

/* 1. the differential: fill on and fill off are the same run. */
static int case_identical(void)
{
    char per_atom[512], batched[512];
    host h1, h2;
    host_init(&h1);
    host_init(&h2);

    intern *sy1 = intern_new();
    SY = sy1;
    world *w1 = build(sy1);
    CHECK(w1);
    NEAR = intern_id(sy1, "near");
    HOSTILE = intern_id(sy1, "hostile");
    world_set_provider_fn(w1, prov, &h1);
    CHECK(play(w1, sy1, per_atom, sizeof per_atom) == 0);

    intern *sy2 = intern_new();
    SY = sy2;
    world *w2 = build(sy2);
    CHECK(w2);
    NEAR = intern_id(sy2, "near");
    HOSTILE = intern_id(sy2, "hostile");
    world_set_provider_fn(w2, prov, &h2);
    world_set_provider_fill_fn(w2, fill, &h2);
    CHECK(play(w2, sy2, batched, sizeof batched) == 0);

    CHECK(strcmp(per_atom, batched) == 0);
    CHECK(strlen(per_atom) > 0);

    /* and the boundary really was crossed in bulk: the per-atom run asked one
     * question per ground atom, the batched one asked a handful of runs */
    CHECK(h1.atom_calls > 0 && h1.fill_calls == 0);
    CHECK(h2.fill_calls > 0);
    CHECK(h2.fill_atoms > h2.fill_calls * 3);        /* runs, not singletons */
    CHECK(h2.atom_calls < h1.atom_calls / 4);        /* the calls went away */

    /* the runs are the grounder's nested loop: `near` varies its LAST argument,
     * the unary `hostile` varies its only one */
    CHECK(h2.max_run >= 4);
    CHECK(h2.run_slot_seen[0] > 0);
    CHECK(h2.run_slot_seen[1] > 0);
    CHECK(h2.run_slot_seen[2] == 0 && h2.run_slot_seen[3] == 0);

    world_free(w1); world_free(w2);
    intern_free(sy1); intern_free(sy2);
    return 0;
}

/* 2. the oracle: agreement is checked, and a liar is caught. */
static int case_oracle(void)
{
    host h;
    host_init(&h);
    intern *sy = intern_new();
    SY = sy;
    world *w = build(sy);
    CHECK(w);
    NEAR = intern_id(sy, "near");
    HOSTILE = intern_id(sy, "hostile");

    /* with no fill callback there is nothing to compare */
    world_set_provider_fn(w, prov, &h);
    bool ok = false;
    CHECK(world_providers_check(w, &ok) == 0);

    world_set_provider_fill_fn(w, fill, &h);
    int n = world_providers_check(w, &ok);
    CHECK(n > 0 && ok);

    /* every registered ground provider atom is compared, not a sample */
    CHECK(n == world_provider_atom_count(w));

    h.lie = true;
    CHECK(world_providers_check(w, &ok) == n);
    CHECK(!ok);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* 3. movement: the answers are re-read every solve, batched as they were
 * per-atom (§5.6's "providers are consulted fresh each solve"). */
static int case_fresh(void)
{
    host h;
    host_init(&h);
    intern *sy = intern_new();
    SY = sy;
    world *w = build(sy);
    CHECK(w);
    NEAR = intern_id(sy, "near");
    HOSTILE = intern_id(sy, "hostile");
    world_set_provider_fn(w, prov, &h);
    world_set_provider_fill_fn(w, fill, &h);

    uint32_t sees01 = intern_id(sy, "sees(a0,a1)");
    uint32_t sees05 = intern_id(sy, "sees(a0,a5)");
    CHECK(world_query(w, dl_pos(sees01)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(sees05)) == DL_REFUTED);

    /* a5 walks next to a0. The host's index is not state the engine watches, so
     * the re-solve comes from the ordinary base-fact edit any tick also makes —
     * providers are then re-read from scratch, batched exactly as they were
     * per-atom. */
    h.pos[5] = 1;
    world_set(w, intern_id(sy, "alarmed(a2)"), true);
    CHECK(world_query(w, dl_pos(sees05)) == DL_PROVED);
    CHECK(world_query(w, dl_pos(sees01)) == DL_PROVED);

    bool ok = false;
    CHECK(world_providers_check(w, &ok) > 0 && ok);

    world_free(w);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (case_identical()) return 1;
    if (case_oracle()) return 1;
    if (case_fresh()) return 1;
    printf("test_provfill: all passed\n");
    return 0;
}
