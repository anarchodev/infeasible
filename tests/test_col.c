/* Differential test for the columnar family solver (DESIGN.md 5.8).
 *
 * Two layers, both checking dlcol against the scalar solver as oracle:
 *
 *  1. A pinned hand-authored schema exercising every semantic path — strict
 *     chains, strict-wins, team defeat, unresolved conflict, defeaters,
 *     negative body literals, empty-body rules, attacks on facts —
 *     instantiated over N=133 entities (not a multiple of 64, so tail-word
 *     masking runs) with deterministic pseudorandom base-fact bits.
 *  2. A seeded fuzz layer (fz_*) that generates random well-formed schemas and
 *     differential-checks them across an N-sweep — including N=1 (world_step),
 *     64/128 (tail == ~0ull), and multi-word counts — covering the
 *     schema-shape paths the fixed schema leaves under one example.
 *
 * The same schema is grounded per entity into the scalar solver; every literal
 * of every entity must get the same definite and defeasible verdict from both.
 * All randomness is a single deterministic LCG stream (I4). */

#include "logic/dl.h"
#include "logic/dl_col.h"
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

/* Coverage signal for the fuzz layer: seed count is blind without evidence the
 * generator actually reaches interesting states (undecided/cyclic, both
 * verdicts at the defeasible level). Aggregated across a run, printed at the
 * end — this is the real gate on trusting the sweep, not the raw seed count. */
static long fz_verdict[3];       /* [0]=proved [1]=refuted [2]=undecided (scalar +d) */
static long fz_schemas_undec;    /* schemas with >=1 undecided defeasible literal   */

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
    return dlcol_add_rule(ctx, name, kind, head, body, nbody);
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
    snprintf(buf, sizeof buf, "a%d[%d]", atom, g->entity);
    dl_lit l = { intern_id(g->sy, buf), neg };
    return l;
}
static int ground_rule(void *ctx, const char *name, dl_rule_kind kind,
                       dl_lit head, const dl_lit *body, int nbody)
{
    ground_ctx *g = ctx;
    char buf[48];
    snprintf(buf, sizeof buf, "%s[%d]", name, g->entity);
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

/* The trace must be one backing too: dlcol_why reads verdicts, fact bits,
 * applicability, and superiority straight off the solved columns, and must
 * print byte-for-byte what dl_why prints for the grounded instance. Scalar
 * atoms/rules are interned as "name[entity]" — exactly dlcol_why's
 * rendering — so the outputs are directly comparable strings. */
static int test_why(void)
{
    uint8_t bits[NENTS][4];
    for (int e = 0; e < NENTS; e++)
        for (int a = 0; a < 4; a++)
            bits[e][a] = (uint8_t)(xrand() & 1);

    dlcol *fam = dlcol_new(NATOMS, NENTS);
    char buf[16];
    for (int a = 0; a < NATOMS; a++) {
        snprintf(buf, sizeof buf, "a%d", a);
        dlcol_set_atom_name(fam, (uint32_t)a, buf);
    }
    builder cb = { fam, col_rule, col_sup, col_lit };
    build_schema(&cb);
    for (int e = 0; e < NENTS; e++)
        for (int a = 0; a < 4; a++) {
            dl_lit l = { (uint32_t)a, !bits[e][a] };
            dlcol_add_fact(fam, l, e);
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

    int bad = 0;
    for (int e = 0; e < NENTS && bad < 3; e++) {
        g.entity = e;
        for (int a = 0; a < NATOMS && bad < 3; a++)
            for (int neg = 0; neg < 2; neg++) {
                char *s1 = NULL, *s2 = NULL;
                size_t n1 = 0, n2 = 0;
                FILE *m1 = open_memstream(&s1, &n1);
                dl_why(t, res, ground_lit(&g, a, neg), m1);
                fclose(m1);
                FILE *m2 = open_memstream(&s2, &n2);
                dl_lit cq = { (uint32_t)a, neg != 0 };
                dlcol_why(fam, cq, e, m2);
                fclose(m2);
                if (strcmp(s1, s2) != 0) {
                    fprintf(stderr,
                            "trace mismatch entity %d atom %d neg %d\n"
                            "--- scalar ---\n%s--- columnar ---\n%s",
                            e, a, neg, s1, s2);
                    bad++;
                }
                free(s1);
                free(s2);
            }
    }

    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    dlcol_free(fam);
    return bad;
}

/* ---- seeded random-schema fuzz ----
 *
 * The rounds above vary facts over one hand-authored schema. That exercises
 * the fact-column and merge machinery well but leaves the *schema-shape* paths
 * — compile_indices over varied rule/superiority topologies, head_off edges,
 * the exact-multiple-of-64 tail branch, multi-word W — under one fixed example.
 * This generator emits random well-formed schemas through the same `builder`
 * callbacks both solvers already consume, then differential-checks them across
 * an N-sweep that includes N=1 (the world_step path), 64/128 (tail == ~0ull),
 * and multi-word counts. Deterministic (single LCG stream) so replays are exact
 * per I4; a failing (N, schema) prints enough to reproduce. */

#define FZ_MAXATOMS 10
#define FZ_MAXRULES 20
#define FZ_MAXBODY  3
#define FZ_MAXSUPS  12

typedef struct {
    dl_rule_kind kind;
    int  head; bool hneg;
    int  nbody, batom[FZ_MAXBODY]; bool bneg[FZ_MAXBODY];
    char name[16];
} fzrule;

typedef struct {
    int natoms, nbase, nrules, nsups;
    fzrule rules[FZ_MAXRULES];
    struct { int winner, loser; } sups[FZ_MAXSUPS];
} fzschema;

static void fz_gen(fzschema *s)
{
    s->natoms = 4 + (int)(xrand() % (FZ_MAXATOMS - 3));   /* 4..FZ_MAXATOMS */
    s->nbase  = 2 + (int)(xrand() % 3);                   /* 2..4           */
    if (s->nbase > s->natoms - 1)
        s->nbase = s->natoms - 1;                         /* keep a derived atom */
    s->nrules = 3 + (int)(xrand() % (FZ_MAXRULES - 4));   /* 3..FZ_MAXRULES-2, room for a cycle */

    for (int r = 0; r < s->nrules; r++) {
        fzrule *R = &s->rules[r];
        uint32_t k = xrand() % 10;                        /* 20% strict, 60% def, 20% defeater */
        R->kind = k < 2 ? DL_STRICT : k < 8 ? DL_DEFEASIBLE : DL_DEFEATER;
        R->head = (int)(xrand() % s->natoms);
        R->hneg = (xrand() & 1) != 0;
        R->nbody = (int)(xrand() % (FZ_MAXBODY + 1));      /* 0..3, empty bodies included */
        for (int i = 0; i < R->nbody; i++) {
            R->batom[i] = (int)(xrand() % s->natoms);
            R->bneg[i]  = (xrand() & 1) != 0;              /* negative body literals */
        }
        snprintf(R->name, sizeof R->name, "r%d", r);
    }

    /* Deliberately seed a 2-cycle among derived atoms in ~1/3 of schemas:
     * x => y, y => x with no other grounding leaves both defeasible-UNDECIDED.
     * Without this the generator decides everything (REFUTED-dominant) and the
     * undecided-agreement path — where dlcol's decided-skip and scalar's must
     * still match — goes untested. */
    if ((xrand() % 3) == 0 && s->natoms - s->nbase >= 2 &&
        s->nrules + 2 <= FZ_MAXRULES) {
        int span = s->natoms - s->nbase;
        int x = s->nbase + (int)(xrand() % span);
        int y = s->nbase + (int)(xrand() % span);
        if (x == y)
            y = s->nbase + (x - s->nbase + 1) % span;
        int pair[2][2] = { { x, y }, { y, x } };  /* head, body */
        for (int i = 0; i < 2; i++) {
            fzrule *R = &s->rules[s->nrules];
            R->kind = DL_DEFEASIBLE;
            R->head = pair[i][0]; R->hneg = false;
            R->nbody = 1; R->batom[0] = pair[i][1]; R->bneg[0] = false;
            snprintf(R->name, sizeof R->name, "cyc%d", s->nrules);
            s->nrules++;
        }
    }

    /* Superiority: winner := min(a,b), loser := max(a,b) in rule-index order.
     * Edges only point from lower to higher index, so `>` is acyclic by
     * construction (the documented invariant). Both solvers consult it one
     * level deep, so they agree regardless; this just keeps theories well-formed. */
    int want = (int)(xrand() % (FZ_MAXSUPS + 1));
    s->nsups = 0;
    for (int i = 0; i < want; i++) {
        int a = (int)(xrand() % s->nrules), b = (int)(xrand() % s->nrules);
        if (a == b)
            continue;
        s->sups[s->nsups].winner = a < b ? a : b;
        s->sups[s->nsups].loser  = a < b ? b : a;
        s->nsups++;
    }
}

/* Replay a generated schema through builder callbacks. idmap translates
 * schema-local rule indices to the ids the callback actually returns — the
 * scalar side grounds per entity, so its rule ids are offset by entity. */
static void fz_replay(builder *b, const fzschema *s)
{
    int idmap[FZ_MAXRULES];
    dl_lit body[FZ_MAXBODY];
    for (int r = 0; r < s->nrules; r++) {
        const fzrule *R = &s->rules[r];
        for (int i = 0; i < R->nbody; i++)
            body[i] = b->lit(b->ctx, R->batom[i], R->bneg[i]);
        idmap[r] = b->rule(b->ctx, R->name, R->kind,
                           b->lit(b->ctx, R->head, R->hneg), body, R->nbody);
    }
    for (int i = 0; i < s->nsups; i++)
        b->sup(b->ctx, idmap[s->sups[i].winner], idmap[s->sups[i].loser]);
}

/* Solve one schema both ways over `nents` entities and compare every literal
 * of every entity. bits is [nents][nbase] closed-world base-fact bits. */
static int fz_diff(const fzschema *s, int nents, const uint8_t *bits)
{
    dlcol *fam = dlcol_new(s->natoms, nents);
    builder cb = { fam, col_rule, col_sup, col_lit };
    fz_replay(&cb, s);
    for (int e = 0; e < nents; e++)
        for (int a = 0; a < s->nbase; a++) {
            dl_lit l = { (uint32_t)a, !bits[e * s->nbase + a] };
            dlcol_add_fact(fam, l, e);
        }
    dlcol_solve(fam);

    intern *sy = intern_new();
    dl_theory *t = dl_theory_new(sy);
    ground_ctx g = { t, sy, 0 };
    builder gb = { &g, ground_rule, ground_sup, ground_lit };
    for (int e = 0; e < nents; e++) {
        g.entity = e;
        fz_replay(&gb, s);
        for (int a = 0; a < s->nbase; a++)
            dl_add_fact(t, ground_lit(&g, a, !bits[e * s->nbase + a]));
        /* Force-intern every atom so the scalar vocabulary matches the family's
         * fixed natoms: an atom in no rule and no fact would otherwise be absent
         * scalar-side (-> UNDECIDED) but present-and-ruleless in the family
         * (-> REFUTED). Same semantics, different vocabulary sizing — a harness
         * artifact, not a solver divergence. */
        for (int a = s->nbase; a < s->natoms; a++)
            ground_lit(&g, a, false);
    }
    dl_result *res = dl_solve(t);

    int bad = 0, schema_undec = 0;
    for (int e = 0; e < nents && bad < 5; e++) {
        g.entity = e;
        for (int a = 0; a < s->natoms; a++)
            for (int neg = 0; neg < 2; neg++) {
                dl_lit sq = ground_lit(&g, a, neg != 0);
                dl_lit cq = { (uint32_t)a, neg != 0 };
                dl_verdict sv = dl_defeasible(res, sq);
                fz_verdict[sv == DL_PROVED ? 0 : sv == DL_REFUTED ? 1 : 2]++;
                if (sv == DL_UNDECIDED)
                    schema_undec = 1;
                if (dl_definite(res, sq) != dlcol_definite(fam, cq, e) ||
                    sv != dlcol_defeasible(fam, cq, e)) {
                    fprintf(stderr,
                            "FUZZ mismatch N=%d entity %d atom %d neg %d: "
                            "scalar D=%d d=%d  columnar D=%d d=%d\n",
                            nents, e, a, neg,
                            dl_definite(res, sq), dl_defeasible(res, sq),
                            dlcol_definite(fam, cq, e), dlcol_defeasible(fam, cq, e));
                    bad++;
                }
            }
    }

    fz_schemas_undec += schema_undec;
    dl_result_free(res);
    dl_theory_free(t);
    intern_free(sy);
    dlcol_free(fam);
    return bad;
}

static int fuzz(void)
{
    /* Include N=1 (world_step), exact multiples of 64 (tail == ~0ull), a
     * one-bit tail (65), and multi-word counts. */
    static const int Ns[] = { 1, 7, 63, 64, 65, 128, 133, 200 };
    static uint8_t bits[200 * 4];   /* max N * max nbase */
    int schemas = 0, total_bad = 0;

    for (int ni = 0; ni < (int)(sizeof Ns / sizeof Ns[0]); ni++) {
        int N = Ns[ni];
        int K = N <= 65 ? 40 : 15;
        for (int k = 0; k < K; k++) {
            fzschema s;
            fz_gen(&s);
            for (int e = 0; e < N; e++)
                for (int a = 0; a < s.nbase; a++)
                    bits[e * s.nbase + a] = (uint8_t)(xrand() & 1);
            int bad = fz_diff(&s, N, bits);
            schemas++;
            total_bad += bad;
            if (bad)   /* stop on first failing schema for a clean report */
                return total_bad;
        }
    }
    long lits = fz_verdict[0] + fz_verdict[1] + fz_verdict[2];
    fprintf(stderr,
            "fuzz: %d schemas, %ld literals compared, 0 mismatches | "
            "coverage: +d=%ld -d=%ld undec=%ld, %ld/%d schemas had undecided lits\n",
            schemas, lits, fz_verdict[0], fz_verdict[1], fz_verdict[2],
            fz_schemas_undec, schemas);
    return total_bad;
}

int main(void)
{
    uint8_t bits[NENTS][4];

    /* INFEASIBLE_FUZZ_SEED reseeds the whole deterministic stream (pinned
     * rounds and fuzz alike), so a driver can sweep many independent seeds.
     * Unset -> the default 0xC0FFEE run, byte-stable for regression. */
    const char *seed_env = getenv("INFEASIBLE_FUZZ_SEED");
    if (seed_env && *seed_env)
        rng_state = (uint32_t)strtoul(seed_env, NULL, 0);

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

    CHECK(test_why() == 0);

    /* Fuzz: random schemas over an N-sweep, differential against scalar. This
     * is what earns dlcol its trust on the structural paths the fixed schema
     * and N=1 production never exercise. */
    CHECK(fuzz() == 0);

    printf("test_col: all passed\n");
    return 0;
}
