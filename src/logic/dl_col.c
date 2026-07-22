#include "logic/dl_col.h"
#include "logic/dl_trace.h"

#include <stdlib.h>
#include <string.h>

/* Columnar family solver (see dl_col.h). This is dl.c's tri-valued fixpoint
 * with every status lifted to a bitvector over entities and every ts_min /
 * ts_max lifted to Kleene 3-valued AND/OR on (true-mask, false-mask) pairs:
 *
 *   AND: t = ta & tb   f = fa | fb        NOT: swap t and f
 *   OR:  t = ta | tb   f = fa & fb
 *
 * Statuses are monotone (masks only gain bits), so sweeping the schema's
 * literals to fixpoint reaches the same least fixpoint as dl.c's per-literal
 * sweep — per entity, bit for bit. */

typedef struct {
    uint8_t kind;
    int32_t head;         /* literal index: atom*2 + neg */
    int32_t body_off;     /* into body[] */
    char   *name;         /* cold; dlcol_why only */
    char   *prov;         /* cold; provenance suffix (§6.3) or NULL */
} crule;

struct dlcol {
    int natoms, nents, nlits;
    int W;                /* words per row */
    uint64_t tail;        /* valid-entity mask for the last word */

    crule  *rules; int nrules, caprules;
    int32_t *body; int nbody, capbody;         /* flattened bodies (lit idx) */
    struct { int winner, loser; } *sups; int nsups, capsups;
    char  **aname;                             /* [natoms] cold; why only */

    /* Compiled at solve time (schema is tiny; rebuilt on demand). */
    bool     dirty;
    int32_t *head_off, *head_rule;             /* rules grouped by head lit */
    int32_t *beat_off, *beat_by;               /* winners grouped by loser  */

    /* Columns: [nlits][W], row r at masks + r*W. */
    uint64_t *fact;
    uint64_t *delta_t, *delta_f;               /* +Delta / -Delta */
    uint64_t *part_t,  *part_f;                /* +d / -d */

    /* Per-rule applicability rows recomputed each sweep, plus scratch. */
    uint64_t *app_t, *app_f;                   /* [nrules][W] */
    uint64_t *scratch;                         /* 8 rows */
};

#define GROW(arr, n, cap) \
    do { \
        if ((n) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 16; \
            (arr) = realloc((arr), (size_t)(cap) * sizeof *(arr)); \
        } \
    } while (0)

static int lit_idx(dl_lit l) { return (int)l.atom * 2 + (l.neg ? 1 : 0); }
static dl_lit lit_from_idx(int i) { dl_lit l = { (uint32_t)(i >> 1), (i & 1) != 0 }; return l; }

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    memcpy(d, s, n);
    return d;
}

static uint64_t *row(uint64_t *base, const dlcol *f, int lit)
{
    return base + (size_t)lit * (size_t)f->W;
}

dlcol *dlcol_new(int natoms, int nentities)
{
    dlcol *f = calloc(1, sizeof *f);
    f->natoms = natoms;
    f->nents = nentities;
    f->nlits = natoms * 2;
    f->W = nentities > 0 ? (nentities + 63) / 64 : 1;
    int r = nentities % 64;
    f->tail = (nentities > 0 && r != 0) ? (~0ull >> (64 - r)) : ~0ull;
    size_t nm = f->nlits ? (size_t)f->nlits * f->W : 1;   /* natoms==0 ok */
    f->fact = calloc(nm, sizeof *f->fact);
    f->delta_t = calloc(nm, sizeof *f->delta_t);
    f->delta_f = calloc(nm, sizeof *f->delta_f);
    f->part_t  = calloc(nm, sizeof *f->part_t);
    f->part_f  = calloc(nm, sizeof *f->part_f);
    f->scratch = calloc((size_t)8 * f->W, sizeof *f->scratch);
    f->aname = calloc((size_t)(natoms ? natoms : 1), sizeof *f->aname);
    f->dirty = true;
    return f;
}

void dlcol_free(dlcol *f)
{
    for (int r = 0; r < f->nrules; r++) {
        free(f->rules[r].name);
        free(f->rules[r].prov);
    }
    for (int a = 0; a < f->natoms; a++)
        free(f->aname[a]);
    free(f->aname);
    free(f->rules); free(f->body); free(f->sups);
    free(f->head_off); free(f->head_rule);
    free(f->beat_off); free(f->beat_by);
    free(f->fact);
    free(f->delta_t); free(f->delta_f);
    free(f->part_t); free(f->part_f);
    free(f->app_t); free(f->app_f);
    free(f->scratch);
    free(f);
}

int dlcol_add_rule(dlcol *f, const char *name, dl_rule_kind kind,
                   dl_lit head, const dl_lit *body, int nbody)
{
    GROW(f->rules, f->nrules, f->caprules);
    crule *r = &f->rules[f->nrules];
    r->name = xstrdup(name ? name : "?");
    r->prov = NULL;
    r->kind = (uint8_t)kind;
    r->head = (int32_t)lit_idx(head);
    r->body_off = f->nbody;
    for (int i = 0; i < nbody; i++) {
        GROW(f->body, f->nbody, f->capbody);
        f->body[f->nbody++] = (int32_t)lit_idx(body[i]);
    }
    f->dirty = true;
    return f->nrules++;
}

void dlcol_add_sup(dlcol *f, int winner, int loser)
{
    GROW(f->sups, f->nsups, f->capsups);
    f->sups[f->nsups].winner = winner;
    f->sups[f->nsups].loser = loser;
    f->nsups++;
    f->dirty = true;
}

void dlcol_set_prov(dlcol *f, int rule_id, const char *prov)
{
    if (rule_id < 0 || rule_id >= f->nrules)
        return;
    free(f->rules[rule_id].prov);
    f->rules[rule_id].prov = prov ? xstrdup(prov) : NULL;
}

void dlcol_set_atom_name(dlcol *f, uint32_t atom, const char *name)
{
    if ((int)atom >= f->natoms)
        return;
    free(f->aname[atom]);
    f->aname[atom] = xstrdup(name);
}

uint64_t *dlcol_fact_row(dlcol *f, dl_lit l)
{
    return row(f->fact, f, lit_idx(l));
}

void dlcol_add_fact(dlcol *f, dl_lit l, int entity)
{
    row(f->fact, f, lit_idx(l))[entity / 64] |= 1ull << (entity % 64);
}

void dlcol_clear_facts(dlcol *f)
{
    memset(f->fact, 0, (size_t)f->nlits * f->W * sizeof(uint64_t));
}

int dlcol_row_words(const dlcol *f) { return f->W; }

/* body slice of rule r: [body_off, body_end(r)) */
static int body_end_c(const dlcol *f, int r)
{
    return r + 1 < f->nrules ? f->rules[r + 1].body_off : f->nbody;
}

static void compile_indices(dlcol *f)
{
    free(f->head_off); free(f->head_rule);
    free(f->beat_off); free(f->beat_by);
    free(f->app_t); free(f->app_f);

    f->head_off  = calloc((size_t)f->nlits + 1, sizeof *f->head_off);
    f->head_rule = malloc((size_t)(f->nrules ? f->nrules : 1) * sizeof *f->head_rule);
    for (int r = 0; r < f->nrules; r++)
        f->head_off[f->rules[r].head + 1]++;
    for (int i = 0; i < f->nlits; i++)
        f->head_off[i + 1] += f->head_off[i];
    int *fill = calloc((size_t)f->nlits, sizeof *fill);
    for (int r = 0; r < f->nrules; r++) {
        int h = f->rules[r].head;
        f->head_rule[f->head_off[h] + fill[h]++] = r;
    }
    free(fill);

    f->beat_off = calloc((size_t)f->nrules + 1, sizeof *f->beat_off);
    f->beat_by  = malloc((size_t)(f->nsups ? f->nsups : 1) * sizeof *f->beat_by);
    for (int s = 0; s < f->nsups; s++)
        f->beat_off[f->sups[s].loser + 1]++;
    for (int r = 0; r < f->nrules; r++)
        f->beat_off[r + 1] += f->beat_off[r];
    int *bfill = calloc((size_t)(f->nrules ? f->nrules : 1), sizeof *bfill);
    for (int s = 0; s < f->nsups; s++) {
        int l = f->sups[s].loser;
        f->beat_by[f->beat_off[l] + bfill[l]++] = f->sups[s].winner;
    }
    free(bfill);

    f->app_t = calloc((size_t)(f->nrules ? f->nrules : 1) * f->W, sizeof *f->app_t);
    f->app_f = calloc((size_t)(f->nrules ? f->nrules : 1) * f->W, sizeof *f->app_f);
    f->dirty = false;
}

/* ---- delta phase: strict rules + facts, vectorized eval_delta ---- */

static void solve_delta(dlcol *f)
{
    const int W = f->W;
    uint64_t *prove   = f->scratch + 0 * W;
    uint64_t *alldead = f->scratch + 1 * W;
    uint64_t *conj_t  = f->scratch + 2 * W;
    uint64_t *dead    = f->scratch + 3 * W;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int q = 0; q < f->nlits; q++) {
            uint64_t *dt = row(f->delta_t, f, q), *df = row(f->delta_f, f, q);
            /* skip literals decided for every entity — statuses are
             * monotone, so nothing here can change (the columnar analog of
             * the scalar sweep's decided-skip; at small W the branchless
             * recompute would otherwise dominate). */
            uint64_t undec = 0;
            for (int w = 0; w < W; w++)
                undec |= ~(dt[w] | df[w]) & ((w == W - 1) ? f->tail : ~0ull);
            if (!undec)
                continue;
            memcpy(prove, row(f->fact, f, q), (size_t)W * sizeof *prove);
            memset(alldead, 0xff, (size_t)W * sizeof *alldead);
            for (int k = f->head_off[q]; k < f->head_off[q + 1]; k++) {
                int r = f->head_rule[k];
                if (f->rules[r].kind != DL_STRICT)
                    continue;
                memset(conj_t, 0xff, (size_t)W * sizeof *conj_t);
                memset(dead, 0, (size_t)W * sizeof *dead);
                for (int i = f->rules[r].body_off; i < body_end_c(f, r); i++) {
                    const uint64_t *bt = row(f->delta_t, f, f->body[i]);
                    const uint64_t *bf = row(f->delta_f, f, f->body[i]);
                    for (int w = 0; w < W; w++) {
                        conj_t[w] &= bt[w];
                        dead[w] |= bf[w];
                    }
                }
                for (int w = 0; w < W; w++) {
                    prove[w] |= conj_t[w];
                    alldead[w] &= dead[w];
                }
            }
            uint64_t any = 0;
            for (int w = 0; w < W; w++) {
                uint64_t m = (w == W - 1) ? f->tail : ~0ull;
                uint64_t nt = (prove[w] & ~df[w] & ~dt[w]) & m;
                dt[w] |= nt;
                uint64_t nf = (alldead[w] & ~row(f->fact, f, q)[w]
                               & ~dt[w] & ~df[w]) & m;
                df[w] |= nf;
                any |= nt | nf;
            }
            if (any)
                changed = true;
        }
    }
}

/* ---- part phase: vectorized eval_part ---- */

/* Recompute every rule's applicability pair from the current part masks:
 * app_t = AND over body of +d(b), app_f = OR over body of -d(b). */
static void refresh_applicability(dlcol *f)
{
    const int W = f->W;
    for (int r = 0; r < f->nrules; r++) {
        uint64_t *at = f->app_t + (size_t)r * W, *af = f->app_f + (size_t)r * W;
        /* a rule whose applicability is decided everywhere can't change:
         * per entity, app_t and app_f are exclusive and monotone */
        uint64_t undec = 0;
        for (int w = 0; w < W; w++)
            undec |= ~(at[w] | af[w]) & ((w == W - 1) ? f->tail : ~0ull);
        if (!undec)
            continue;
        memset(at, 0xff, (size_t)W * sizeof *at);
        memset(af, 0, (size_t)W * sizeof *af);
        for (int i = f->rules[r].body_off; i < body_end_c(f, r); i++) {
            const uint64_t *pt = row(f->part_t, f, f->body[i]);
            const uint64_t *pf = row(f->part_f, f, f->body[i]);
            for (int w = 0; w < W; w++) {
                at[w] &= pt[w];
                af[w] |= pf[w];
            }
        }
    }
}

static void solve_part(dlcol *f)
{
    const int W = f->W;
    uint64_t *sup_t = f->scratch + 0 * W;   /* supported(q)            */
    uint64_t *sup_f = f->scratch + 1 * W;   /* == notsupported(q) true */
    uint64_t *aac_t = f->scratch + 2 * W;   /* all attackers countered */
    uint64_t *auc_t = f->scratch + 3 * W;   /* attacker uncountered    */
    uint64_t *bt    = f->scratch + 4 * W;   /* beaten-by-supporter t   */
    uint64_t *bf    = f->scratch + 5 * W;   /* beaten-by-supporter f   */

    bool changed = true;
    while (changed) {
        changed = false;
        refresh_applicability(f);
        for (int q = 0; q < f->nlits; q++) {
            int nq = q ^ 1;
            uint64_t *pt = row(f->part_t, f, q), *pf = row(f->part_f, f, q);
            /* decided-everywhere literals can't move; skip (see solve_delta) */
            uint64_t undec = 0;
            for (int w = 0; w < W; w++)
                undec |= ~(pt[w] | pf[w]) & ((w == W - 1) ? f->tail : ~0ull);
            if (!undec)
                continue;

            /* supported(q): OR of applicable over strict/defeasible rules
             * for q; empty set -> (false, true). sup_f doubles as
             * notsupported(q)'s true-mask (AND of NOT applicable). */
            memset(sup_t, 0, (size_t)W * sizeof *sup_t);
            memset(sup_f, 0xff, (size_t)W * sizeof *sup_f);
            for (int k = f->head_off[q]; k < f->head_off[q + 1]; k++) {
                int r = f->head_rule[k];
                if (f->rules[r].kind == DL_DEFEATER)
                    continue;
                const uint64_t *at = f->app_t + (size_t)r * W;
                const uint64_t *af = f->app_f + (size_t)r * W;
                for (int w = 0; w < W; w++) {
                    sup_t[w] |= at[w];
                    sup_f[w] &= af[w];
                }
            }

            /* Attackers: every rule for ~q (defeaters included).
             *   countered(s,q) = NOT applicable(s) OR beaten(s,q)
             *   aac = AND over s of countered      (empty -> true)
             *   auc = OR over s of (applicable(s) AND NOT beaten(s,q))
             *                                      (empty -> false)   */
            memset(aac_t, 0xff, (size_t)W * sizeof *aac_t);
            memset(auc_t, 0, (size_t)W * sizeof *auc_t);
            for (int k = f->head_off[nq]; k < f->head_off[nq + 1]; k++) {
                int s = f->head_rule[k];
                /* beaten(s,q): OR of applicable over the strict/defeasible
                 * rules for q that beat s; empty -> (false, true). */
                memset(bt, 0, (size_t)W * sizeof *bt);
                memset(bf, 0xff, (size_t)W * sizeof *bf);
                for (int b = f->beat_off[s]; b < f->beat_off[s + 1]; b++) {
                    int t = f->beat_by[b];
                    if (f->rules[t].kind == DL_DEFEATER || f->rules[t].head != q)
                        continue;
                    const uint64_t *at = f->app_t + (size_t)t * W;
                    const uint64_t *af = f->app_f + (size_t)t * W;
                    for (int w = 0; w < W; w++) {
                        bt[w] |= at[w];
                        bf[w] &= af[w];
                    }
                }
                const uint64_t *at = f->app_t + (size_t)s * W;
                const uint64_t *af = f->app_f + (size_t)s * W;
                for (int w = 0; w < W; w++) {
                    aac_t[w] &= af[w] | bt[w];
                    auc_t[w] |= at[w] & bf[w];
                }
            }

            /* +d q = +Delta q  OR  (-Delta ~q AND supported AND aac)
             * -d q = -Delta q AND (+Delta ~q OR notsupported OR auc)
             * Merge monotonically, proved first, tail-masked. */
            const uint64_t *dtq = row(f->delta_t, f, q);
            const uint64_t *dfq = row(f->delta_f, f, q);
            const uint64_t *dtn = row(f->delta_t, f, nq);
            const uint64_t *dfn = row(f->delta_f, f, nq);
            uint64_t any = 0;
            for (int w = 0; w < W; w++) {
                uint64_t m = (w == W - 1) ? f->tail : ~0ull;
                uint64_t pos = dtq[w] | (dfn[w] & sup_t[w] & aac_t[w]);
                uint64_t nt = (pos & ~pf[w] & ~pt[w]) & m;
                pt[w] |= nt;
                uint64_t neg = dfq[w] & (dtn[w] | sup_f[w] | auc_t[w]);
                uint64_t nf = (neg & ~pt[w] & ~pf[w]) & m;
                pf[w] |= nf;
                any |= nt | nf;
            }
            if (any)
                changed = true;
        }
    }
}

void dlcol_solve(dlcol *f)
{
    if (f->dirty)
        compile_indices(f);
    size_t bytes = (size_t)f->nlits * f->W * sizeof(uint64_t);
    memset(f->delta_t, 0, bytes);
    memset(f->delta_f, 0, bytes);
    memset(f->part_t, 0, bytes);
    memset(f->part_f, 0, bytes);
    /* app rows must not survive a solve: refresh_applicability skips rules
     * whose rows look decided, so stale rows would be trusted */
    size_t rbytes = (size_t)(f->nrules ? f->nrules : 1) * f->W * sizeof(uint64_t);
    memset(f->app_t, 0, rbytes);
    memset(f->app_f, 0, rbytes);
    solve_delta(f);
    solve_part(f);
}

static dl_verdict verdict_at(const dlcol *f, const uint64_t *t,
                             const uint64_t *fl, int entity)
{
    if (entity < 0 || entity >= f->nents)
        return DL_UNDECIDED;
    uint64_t bit = 1ull << (entity % 64);
    if (t[entity / 64] & bit)
        return DL_PROVED;
    if (fl[entity / 64] & bit)
        return DL_REFUTED;
    return DL_UNDECIDED;
}

dl_verdict dlcol_definite(const dlcol *f, dl_lit q, int entity)
{
    int qi = lit_idx(q);
    if (qi >= f->nlits)
        return DL_UNDECIDED;
    return verdict_at(f, row((uint64_t *)f->delta_t, f, qi),
                      row((uint64_t *)f->delta_f, f, qi), entity);
}

dl_verdict dlcol_defeasible(const dlcol *f, dl_lit q, int entity)
{
    int qi = lit_idx(q);
    if (qi >= f->nlits)
        return DL_UNDECIDED;
    return verdict_at(f, row((uint64_t *)f->part_t, f, qi),
                      row((uint64_t *)f->part_f, f, qi), entity);
}

const uint64_t *dlcol_proved_row(const dlcol *f, dl_lit l)
{
    int qi = lit_idx(l);
    if (qi >= f->nlits)
        return NULL;
    return row((uint64_t *)f->part_t, f, qi);
}

/* ---- why-trace: an adapter over the shared renderer (dl_trace.h) ----
 *
 * Read straight off the solved columns, one entity's slice. dl_lit here is in
 * the family's *location* space (atom = loc); the renderer only round-trips it
 * back through these accessors, never inspecting it, so that stays transparent.
 * A trace "rule handle" is a family rule id. Names carry an entity tag — that,
 * plus reading columns instead of the permuted arrays, is the only difference
 * from the scalar backing; the format itself lives once in dl_trace.c. */

static void col_print_lit(const dlcol *f, int lit, int entity, FILE *out)
{
    const char *n = f->aname[lit >> 1];
    if (n)
        fprintf(out, "%s%s[%d]", (lit & 1) ? "~" : "", n, entity);
    else
        fprintf(out, "%sa%d[%d]", (lit & 1) ? "~" : "", lit >> 1, entity);
}

/* Tri-valued applicability of rule ri for one entity — dl.c's ts_applicable
 * evaluated on this entity's bits of the part columns. */
static int col_applicable(const dlcol *f, int ri, int entity)
{
    uint64_t bit = 1ull << (entity % 64);
    int w = entity / 64, acc = 1;
    for (int i = f->rules[ri].body_off; i < body_end_c(f, ri); i++) {
        int b = f->body[i];
        if (row((uint64_t *)f->part_f, f, b)[w] & bit)
            return -1;
        if (!(row((uint64_t *)f->part_t, f, b)[w] & bit))
            acc = 0;
    }
    return acc;
}

static bool col_beats(const dlcol *f, int winner, int loser)
{
    for (int k = f->beat_off[loser]; k < f->beat_off[loser + 1]; k++)
        if (f->beat_by[k] == winner)
            return true;
    return false;
}

typedef struct { const dlcol *f; int entity; } col_trace;

static void cc_put_lit(void *ctx, dl_lit l, FILE *out)
{
    const col_trace *c = ctx;
    col_print_lit(c->f, lit_idx(l), c->entity, out);
}
static void cc_put_rule(void *ctx, int r, FILE *out)
{
    const col_trace *c = ctx;
    fprintf(out, "%s[%d]", c->f->rules[r].name, c->entity);
}
static dl_verdict cc_definite(void *ctx, dl_lit l)
{ const col_trace *c = ctx; return dlcol_definite(c->f, l, c->entity); }
static dl_verdict cc_defeasible(void *ctx, dl_lit l)
{ const col_trace *c = ctx; return dlcol_defeasible(c->f, l, c->entity); }
static bool cc_is_fact(void *ctx, dl_lit l)
{
    const col_trace *c = ctx;
    const dlcol *f = c->f;
    int e = c->entity, qi = lit_idx(l);
    return e >= 0 && e < f->nents &&
        ((row((uint64_t *)f->fact, f, qi)[e / 64] >> (e % 64)) & 1);
}
static bool cc_solved(void *ctx)
{ const col_trace *c = ctx; return !(c->f->dirty || !c->f->head_off); }
static int cc_nhead(void *ctx, dl_lit l)
{
    const dlcol *f = ((const col_trace *)ctx)->f;
    int qi = lit_idx(l);
    return f->head_off ? f->head_off[qi + 1] - f->head_off[qi] : 0;
}
static int cc_head_at(void *ctx, dl_lit l, int i)
{
    const dlcol *f = ((const col_trace *)ctx)->f;
    return f->head_rule[f->head_off[lit_idx(l)] + i];
}
static dl_rule_kind cc_rule_kind(void *ctx, int r)
{ return ((const col_trace *)ctx)->f->rules[r].kind; }
static int cc_nbody(void *ctx, int r)
{
    const dlcol *f = ((const col_trace *)ctx)->f;
    return body_end_c(f, r) - f->rules[r].body_off;
}
static dl_lit cc_body_at(void *ctx, int r, int i)
{
    const dlcol *f = ((const col_trace *)ctx)->f;
    return lit_from_idx(f->body[f->rules[r].body_off + i]);
}
static int cc_applicable(void *ctx, int r)
{ const col_trace *c = ctx; return col_applicable(c->f, r, c->entity); }
static bool cc_beats(void *ctx, int w, int l)
{ return col_beats(((const col_trace *)ctx)->f, w, l); }
static const char *cc_rule_prov(void *ctx, int r)
{ return ((const col_trace *)ctx)->f->rules[r].prov; }

static const dl_trace_vtbl col_vtbl = {
    .put_lit = cc_put_lit, .put_rule = cc_put_rule,
    .definite = cc_definite, .defeasible = cc_defeasible,
    .is_fact = cc_is_fact, .solved = cc_solved,
    .nhead = cc_nhead, .head_at = cc_head_at,
    .rule_kind = cc_rule_kind, .nbody = cc_nbody, .body_at = cc_body_at,
    .applicable = cc_applicable, .beats = cc_beats,
    .rule_prov = cc_rule_prov,
};

void dlcol_why(const dlcol *f, dl_lit q, int entity, FILE *out)
{
    col_trace ctx = { f, entity };
    dl_trace_render(&col_vtbl, &ctx, q, out);
}
