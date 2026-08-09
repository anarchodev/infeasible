/* Golden test for the structured step log (#88): everything a step tells its
 * client past the next state itself — the changeset, and the commit receipt's
 * provenance and pipeline ends.
 *
 * The thing being pinned is that a combat-log line is DATA, not a rendered
 * string a host has to parse back apart: "12 fire damage to grik from vera's
 * fireball, 5 absorbed" is (pred=fireball, C=vera, T=grik, -12, raw -5 vs
 * applied 0). So:
 *
 *   - provenance is structured — pred + (var, entity) pairs — not only the
 *     formatted ground name `fireball[C=vera,T=grik]`;
 *   - both ends of the §5.8 pipeline are reported, so absorption and overkill
 *     are renderable rather than inferred from a value that already clamped;
 *   - a thwarted attempt is a row: an action submitted whose rule failed its
 *     guards carries what it WOULD have contributed ("Immune — 0");
 *   - the step enumerates what moved instead of making the client poll;
 *   - the ROUTED lane path builds the same receipts as N=1 — pinned
 *     differentially against it, since a receipt reflecting the last N=1 step
 *     while the fast path silently builds none is worse than no receipt. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static world *compile(const char *src, intern *sy)
{
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    world *w = story_compile(src, "t.story", sy, &dg);
    if (!w) fprintf(stderr, "  compile: %s\n", dg.count ? di[0].msg : "?");
    return w;
}

/* The binding of contribution `c`, rendered as "A=hero,T=goblin". */
static void bind_str(const world_contrib *c, intern *sy, char *buf, size_t cap)
{
    size_t off = 0;
    buf[0] = '\0';
    for (int i = 0; i < c->nbind; i++) {
        int k = snprintf(buf + off, cap - off, "%s%s=%s", off ? "," : "",
                         intern_name(sy, c->vars[i]), intern_name(sy, c->ents[i]));
        if (k > 0 && (size_t)k < cap - off) off += (size_t)k;
    }
}

/* ---- provenance, structured -------------------------------------------- */

static const char *SRC =
    "sort actor\n"
    "entity ( vera, grik : actor )\n"
    "state (\n"
    "  alive(actor)\n"
    "  hp(actor) : int in 0 .. 30\n"
    "  immune(actor)\n"
    "  burning(actor)\n"
    ")\n"
    "init ( alive(vera) alive(grik) hp(vera) = 20 hp(grik) = 5 immune(vera) )\n"
    "action fireball(C: actor, T: actor):\n"
    "  requires alive(C) & alive(T) & ~immune(T)\n"
    "  causes   hp(T) -= 12 & burning(T)\n"
    "action mend(T: actor): causes hp(T) += 3\n"
    "rule slain(X: actor): hp(X)' <= 0 & alive(X) causes ~alive(X)\n";

static int test_structured_provenance(void)
{
    intern *sy = intern_new();
    world *w = compile(SRC, sy);
    CHECK(w != NULL);

    char err[256], bind[128];
    uint32_t cast = intern_id(sy, "fireball(vera,grik)");
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);

    world_receipt rp;
    CHECK(world_num_receipt(w, intern_id(sy, "hp(grik)"), &rp));
    CHECK(rp.n == 1);
    CHECK(rp.items[0].op == WORLD_OP_SUB && rp.items[0].amount == -12);
    CHECK(!rp.items[0].defeated);

    /* the ground name still reads as it always did … */
    CHECK(strcmp(rp.items[0].rule, "fireball[C=vera,T=grik]") == 0);
    /* … and the same identity is available WITHOUT parsing that string */
    CHECK(rp.items[0].pred == intern_id(sy, "fireball"));
    bind_str(&rp.items[0], sy, bind, sizeof bind);
    CHECK(strcmp(bind, "C=vera,T=grik") == 0);

    /* both ends of the pipeline: 5 - 12 = -7 raw, retracted to 0 by the range.
     * The 7 points of overkill are the difference, and `clamped` says the
     * schema's range is what spoke. */
    CHECK(rp.base == 5 && rp.raw == -7 && rp.applied == 0);
    CHECK(rp.has_range && rp.clamped && rp.lo == 0 && rp.hi == 30);
    CHECK(world_get_num(w, intern_id(sy, "hp(grik)")) == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- the thwarted attempt ----------------------------------------------- */

static int test_defeated_contribution(void)
{
    intern *sy = intern_new();
    world *w = compile(SRC, sy);
    CHECK(w != NULL);

    char err[256], bind[128];
    uint32_t cast = intern_id(sy, "fireball(grik,vera)");   /* vera is immune */
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);   /* a legal, empty step */
    CHECK(world_get_num(w, intern_id(sy, "hp(vera)")) == 20);

    /* the attempt is on the record, with what it would have done */
    world_receipt rp;
    CHECK(world_num_receipt(w, intern_id(sy, "hp(vera)"), &rp));
    CHECK(rp.n == 1);
    CHECK(rp.items[0].defeated);
    CHECK(rp.items[0].op == WORLD_OP_SUB && rp.items[0].amount == -12);
    CHECK(rp.items[0].pred == intern_id(sy, "fireball"));
    bind_str(&rp.items[0], sy, bind, sizeof bind);
    CHECK(strcmp(bind, "C=grik,T=vera") == 0);
    /* and it contributed nothing: the pipeline never moved */
    CHECK(rp.base == 20 && rp.raw == 20 && rp.applied == 20 && !rp.clamped);

    /* a ramification that does not fire is NOT an attempt — every rule in the
     * world would qualify. `slain` failed its guard here and is absent. */
    CHECK(world_num_receipt(w, intern_id(sy, "hp(grik)"), &rp));
    CHECK(rp.n == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- the changeset ------------------------------------------------------ */

static int test_changeset(void)
{
    intern *sy = intern_new();
    world *w = compile(SRC, sy);
    CHECK(w != NULL);

    char err[256];
    uint32_t cast = intern_id(sy, "fireball(vera,grik)");
    CHECK(world_step(w, &cast, 1, err, sizeof err) == 0);

    /* what moved: grik burns, grik dies (the ramification), grik's hp drops.
     * A client renders this without knowing which atoms to ask about. */
    int nb, nn;
    const world_bool_delta *bd = world_bool_deltas(w, &nb);
    const world_num_delta  *nd = world_num_deltas(w, &nn);
    CHECK(nb == 2);
    bool burning = false, died = false;
    for (int i = 0; i < nb; i++) {
        if (bd[i].atom == intern_id(sy, "burning(grik)") && bd[i].value) burning = true;
        if (bd[i].atom == intern_id(sy, "alive(grik)") && !bd[i].value) died = true;
    }
    CHECK(burning && died);
    CHECK(nn == 1);
    CHECK(nd[0].atom == intern_id(sy, "hp(grik)"));
    CHECK(nd[0].from == 5 && nd[0].to == 0);       /* the APPLIED move, clamped */

    /* the unchanged are absent: vera is in neither list */
    for (int i = 0; i < nb; i++)
        CHECK(bd[i].atom != intern_id(sy, "alive(vera)"));
    for (int i = 0; i < nn; i++)
        CHECK(nd[i].atom != intern_id(sy, "hp(vera)"));

    /* a step that changes nothing has an empty changeset … */
    uint32_t dud = intern_id(sy, "fireball(grik,vera)");    /* immune */
    CHECK(world_step(w, &dud, 1, err, sizeof err) == 0);
    world_bool_deltas(w, &nb);
    world_num_deltas(w, &nn);
    CHECK(nb == 0 && nn == 0);

    /* … and a REJECTED step leaves none behind (no tick, nothing moved) */
    uint32_t mend = intern_id(sy, "mend(grik)");
    CHECK(world_step(w, &mend, 1, err, sizeof err) == 0);   /* fill it again */
    world_num_deltas(w, &nn);
    CHECK(nn > 0);
    uint32_t bogus = intern_id(sy, "no_such_action");
    CHECK(world_step(w, &bogus, 1, err, sizeof err) == -1);
    world_bool_deltas(w, &nb);
    world_num_deltas(w, &nn);
    CHECK(nb == 0 && nn == 0);

    world_free(w);
    intern_free(sy);
    return 0;
}

/* ---- the routed lane path builds the same receipts ---------------------- */

/* Homogeneous: world_step routes the whole transition (booleans and numerics)
 * through the step lane family. */
static const char *SRC_LANED =
    "sort unit\n"
    "entity ( u0, u1, u2, u3 : unit )\n"
    "state ( hp(unit) : int in 0 .. 20   on_fire(unit) )\n"
    "init ( hp(u0)=10 hp(u1)=10 hp(u2)=10 hp(u3)=10  on_fire(u1) on_fire(u3) )\n"
    "action tick(X: unit):   causes hp(X) -= 1\n"
    "action heal(X: unit):   causes hp(X) := 20\n"
    "action ignite(X: unit): causes on_fire(X)\n"
    "rule burn(X: unit): on_fire(X) causes hp(X) -= 5\n";

/* Identical mechanics plus a two-variable action, which bails the step lanes —
 * the N=1 oracle the routed receipts are compared against. */
static const char *SRC_N1 =
    "sort unit\n"
    "entity ( u0, u1, u2, u3 : unit )\n"
    "state ( hp(unit) : int in 0 .. 20   on_fire(unit)  pinned(unit) )\n"
    "init ( hp(u0)=10 hp(u1)=10 hp(u2)=10 hp(u3)=10  on_fire(u1) on_fire(u3) )\n"
    "action tick(X: unit):   causes hp(X) -= 1\n"
    "action heal(X: unit):   causes hp(X) := 20\n"
    "action ignite(X: unit): causes on_fire(X)\n"
    "rule burn(X: unit): on_fire(X) causes hp(X) -= 5\n"
    "action pin(A: unit, B: unit): causes pinned(A)\n";      /* never submitted */

static const char *UNITS[] = { "u0", "u1", "u2", "u3" };

/* Compare one fluent's receipt across the two worlds. The ground NAME differs
 * by construction (a laned contribution names the authored rule, since one
 * schema rule serves every lane), so the comparison is over what a client
 * actually renders: the structured identity, the amounts, and the pipeline. */
static int same_receipt(world *L, world *N, intern *sy, const char *atom)
{
    world_receipt a, b;
    uint32_t at = intern_id(sy, atom);
    CHECK(world_num_receipt(L, at, &a));
    CHECK(world_num_receipt(N, at, &b));
    CHECK(a.base == b.base && a.raw == b.raw && a.applied == b.applied);
    CHECK(a.clamped == b.clamped && a.has_range == b.has_range);
    CHECK(a.lo == b.lo && a.hi == b.hi);
    CHECK(a.n == b.n);
    for (int i = 0; i < a.n; i++) {
        CHECK(a.items[i].op == b.items[i].op);
        CHECK(a.items[i].amount == b.items[i].amount);
        CHECK(a.items[i].pred == b.items[i].pred);
        /* the laned row binds its lane's entity; the N=1 row its instance's */
        CHECK(a.items[i].nbind == 1 && b.items[i].nbind == 1);
        CHECK(a.items[i].vars[0] == b.items[i].vars[0]);
        CHECK(a.items[i].ents[0] == b.items[i].ents[0]);
    }
    return 0;
}

static int test_routed_receipts(void)
{
    intern *sy = intern_new();
    world *L = compile(SRC_LANED, sy);
    world *N = compile(SRC_N1, sy);
    CHECK(L && N);
    CHECK(world_routes_numeric(L));      /* the point of the fixture */
    CHECK(!world_routes_numeric(N));

    /* u1 burns (a ramification delta) and is ticked (an action delta); u2 is
     * healed (a winning assign over a delta); u3 burns down to the clamp. */
    const char *SCRIPT[][2] = {
        { "tick(u1)",   NULL },
        { "heal(u2)",   "tick(u2)" },
        { "ignite(u0)", NULL },
        { "tick(u3)",   NULL },
        { "tick(u3)",   NULL },
    };
    char err[256];
    for (size_t k = 0; k < sizeof SCRIPT / sizeof SCRIPT[0]; k++) {
        uint32_t acts[2];
        int na = 0;
        for (int j = 0; j < 2; j++)
            if (SCRIPT[k][j]) acts[na++] = intern_id(sy, SCRIPT[k][j]);
        CHECK(world_step(L, acts, na, err, sizeof err) == 0);
        CHECK(world_step(N, acts, na, err, sizeof err) == 0);
        for (int u = 0; u < 4; u++) {
            char at[32];
            snprintf(at, sizeof at, "hp(%s)", UNITS[u]);
            CHECK(world_get_num(L, intern_id(sy, at)) ==
                  world_get_num(N, intern_id(sy, at)));
            if (same_receipt(L, N, sy, at)) return 1;
        }
    }

    /* and the routed changeset enumerates the same moves */
    uint32_t t0 = intern_id(sy, "heal(u0)");   /* u0 burned to the floor above;
                                                * a heal is a move both paths see */
    CHECK(world_step(L, &t0, 1, err, sizeof err) == 0);
    CHECK(world_step(N, &t0, 1, err, sizeof err) == 0);
    int ln, nn;
    const world_num_delta *ld = world_num_deltas(L, &ln);
    const world_num_delta *nd = world_num_deltas(N, &nn);
    CHECK(ln == nn && ln > 0);
    for (int i = 0; i < ln; i++)
        CHECK(ld[i].atom == nd[i].atom && ld[i].from == nd[i].from &&
              ld[i].to == nd[i].to);

    world_free(L);
    world_free(N);
    intern_free(sy);
    return 0;
}

int main(void)
{
    if (test_structured_provenance()) return 1;
    if (test_defeated_contribution()) return 1;
    if (test_changeset()) return 1;
    if (test_routed_receipts()) return 1;
    printf("test_steplog: all passed\n");
    return 0;
}
