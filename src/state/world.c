#include "state/world.h"
#include "core/arena.h"
#include "logic/dl_col.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    dl_rule_kind kind;
    dl_lit head;
    dl_lit *body;
    int nbody;
} jrule;

typedef struct { int winner, loser; } jsup;

typedef struct {
    uint32_t num_atom;    /* ground numeric fluent this writes */
    world_numop op;
    const expr_ins *code; /* RHS bytecode (arena-copied) */
    int ncode;
} num_effect;

typedef struct {
    const char *name;
    uint32_t action;      /* INTERN_NONE = ramification */
    step_cond *body;
    int nbody;
    dl_lit *effects;      /* primed heads */
    int neffects;
    num_effect *neffs;    /* numeric effects (§5.8 write side) */
    int nneff, capneff;
} srule;

/* Per-numeric-fluent commit receipt, rebuilt each successful step. */
typedef struct {
    long base;
    world_contrib *items;
    int n, cap;
} num_receipt;

struct world {
    arena a;
    intern *syms;
    uint32_t *fluents; bool *vals; uint32_t *primed;
    int nfl, capfl;
    jrule *jrules; int njr, capjr;
    jsup *jsups; int njs, capjs;
    srule *srules; int nsr, capsr;

    /* numeric value store + comparison guards (§5.8, read side) */
    struct { uint32_t atom; long value, min, max; bool has_range; } *nums;
    int nnum, capnum;
    struct { uint32_t guard, num; world_cmp op; long threshold; } *guards;
    int ng, capg;

    /* commit receipts, one per numeric fluent (parallel to nums), valid only
     * immediately after a successful world_step (§5.8 write side). Sized lazily
     * and grown when new numeric fluents are declared after a step. */
    num_receipt *rcpt;
    int caprcpt;

    /* Cached columnar step schema (an N=1 family — DESIGN.md 5.8: single
     * derive is multiderive at N=1). The step theory's structure — judgment
     * rules, generated inertia, causal rules, superiority — is fixed
     * between steps; only the fact bits change. Rebuilt lazily when rules
     * or fluents are added; each world_step then just rewrites fact
     * columns and re-solves, paying no theory rebuild or compile. */
    dlcol *fam;
    bool fam_dirty;
    uint32_t *loc_of;             /* intern atom -> schema atom, ~0u = absent */
    uint32_t loc_cap;
    uint32_t *fl_loc, *pr_loc;    /* per fluent: schema ids of f and f' */
};

#define GROW(arr, n, cap) \
    do { \
        if ((n) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 16; \
            (arr) = realloc((arr), (size_t)(cap) * sizeof *(arr)); \
        } \
    } while (0)

world *world_new(intern *syms)
{
    world *w = calloc(1, sizeof *w);
    arena_init(&w->a);
    w->syms = syms;
    return w;
}

void world_free(world *w)
{
    if (w->fam)
        dlcol_free(w->fam);
    free(w->loc_of);
    free(w->fl_loc);
    free(w->pr_loc);
    arena_release(&w->a);
    free(w->fluents);
    free(w->vals);
    free(w->primed);
    free(w->jrules);
    free(w->jsups);
    free(w->srules);
    free(w->nums);
    free(w->guards);
    if (w->rcpt) {
        for (int i = 0; i < w->caprcpt; i++)
            free(w->rcpt[i].items);
        free(w->rcpt);
    }
    free(w);
}

static int fluent_index(const world *w, uint32_t atom)
{
    for (int i = 0; i < w->nfl; i++)
        if (w->fluents[i] == atom)
            return i;
    return -1;
}

void world_declare_fluent(world *w, uint32_t atom)
{
    if (fluent_index(w, atom) >= 0)
        return;
    int oldcap = w->capfl;
    GROW(w->fluents, w->nfl, w->capfl);
    if (w->capfl != oldcap) {
        w->vals = realloc(w->vals, (size_t)w->capfl * sizeof *w->vals);
        w->primed = realloc(w->primed, (size_t)w->capfl * sizeof *w->primed);
    }

    char buf[256];
    snprintf(buf, sizeof buf, "%s'", intern_name(w->syms, atom));
    w->fluents[w->nfl] = atom;
    w->vals[w->nfl] = false;
    w->primed[w->nfl] = intern_id(w->syms, buf);
    w->nfl++;
    w->fam_dirty = true;
}

void world_set(world *w, uint32_t atom, bool value)
{
    int i = fluent_index(w, atom);
    if (i >= 0)
        w->vals[i] = value;
}

bool world_get(const world *w, uint32_t atom)
{
    int i = fluent_index(w, atom);
    return i >= 0 && w->vals[i];
}

/* ---- numeric value store & guards (§5.8, read side) ---------------- */

static int num_index(const world *w, uint32_t atom)
{
    for (int i = 0; i < w->nnum; i++)
        if (w->nums[i].atom == atom) return i;
    return -1;
}

void world_declare_num(world *w, uint32_t atom, long min, long max, bool has_range)
{
    if (num_index(w, atom) >= 0) return;
    GROW(w->nums, w->nnum, w->capnum);
    w->nums[w->nnum].atom = atom;
    w->nums[w->nnum].value = 0;
    w->nums[w->nnum].min = min;
    w->nums[w->nnum].max = max;
    w->nums[w->nnum].has_range = has_range;
    w->nnum++;
}

void world_set_num(world *w, uint32_t atom, long value)
{
    int i = num_index(w, atom);
    if (i >= 0) w->nums[i].value = value;
}

long world_get_num(const world *w, uint32_t atom)
{
    int i = num_index(w, atom);
    return i >= 0 ? w->nums[i].value : 0;
}

void world_add_guard(world *w, uint32_t guard, uint32_t num,
                     world_cmp op, long threshold)
{
    for (int i = 0; i < w->ng; i++)          /* dedup: one atom per (fluent,op,thr) */
        if (w->guards[i].guard == guard) return;
    GROW(w->guards, w->ng, w->capg);
    w->guards[w->ng].guard = guard;
    w->guards[w->ng].num = num;
    w->guards[w->ng].op = op;
    w->guards[w->ng].threshold = threshold;
    w->ng++;
}

/* Does guard g hold for the current value of its numeric fluent? */
static bool guard_holds(const world *w, int g)
{
    long v = world_get_num(w, w->guards[g].num), t = w->guards[g].threshold;
    switch (w->guards[g].op) {
    case WORLD_CMP_LE: return v <= t;
    case WORLD_CMP_LT: return v <  t;
    case WORLD_CMP_GE: return v >= t;
    case WORLD_CMP_GT: return v >  t;
    case WORLD_CMP_EQ: return v == t;
    }
    return false;
}

int world_add_rule(world *w, const char *name, dl_rule_kind kind,
                   dl_lit head, const dl_lit *body, int nbody)
{
    GROW(w->jrules, w->njr, w->capjr);
    jrule *r = &w->jrules[w->njr];
    r->name = arena_strdup(&w->a, name);
    r->kind = kind;
    r->head = head;
    r->nbody = nbody;
    r->body = arena_alloc(&w->a, (size_t)(nbody ? nbody : 1) * sizeof(dl_lit));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(dl_lit));
    w->fam_dirty = true;
    return w->njr++;
}

void world_add_sup(world *w, int winner, int loser)
{
    GROW(w->jsups, w->njs, w->capjs);
    w->jsups[w->njs].winner = winner;
    w->jsups[w->njs].loser = loser;
    w->njs++;
    w->fam_dirty = true;
}

int world_add_step_rule(world *w, const char *name, uint32_t action,
                        const step_cond *body, int nbody,
                        const dl_lit *effects, int neffects)
{
    GROW(w->srules, w->nsr, w->capsr);
    srule *r = &w->srules[w->nsr];
    r->name = arena_strdup(&w->a, name);
    r->action = action;
    r->nbody = nbody;
    r->body = arena_alloc(&w->a, (size_t)(nbody ? nbody : 1) * sizeof(step_cond));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(step_cond));
    r->neffects = neffects;
    r->effects = arena_alloc(&w->a, (size_t)(neffects ? neffects : 1) * sizeof(dl_lit));
    if (neffects)
        memcpy(r->effects, effects, (size_t)neffects * sizeof(dl_lit));
    r->neffs = NULL;
    r->nneff = r->capneff = 0;
    w->fam_dirty = true;
    return w->nsr++;
}

void world_add_num_effect(world *w, int rule, uint32_t num_atom,
                          world_numop op, const expr_ins *code, int ncode)
{
    srule *r = &w->srules[rule];
    /* grow the effect list; it lives in the arena, so double by re-copying */
    if (r->nneff == r->capneff) {
        int nc = r->capneff ? r->capneff * 2 : 4;
        num_effect *ne = arena_alloc(&w->a, (size_t)nc * sizeof *ne);
        if (r->nneff)
            memcpy(ne, r->neffs, (size_t)r->nneff * sizeof *ne);
        r->neffs = ne;
        r->capneff = nc;
    }
    expr_ins *owncode = arena_alloc(&w->a, (size_t)(ncode ? ncode : 1) * sizeof *owncode);
    if (ncode)
        memcpy(owncode, code, (size_t)ncode * sizeof *owncode);
    num_effect *e = &r->neffs[r->nneff++];
    e->num_atom = num_atom;
    e->op = op;
    e->code = owncode;
    e->ncode = ncode;
    /* numeric effects run in the commit phase, not the fixpoint — the cached
     * boolean family is unaffected, so no fam_dirty here. */
}

/* Closed-world base facts + judgment rules; jrule handle i maps to dl rule
 * id i because they are added first and in order. */
static void add_state_and_judgments(const world *w, dl_theory *th)
{
    for (int j = 0; j < w->njr; j++) {
        const jrule *r = &w->jrules[j];
        dl_add_rule(th, r->name, r->kind, r->head, r->body, r->nbody);
    }
    for (int j = 0; j < w->njs; j++)
        dl_add_sup(th, w->jsups[j].winner, w->jsups[j].loser);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit f = dl_pos(w->fluents[i]);
        dl_add_fact(th, w->vals[i] ? f : dl_complement(f));
    }
    /* numeric guard atoms: closed-world from the value store, both polarities */
    for (int g = 0; g < w->ng; g++) {
        dl_lit gl = dl_pos(w->guards[g].guard);
        dl_add_fact(th, guard_holds(w, g) ? gl : dl_complement(gl));
    }
}

dl_verdict world_query(world *w, dl_lit q)
{
    dl_theory *th = dl_theory_new(w->syms);
    add_state_and_judgments(w, th);
    dl_result *res = dl_solve(th);
    dl_verdict v = dl_defeasible(res, q);
    dl_result_free(res);
    dl_theory_free(th);
    return v;
}

void world_why(world *w, dl_lit q, FILE *out)
{
    dl_theory *th = dl_theory_new(w->syms);
    add_state_and_judgments(w, th);
    dl_result *res = dl_solve(th);
    dl_why(th, res, q, out);
    dl_result_free(res);
    dl_theory_free(th);
}

static dl_lit primed_lit(const world *w, dl_lit l)
{
    int i = fluent_index(w, l.atom);
    dl_lit p = { w->primed[i], l.neg };
    return p;
}

/* ---- the columnar step schema ----
 *
 * The theory world_step used to rebuild per call, built once into an N=1
 * dlcol family: judgment rules, generated inertia (f => f', ~f => ~f'),
 * causal rules each superior to the inertia rule they conflict with.
 * Semantics identical to the scalar construction below (the golden world
 * tests pin it); only the per-step cost changes. */

#define LOC_NONE (~0u)

static uint32_t assign_loc(world *w, uint32_t atom, uint32_t *n)
{
    if (w->loc_of[atom] == LOC_NONE)
        w->loc_of[atom] = (*n)++;
    return w->loc_of[atom];
}

static dl_lit loc_lit(const world *w, dl_lit l)
{
    dl_lit m = { w->loc_of[l.atom], l.neg };
    return m;
}

static void build_step_family(world *w)
{
    if (w->fam)
        dlcol_free(w->fam);

    /* pass 1: assign dense schema ids to every atom the step theory touches */
    uint32_t na = intern_count(w->syms);
    w->loc_of = realloc(w->loc_of, (size_t)na * sizeof *w->loc_of);
    memset(w->loc_of, 0xff, (size_t)na * sizeof *w->loc_of);
    w->loc_cap = na;
    uint32_t n = 0;
    w->fl_loc = realloc(w->fl_loc, (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->fl_loc);
    w->pr_loc = realloc(w->pr_loc, (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->pr_loc);
    for (int i = 0; i < w->nfl; i++) {
        w->fl_loc[i] = assign_loc(w, w->fluents[i], &n);
        w->pr_loc[i] = assign_loc(w, w->primed[i], &n);
    }
    for (int j = 0; j < w->njr; j++) {
        assign_loc(w, w->jrules[j].head.atom, &n);
        for (int i = 0; i < w->jrules[j].nbody; i++)
            assign_loc(w, w->jrules[j].body[i].atom, &n);
    }
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        if (r->action != INTERN_NONE)
            assign_loc(w, r->action, &n);
        for (int i = 0; i < r->nbody; i++)
            assign_loc(w, r->body[i].primed
                              ? primed_lit(w, r->body[i].lit).atom
                              : r->body[i].lit.atom, &n);
        /* effect heads are primed fluents — already assigned */
    }

    /* pass 2: emit the schema */
    dlcol *f = dlcol_new((int)n, 1);
    for (uint32_t a = 0; a < na; a++)
        if (w->loc_of[a] != LOC_NONE)
            dlcol_set_atom_name(f, w->loc_of[a], intern_name(w->syms, a));

    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    char buf[300];

    /* judgments first: jsup handles equal schema rule ids */
    for (int j = 0; j < w->njr; j++) {
        const jrule *r = &w->jrules[j];
        if (r->nbody > bodycap) {
            body = malloc((size_t)r->nbody * sizeof *body);
            bodycap = r->nbody;
        }
        for (int i = 0; i < r->nbody; i++)
            body[i] = loc_lit(w, r->body[i]);
        dlcol_add_rule(f, r->name, r->kind, loc_lit(w, r->head), body, r->nbody);
    }
    for (int j = 0; j < w->njs; j++)
        dlcol_add_sup(f, w->jsups[j].winner, w->jsups[j].loser);

    /* generated inertia, ids recorded for causal superiority */
    int *inertia_pos = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_pos);
    int *inertia_neg = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_neg);
    for (int i = 0; i < w->nfl; i++) {
        const char *fname = intern_name(w->syms, w->fluents[i]);
        dl_lit now = { w->fl_loc[i], false }, nxt = { w->pr_loc[i], false };
        snprintf(buf, sizeof buf, "inertia+%s", fname);
        inertia_pos[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nxt, &now, 1);
        dl_lit nnow = dl_complement(now), nnxt = dl_complement(nxt);
        snprintf(buf, sizeof buf, "inertia-%s", fname);
        inertia_neg[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nnxt, &nnow, 1);
    }

    /* causal rules and ramifications, one rule per effect, each superior to
     * the inertia rule it conflicts with */
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        int nbody = r->nbody + (r->action != INTERN_NONE ? 1 : 0);
        if (nbody > bodycap) {
            if (body != lbuf)
                free(body);
            body = malloc((size_t)nbody * sizeof *body);
            bodycap = nbody;
        }
        int bi = 0;
        for (int i = 0; i < r->nbody; i++)
            body[bi++] = loc_lit(w, r->body[i].primed
                                        ? primed_lit(w, r->body[i].lit)
                                        : r->body[i].lit);
        if (r->action != INTERN_NONE) {
            dl_lit act = { r->action, false };
            body[bi++] = loc_lit(w, act);
        }
        for (int e = 0; e < r->neffects; e++) {
            dl_lit eff = r->effects[e];
            int fi = fluent_index(w, eff.atom);
            dl_lit head = { w->pr_loc[fi], eff.neg };
            snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                     eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
            int rid = dlcol_add_rule(f, buf, DL_DEFEASIBLE, head, body, nbody);
            /* effect f' conflicts with inertia-f ; effect ~f' with inertia+f */
            dlcol_add_sup(f, rid, eff.neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
    }
    if (body != lbuf)
        free(body);
    free(inertia_pos);
    free(inertia_neg);

    w->fam = f;
    w->fam_dirty = false;
}

/* ---- numeric write side: expression VM + commit pipeline (§5.8) ---- */

/* Stack VM over `long`; integer-only. Reads numeric fluents' *current* values
 * (the store double-buffers, so every effect this step sees the pre-step
 * state). Bytecode is compiler-emitted and well-formed; depth 64 covers any
 * M1 effect expression. */
static long eval_expr(const world *w, const expr_ins *code, int n)
{
    long st[64];
    int sp = 0;
    for (int i = 0; i < n; i++) {
        switch (code[i].op) {
        case EXPR_CONST: st[sp++] = code[i].arg; break;
        case EXPR_LOAD:  st[sp++] = world_get_num(w, (uint32_t)code[i].arg); break;
        case EXPR_NEG:   st[sp-1] = -st[sp-1]; break;
        case EXPR_ADD:   sp--; st[sp-1] += st[sp]; break;
        case EXPR_SUB:   sp--; st[sp-1] -= st[sp]; break;
        case EXPR_MUL:   sp--; st[sp-1] *= st[sp]; break;
        case EXPR_MIN:   sp--; if (st[sp] < st[sp-1]) st[sp-1] = st[sp]; break;
        case EXPR_MAX:   sp--; if (st[sp] > st[sp-1]) st[sp-1] = st[sp]; break;
        }
    }
    return sp ? st[sp-1] : 0;
}

/* A step rule fires this step iff its action occurred (or it is a ramification)
 * and every body literal holds in the settled step theory. Numeric effects run
 * in the commit phase, so their firing is read off the solved columns rather
 * than resolved inside the fixpoint (that is why suppression-by-superiority
 * over numeric effects is a later slice — here every fired effect contributes). */
static bool srule_fired(const world *w, const srule *r,
                        const uint32_t *actions, int nactions)
{
    if (r->action != INTERN_NONE) {
        bool occurred = false;
        for (int i = 0; i < nactions; i++)
            if (actions[i] == r->action) { occurred = true; break; }
        if (!occurred) return false;
    }
    for (int i = 0; i < r->nbody; i++) {
        dl_lit l = r->body[i].primed ? primed_lit(w, r->body[i].lit)
                                     : r->body[i].lit;
        if (l.atom >= w->loc_cap || w->loc_of[l.atom] == LOC_NONE)
            return false;
        dl_lit loc = { w->loc_of[l.atom], l.neg };
        if (dlcol_defeasible(w->fam, loc, 0) != DL_PROVED)
            return false;
    }
    return true;
}

static void rcpt_push(num_receipt *rc, const char *rule, world_numop op, long amt)
{
    if (rc->n == rc->cap) {
        rc->cap = rc->cap ? rc->cap * 2 : 4;
        rc->items = realloc(rc->items, (size_t)rc->cap * sizeof *rc->items);
    }
    rc->items[rc->n].rule = rule;
    rc->items[rc->n].op = op;
    rc->items[rc->n].amount = amt;
    rc->n++;
}

int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz)
{
    if (!w->fam || w->fam_dirty)
        build_step_family(w);
    dlcol *f = w->fam;

    /* fact columns: current state closed-world, plus occurring actions.
     * An action atom no rule mentions is semantically inert; skip it. */
    dlcol_clear_facts(f);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit l = { w->fl_loc[i], !w->vals[i] };
        dlcol_add_fact(f, l, 0);
    }
    for (int i = 0; i < nactions; i++) {
        uint32_t a = actions[i];
        if (a < w->loc_cap && w->loc_of[a] != LOC_NONE) {
            dl_lit l = { w->loc_of[a], false };
            dlcol_add_fact(f, l, 0);
        }
    }
    /* numeric guard atoms feed judgment rules inside the step theory too */
    for (int g = 0; g < w->ng; g++) {
        uint32_t ga = w->guards[g].guard;
        if (ga < w->loc_cap && w->loc_of[ga] != LOC_NONE) {
            dl_lit l = { w->loc_of[ga], !guard_holds(w, g) };
            dlcol_add_fact(f, l, 0);
        }
    }

    dlcol_solve(f);

    int rc = 0;
    bool *next = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *next);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit p = { w->pr_loc[i], false };
        if (dlcol_defeasible(f, p, 0) == DL_PROVED) {
            next[i] = true;
        } else if (dlcol_defeasible(f, dl_complement(p), 0) == DL_PROVED) {
            next[i] = false;
        } else {
            if (err)
                snprintf(err, errsz,
                         "conflicting or undecided effects on fluent '%s'",
                         intern_name(w->syms, w->fluents[i]));
            rc = -1;
            break;
        }
    }

    /* numeric commit pipeline (§5.8): base (winning := else inertia) + Σ deltas,
     * clamped to the declared range. Built into scratch + receipts, committed
     * with the boolean state only if nothing is contested. */
    long *nextnum = malloc((size_t)(w->nnum ? w->nnum : 1) * sizeof *nextnum);
    if (rc == 0 && w->nnum > w->caprcpt) {
        w->rcpt = realloc(w->rcpt, (size_t)w->nnum * sizeof *w->rcpt);
        memset(&w->rcpt[w->caprcpt], 0,
               (size_t)(w->nnum - w->caprcpt) * sizeof *w->rcpt);
        w->caprcpt = w->nnum;
    }
    for (int i = 0; rc == 0 && i < w->nnum; i++) {
        num_receipt *rcp = &w->rcpt[i];
        rcp->n = 0;
        bool have_assign = false, assign_conflict = false;
        long assign_val = 0, delta = 0;
        const char *assign_rule = NULL;

        for (int s = 0; s < w->nsr; s++) {
            const srule *r = &w->srules[s];
            if (r->nneff == 0 || !srule_fired(w, r, actions, nactions))
                continue;
            for (int e = 0; e < r->nneff; e++) {
                const num_effect *ef = &r->neffs[e];
                if (ef->num_atom != w->nums[i].atom)
                    continue;
                long v = eval_expr(w, ef->code, ef->ncode);
                if (ef->op == WORLD_OP_ASSIGN) {
                    if (have_assign) {
                        if (v != assign_val) assign_conflict = true;
                    } else {
                        have_assign = true;
                        assign_val = v;
                        assign_rule = r->name;
                    }
                } else {
                    long d = (ef->op == WORLD_OP_ADD) ? v : -v;
                    delta += d;
                    rcpt_push(rcp, r->name, ef->op, d);
                }
            }
        }
        if (assign_conflict) {
            if (err)
                snprintf(err, errsz,
                         "conflicting `:=` effects on numeric fluent '%s'",
                         intern_name(w->syms, w->nums[i].atom));
            rc = -1;
            break;
        }
        /* receipt order: winning assign first, then the deltas in scan order */
        if (have_assign) {
            rcpt_push(rcp, NULL, WORLD_OP_ASSIGN, 0);   /* grow, then shift */
            memmove(&rcp->items[1], &rcp->items[0],
                    (size_t)(rcp->n - 1) * sizeof *rcp->items);
            rcp->items[0].rule = assign_rule;
            rcp->items[0].op = WORLD_OP_ASSIGN;
            rcp->items[0].amount = assign_val;
        }
        long base = have_assign ? assign_val : w->nums[i].value;
        rcp->base = base;
        long val = base + delta;
        if (w->nums[i].has_range) {
            if (val < w->nums[i].min) val = w->nums[i].min;
            if (val > w->nums[i].max) val = w->nums[i].max;
        }
        nextnum[i] = val;
    }

    if (rc == 0) {
        if (w->nfl)
            memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);
        for (int i = 0; i < w->nnum; i++)
            w->nums[i].value = nextnum[i];
    }

    free(next);
    free(nextnum);
    return rc;
}

int world_num_receipt(const world *w, uint32_t atom, long *base,
                      world_contrib *out, int cap)
{
    int i = num_index(w, atom);
    if (i < 0 || !w->rcpt) {
        if (base) *base = 0;
        return 0;
    }
    const num_receipt *rcp = &w->rcpt[i];
    if (base) *base = rcp->base;
    for (int k = 0; k < rcp->n && k < cap; k++)
        out[k] = rcp->items[k];
    return rcp->n;
}
