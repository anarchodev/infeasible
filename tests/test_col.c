/* Differential golden test for the columnar family solver (DESIGN.md 5.8).
 *
 * One schema exercising every semantic path — strict chains, strict-wins,
 * team defeat, unresolved conflict, defeaters, negative body literals,
 * empty-body rules, attacks on facts — instantiated over N entities with
 * deterministic pseudorandom base-fact bits, closed-world. The same family
 * is grounded per entity into the scalar solver; every literal of every
 * entity must get the same definite and defeasible verdict from both.
 * N is not a multiple of 64 so tail-word masking is exercised. */

#include "logic/dl.h"
#include "logic/dl_col.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

enum {
    B0, B1, B2, B3,          /* base fluents, closed-world random bits */
    S1, S2,                  /* strict chain off B0; S2 attacked defeasibly */
    Q,                       /* team defeat: two supporters, two attackers */
    C,                       /* unresolved conflict */
    W_,                      /* defeater blocks */
    Z,                       /* derived chain with a negative body literal */
    G,                       /* empty-body rule vs attacker */
    NATOMS
};
enum { NENTS = 133 };        /* deliberately not a multiple of 64 */

static uint32_t rng_state = 0xC0FFEEu;
static uint32_t xrand(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state; }

/* Add the schema's rules through generic callbacks so the columnar family
 * and each scalar grounding are built from the same single description. */
typedef struct {
    void *ctx;
    int (*rule)(void *ctx, const char *name, dl_rule_kind kind,
                dl_lit head, const dl_lit *body, int nbody);
    void (*sup)(void *ctx, int winner, int loser);
    dl_lit (*lit)(void *ctx, int atom, bool neg);   /* schema atom -> dl_lit */
} builder;

static dl_lit L(builder *b, int atom) { return b->lit(b->ctx, atom, false); }
static dl_lit NL(builder *b, int atom) { return b->lit(b->ctx, atom, true); }

static void build_schema(builder *b)
{
    dl_lit body[2];

    body[0] = L(b, B0);
    b->rule(b->ctx, "s1_of_b0", DL_STRICT, L(b, S1), body, 1);
    body[0] = L(b, S1);
    b->rule(b->ctx, "s2_of_s1", DL_STRICT, L(b, S2), body, 1);
    body[0] = L(b, B1);
    b->rule(b->ctx, "attack_s2", DL_DEFEASIBLE, NL(b, S2), body, 1);

    body[0] = L(b, B0);
    int f1 = b->rule(b->ctx, "q_f1", DL_DEFEASIBLE, L(b, Q), body, 1);
    body[0] = L(b, B1);
    int f2 = b->rule(b->ctx, "q_f2", DL_DEFEASIBLE, L(b, Q), body, 1);
    body[0] = L(b, B2);
    int a1 = b->rule(b->ctx, "q_a1", DL_DEFEASIBLE, NL(b, Q), body, 1);
    body[0] = L(b, B3);
    int a2 = b->rule(b->ctx, "q_a2", DL_DEFEASIBLE, NL(b, Q), body, 1);
    b->sup(b->ctx, f1, a1);   /* team defeat: no single rule beats both */
    b->sup(b->ctx, f2, a2);

    body[0] = L(b, B0);
    b->rule(b->ctx, "c_for", DL_DEFEASIBLE, L(b, C), body, 1);
    body[0] = L(b, B1);
    b->rule(b->ctx, "c_against", DL_DEFEASIBLE, NL(b, C), body, 1);

    body[0] = L(b, B2);
    b->rule(b->ctx, "w_for", DL_DEFEASIBLE, L(b, W_), body, 1);
    body[0] = L(b, B3);
    b->rule(b->ctx, "w_block", DL_DEFEATER, NL(b, W_), body, 1);

    body[0] = L(b, Q);
    body[1] = NL(b, C);
    b->rule(b->ctx, "z_of_q", DL_DEFEASIBLE, L(b, Z), body, 2);

    b->rule(b->ctx, "g_default", DL_DEFEASIBLE, L(b, G), NULL, 0);
    body[0] = L(b, B0);
    b->rule(b->ctx, "g_attack", DL_DEFEASIBLE, NL(b, G), body, 1);

    body[0] = L(b, B1);
    b->rule(b->ctx, "b0_block", DL_DEFEATER, NL(b, B0), body, 1);
}

/* columnar builder callbacks */
static int col_rule(void *ctx, const char *name, dl_rule_kind kind,
                    dl_lit head, const dl_lit *body, int nbody)
{
    (void)name;
    return dlcol_add_rule(ctx, kind, head, body, nbody);
}
static void col_sup(void *ctx, int w, int l) { dlcol_add_sup(ctx, w, l); }
static dl_lit col_lit(void *ctx, int atom, bool neg)
{
    (void)ctx;
    dl_lit l = { (uint32_t)atom, neg };
    return l;
}

/* scalar builder callbacks: atoms become per-entity interned names */
typedef struct {
    dl_theory *t;
    intern *sy;
    int entity;
} ground_ctx;

static dl_lit ground_lit(void *ctx, int atom, bool neg)
{
    ground_ctx *g = ctx;
    char buf[32];
    snprintf(buf, sizeof buf, "a%d_%d", atom, g->entity);
    dl_lit l = { intern_id(g->sy, buf), neg };
    return l;
}
static int ground_rule(void *ctx, const char *name, dl_rule_kind kind,
                       dl_lit head, const dl_lit *body, int nbody)
{
    ground_ctx *g = ctx;
    char buf[48];
    snprintf(buf, sizeof buf, "%s_%d", name, g->entity);
    return dl_add_rule(g->t, buf, kind, head, body, nbody);
}
static void ground_sup(void *ctx, int w, int l)
{
    dl_add_sup(((ground_ctx *)ctx)->t, w, l);
}

static int run_round(const uint8_t bits[NENTS][4])
{
    /* columnar side: one schema, fact columns from the bits */
    dlcol *fam = dlcol_new(NATOMS, NENTS);
    builder cb = { fam, col_rule, col_sup, col_lit };
    build_schema(&cb);
    for (int e = 0; e < NENTS; e++)
        for (int a = 0; a < 4; a++) {
            dl_lit l = { (uint32_t)a, !bits[e][a] };   /* closed world */
            dlcol_add_fact(fam, l, e);
        }
    dlcol_solve(fam);

    /* scalar side: the same family grounded entity by entity */
    intern *sy = intern_new();
    dl_theory *t = dl_theory_new(sy);
    ground_ctx g = { t, sy, 0 };
    builder gb = { &g, ground_rule, ground_sup, ground_lit };
    for (int e = 0; e < NENTS; e++) {
        g.entity = e;
        build_schema(&gb);
        for (int a = 0; a < 4; a++)
            dl_add_fact(t, ground_lit(&g, a, !bits[e][a]));
    }
    dl_result *res = dl_solve(t);

    int bad = 0;
    for (int e = 0; e < NENTS && bad < 5; e++) {
        g.entity = e;
        for (int a = 0; a < NATOMS; a++)
            for (int neg = 0; neg < 2; neg++) {
                dl_lit sq = ground_lit(&g, a, neg);
                dl_lit cq = { (uint32_t)a, neg != 0 };
                if (dl_definite(res, sq) != dlcol_definite(fam, cq, e) ||
                    dl_defeasible(res, sq) != dlcol_defeasible(fam, cq, e)) {
                    fprintf(stderr,
                            "mismatch entity %d atom %d neg %d: "
                            "scalar D=%d d=%d  columnar D=%d d=%d\n",
                            e, a, neg,
                            dl_definite(res, sq), dl_defeasible(res, sq),
                            dlcol_definite(fam, cq, e),
                            dlcol_defeasible(fam, cq, e));
                    bad++;
                }
            }
    }

    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    dlcol_free(fam);
    return bad;
}

int main(void)
{
    uint8_t bits[NENTS][4];

    /* Round 1: random base-fact bits. */
    for (int e = 0; e < NENTS; e++)
        for (int a = 0; a < 4; a++)
            bits[e][a] = (uint8_t)(xrand() & 1);
    /* Pin a few corner entities regardless of the stream: */
    for (int a = 0; a < 4; a++) {
        bits[0][a] = 1;              /* everything true            */
        bits[1][a] = 0;              /* everything false           */
        bits[NENTS - 1][a] = 1;      /* last entity (tail word)    */
    }
    CHECK(run_round((const uint8_t (*)[4])bits) == 0);

    /* Round 2: flipped bits — a second full solve on fresh structures,
     * covering a different verdict mix through the same schema. */
    for (int e = 0; e < NENTS; e++)
        for (int a = 0; a < 4; a++)
            bits[e][a] ^= (uint8_t)((e + a) & 1);
    CHECK(run_round((const uint8_t (*)[4])bits) == 0);

    /* Round 3: re-solve on a mutated family (no rebuild) must equal a fresh
     * scalar solve — the tick path: hosts mutate columns, then recompute. */
    {
        dlcol *fam = dlcol_new(NATOMS, NENTS);
        builder cb = { fam, col_rule, col_sup, col_lit };
        build_schema(&cb);
        for (int e = 0; e < NENTS; e++)
            for (int a = 0; a < 4; a++) {
                dl_lit l = { (uint32_t)a, !bits[e][a] };
                dlcol_add_fact(fam, l, e);
            }
        dlcol_solve(fam);

        /* flip B0 for every third entity: clear both closed-world rows' bit,
         * set the other one */
        for (int e = 0; e < NENTS; e += 3) {
            bits[e][B0] ^= 1;
            uint64_t bit = 1ull << (e % 64);
            dl_lit pos = { B0, false }, neg = { B0, true };
            dlcol_fact_row(fam, pos)[e / 64] &= ~bit;
            dlcol_fact_row(fam, neg)[e / 64] &= ~bit;
            dlcol_add_fact(fam, bits[e][B0] ? pos : neg, e);
        }
        dlcol_solve(fam);

        intern *sy = intern_new();
        dl_theory *t = dl_theory_new(sy);
        ground_ctx g = { t, sy, 0 };
        builder gb = { &g, ground_rule, ground_sup, ground_lit };
        for (int e = 0; e < NENTS; e++) {
            g.entity = e;
            build_schema(&gb);
            for (int a = 0; a < 4; a++)
                dl_add_fact(t, ground_lit(&g, a, !bits[e][a]));
        }
        dl_result *res = dl_solve(t);
        for (int e = 0; e < NENTS; e++) {
            g.entity = e;
            for (int a = 0; a < NATOMS; a++)
                for (int neg = 0; neg < 2; neg++) {
                    dl_lit sq = ground_lit(&g, a, neg);
                    dl_lit cq = { (uint32_t)a, neg != 0 };
                    CHECK(dl_definite(res, sq) == dlcol_definite(fam, cq, e));
                    CHECK(dl_defeasible(res, sq) == dlcol_defeasible(fam, cq, e));
                }
        }
        dl_result_free(res);
        dl_theory_free(t);
        intern_free(sy);
        dlcol_free(fam);
    }

    printf("test_col: all passed\n");
    return 0;
}
