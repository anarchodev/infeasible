#include "state/world.h"
#include "core/arena.h"
#include "logic/dl_col.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *prov;     /* provenance suffix (§6.3), or NULL */
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
    const char *prov;     /* provenance suffix (§6.3), or NULL */
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

/* A lane family (the DoD thesis): one dl_col schema over `nent` lane entities.
 * `niter` is the join iteration count — 1 for a single-variable rule set (solve
 * once); for a two-variable rule it is the size of the non-lane sort, and the
 * family is re-solved per iteration against a different fact slice (lane one
 * axis, iterate the other). `ground[(a*niter + it)*nent + e]` is the named
 * ground atom for predicate-local `a` at iteration `it`, lane `e`; `is_fluent[a]`
 * flags locals that take base facts. */
typedef struct {
    dlcol   *fam;
    int      natoms, nent, niter;
    uint32_t *ground;             /* [natoms*niter*nent] */
    bool    *is_fluent;           /* [natoms]: local takes closed-world base facts */
    bool    *is_import;           /* [natoms]: local is derived elsewhere, its
                                   * per-cell verdict queried and injected (§5.5) */
    bool     solved;              /* fam currently holds iteration cur_it, and
                                   * that solve reflects the live base facts */
    int      cur_it;              /* which iteration is loaded/solved in fam
                                   * (always 0 when niter==1) */
} lane_family;

/* reverse map: a named ground atom -> its (family, predicate-local, lane,
 * iteration), so world_query can route to the lane family. fam < 0 = not a
 * routable lane atom (unmentioned, or ambiguous within a join family). */
typedef struct { int fam, a, e, it; } lane_ref;

/* A step lane family: the transition theory (generated inertia + causal rules)
 * over one lane sort, bit-parallel across `nent` entities. Each local is a
 * current fluent (loaded closed-world from the fact store), a primed fluent (the
 * next-state readout), or an action (loaded from the step's action list).
 * `ground[a*nent + e]` names the equivalent scalar atom for local `a`, lane `e` —
 * the primed local's is the fluent's `f'` twin, so the differential can look up
 * the N=1 verdict. Prototype-before-adopt: validated, not yet driving world_step. */
typedef struct {
    dlcol   *fam;
    int      nloc, nent;
    uint32_t *ground;             /* [nloc*nent] */
    uint8_t *kind;                /* [nloc]: WORLD_STEP_{CUR,PRIMED,ACTION} */
    int     *fl_of;               /* [nloc*nent]: world fluent index for a CUR cell
                                   * (fact source) or PRIMED cell (commit target),
                                   * -1 otherwise — so the solve reads w->vals and
                                   * the commit writes it directly, no linear scan */
    int     *act_of;              /* [act_of_cap]: ground action atom -> flat cell
                                   * (local*nent + lane), -1 if not an action cell —
                                   * so a step's action list maps to lanes in O(k) */
    uint32_t act_of_cap;
} step_lane_family;

struct world {
    arena a;
    intern *syms;
    uint32_t *fluents; bool *vals; uint32_t *primed;
    const char **fl_prov;         /* decl span per fluent (§6.3), or NULL */
    int nfl, capfl;
    /* atom -> index maps, so declare/lookup is O(1) not a linear scan (interns
     * are dense uint32, so a direct-indexed array is the natural perfect hash).
     * Grown geometrically; slot value -1 = absent. Fluents/nums are append-only. */
    int *fluent_of; uint32_t fluent_of_cap;
    int *num_of;    uint32_t num_of_cap;
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
    dlcol *fam;                   /* step family: judgments + inertia + causal  */
    dlcol *jfam;                  /* judgment family: judgments only (the query
                                   * layer — DESIGN.md §6.3, one columnar engine
                                   * for both "what's true" and "what happens") */
    bool fam_dirty;               /* structure stale: rebuild both families      */
    bool jfam_solved;             /* jfam holds the current state's judgments     */
    uint32_t *loc_of;             /* intern atom -> schema atom, ~0u = absent    */
    uint32_t loc_cap, nloc;       /* loc_of size; # assigned schema locations    */
    uint32_t *fl_loc, *pr_loc;    /* per fluent: schema ids of f and f'          */

    lane_family *lanes;           /* per-sort N-lane families (DoD thesis)       */
    int nlanes, caplanes;
    lane_ref *lane_map;           /* ground atom -> (family, local, lane)        */
    uint32_t lane_map_cap;
    bool lanes_ok;                /* lane families reflect the current structure */

    step_lane_family *steplanes;  /* transition layer, bit-parallel (DoD thesis) */
    int nsteplanes, capsteplanes;

    /* When world_step is answered on the step lanes, w->fam does NOT hold the
     * transition — so world_step_why lazily re-solves it from this snapshot of
     * the pre-step state + actions (the analog of world_why staying on jfam). */
    bool last_routed;
    bool *step_snap; int step_snap_cap;
    uint32_t *last_actions; int last_nactions, last_actions_cap;
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

/* A base-fact edit: the judgment family and every lane family must re-solve. */
static void invalidate_state_solved(world *w)
{
    w->jfam_solved = false;
    for (int i = 0; i < w->nlanes; i++)
        w->lanes[i].solved = false;
}

void world_free(world *w)
{
    if (w->fam)
        dlcol_free(w->fam);
    if (w->jfam)
        dlcol_free(w->jfam);
    for (int i = 0; i < w->nlanes; i++) {
        dlcol_free(w->lanes[i].fam);
        free(w->lanes[i].ground);
        free(w->lanes[i].is_fluent);
        free(w->lanes[i].is_import);
    }
    free(w->lanes);
    for (int i = 0; i < w->nsteplanes; i++) {
        dlcol_free(w->steplanes[i].fam);
        free(w->steplanes[i].ground);
        free(w->steplanes[i].kind);
        free(w->steplanes[i].fl_of);
        free(w->steplanes[i].act_of);
    }
    free(w->steplanes);
    free(w->step_snap);
    free(w->last_actions);
    free(w->lane_map);
    free(w->loc_of);
    free(w->fl_loc);
    free(w->pr_loc);
    arena_release(&w->a);
    free(w->fluents);
    free(w->vals);
    free(w->primed);
    free(w->fl_prov);
    free(w->fluent_of);
    free(w->num_of);
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

/* atom -> index map: geometric growth keeps declare amortized O(1) even when
 * atoms arrive in increasing id order (a per-call grow-to-atom+1 would be O(n^2)
 * of reallocs — the very trap this replaces). New slots init to -1. */
static void atom_map_set(int **map, uint32_t *cap, uint32_t key, int val)
{
    if (key >= *cap) {
        uint32_t nc = *cap ? *cap : 16;
        while (nc <= key) nc *= 2;
        *map = realloc(*map, (size_t)nc * sizeof **map);
        for (uint32_t k = *cap; k < nc; k++) (*map)[k] = -1;
        *cap = nc;
    }
    (*map)[key] = val;
}

static int fluent_index(const world *w, uint32_t atom)
{
    return atom < w->fluent_of_cap ? w->fluent_of[atom] : -1;
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
        w->fl_prov = realloc(w->fl_prov, (size_t)w->capfl * sizeof *w->fl_prov);
    }

    char buf[256];
    snprintf(buf, sizeof buf, "%s'", intern_name(w->syms, atom));
    w->fluents[w->nfl] = atom;
    w->vals[w->nfl] = false;
    w->primed[w->nfl] = intern_id(w->syms, buf);
    w->fl_prov[w->nfl] = NULL;
    atom_map_set(&w->fluent_of, &w->fluent_of_cap, atom, w->nfl);
    w->nfl++;
    w->fam_dirty = true;
    w->lanes_ok = false;          /* a structural edit stales the lane families */
}

/* Where a fluent was declared (§6.3), for its generated inertia rules'
 * provenance. `at` is a "srcname:line" span; copied, NULL clears it. */
void world_set_fluent_prov(world *w, uint32_t atom, const char *at)
{
    int i = fluent_index(w, atom);
    if (i >= 0)
        w->fl_prov[i] = at ? arena_strdup(&w->a, at) : NULL;
}

void world_set(world *w, uint32_t atom, bool value)
{
    int i = fluent_index(w, atom);
    if (i >= 0) {
        w->vals[i] = value;
        invalidate_state_solved(w);                /* judgments now stale */
    }
}

bool world_get(const world *w, uint32_t atom)
{
    int i = fluent_index(w, atom);
    return i >= 0 && w->vals[i];
}

/* ---- numeric value store & guards (§5.8, read side) ---------------- */

static int num_index(const world *w, uint32_t atom)
{
    return atom < w->num_of_cap ? w->num_of[atom] : -1;
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
    atom_map_set(&w->num_of, &w->num_of_cap, atom, w->nnum);
    w->nnum++;
}

void world_set_num(world *w, uint32_t atom, long value)
{
    int i = num_index(w, atom);
    if (i >= 0) {
        w->nums[i].value = value;
        invalidate_state_solved(w);                /* guard truth may change */
    }
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
    r->prov = NULL;
    r->kind = kind;
    r->head = head;
    r->nbody = nbody;
    r->body = arena_alloc(&w->a, (size_t)(nbody ? nbody : 1) * sizeof(dl_lit));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(dl_lit));
    w->fam_dirty = true;
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    return w->njr++;
}

void world_add_sup(world *w, int winner, int loser)
{
    GROW(w->jsups, w->njs, w->capjs);
    w->jsups[w->njs].winner = winner;
    w->jsups[w->njs].loser = loser;
    w->njs++;
    w->fam_dirty = true;
    w->lanes_ok = false;          /* a structural edit stales the lane families */
}

int world_add_step_rule(world *w, const char *name, uint32_t action,
                        const step_cond *body, int nbody,
                        const dl_lit *effects, int neffects)
{
    GROW(w->srules, w->nsr, w->capsr);
    srule *r = &w->srules[w->nsr];
    r->name = arena_strdup(&w->a, name);
    r->prov = NULL;
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
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    return w->nsr++;
}

void world_set_rule_prov(world *w, int rule, const char *prov)
{
    if (rule >= 0 && rule < w->njr)
        w->jrules[rule].prov = prov ? arena_strdup(&w->a, prov) : NULL;
}

void world_set_step_prov(world *w, int rule, const char *prov)
{
    if (rule >= 0 && rule < w->nsr) {
        w->srules[rule].prov = prov ? arena_strdup(&w->a, prov) : NULL;
        w->fam_dirty = true;
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    }
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

#define LOC_NONE (~0u)   /* schema location for an atom absent from the family */

/* Both families (step + judgment) are built from one location map; the query
 * layer runs on the columnar engine too (DESIGN.md §6.3 — one production
 * engine, the scalar dl kept only as test_col's differential oracle). */
static void ensure_families(world *w);
static void solve_judgment_family(world *w);
static void solve_lane_iter(world *w, lane_family *lf, int it);
static void solve_step_family(world *w, const uint32_t *actions, int nactions);
static void solve_step_family_vals(world *w, const bool *vals,
                                   const uint32_t *actions, int nactions);

/* The N=1 judgment path: the proven route, and the oracle world_lanes_check
 * measures the lane families against. */
static dl_verdict query_jfam(world *w, dl_lit q)
{
    ensure_families(w);
    if (!w->jfam_solved)
        solve_judgment_family(w);
    if (q.atom >= w->loc_cap || w->loc_of[q.atom] == LOC_NONE)
        return DL_UNDECIDED;                       /* absent: unmentioned atom */
    dl_lit loc = { w->loc_of[q.atom], q.neg };
    return dlcol_defeasible(w->jfam, loc, 0);
}

dl_verdict world_query(world *w, dl_lit q)
{
    ensure_families(w);
    /* the hot path: if this atom is a lane cell, answer from the bit-parallel
     * family (all lanes solved at once) instead of the N=1 judgment family. For
     * a join family the cell names an iteration too; solve that iteration's fact
     * slice on demand and cache it (cur_it), so a run of queries into the same
     * slice pays one solve — the per-iteration adopt of the join matcher. */
    if (w->lanes_ok && q.atom < w->lane_map_cap && w->lane_map[q.atom].fam >= 0) {
        lane_ref r = w->lane_map[q.atom];
        lane_family *lf = &w->lanes[r.fam];
        if (!lf->solved || lf->cur_it != r.it) {
            solve_lane_iter(w, lf, r.it);
            lf->cur_it = r.it;
            lf->solved = true;
        }
        dl_lit la = { (uint32_t)r.a, q.neg };
        return dlcol_defeasible(lf->fam, la, r.e);
    }
    return query_jfam(w, q);
}

void world_why(world *w, dl_lit q, FILE *out)
{
    ensure_families(w);
    if (!w->jfam_solved)
        solve_judgment_family(w);
    if (q.atom >= w->loc_cap || w->loc_of[q.atom] == LOC_NONE) {
        fprintf(out, "why %s%s?\n  (not in the theory — no rule or fact)\n",
                q.neg ? "~" : "", intern_name(w->syms, q.atom));
        return;
    }
    dl_lit loc = { w->loc_of[q.atom], q.neg };
    dlcol_why(w->jfam, loc, 0, out);
}

/* ---- lane families (DoD thesis) ---- */

void world_add_lane_family(world *w, dlcol *fam, int natoms, int nent, int niter,
                           const uint32_t *ground, const bool *is_fluent,
                           const bool *is_import)
{
    GROW(w->lanes, w->nlanes, w->caplanes);
    int fi = w->nlanes;
    lane_family *lf = &w->lanes[w->nlanes++];
    lf->fam = fam;
    lf->natoms = natoms;
    lf->nent = nent;
    lf->niter = niter;
    lf->solved = false;
    lf->cur_it = 0;
    size_t g = (size_t)natoms * (size_t)niter * (size_t)nent;
    lf->ground = malloc((g ? g : 1) * sizeof *lf->ground);
    memcpy(lf->ground, ground, (g ? g : 1) * sizeof *lf->ground);
    lf->is_fluent = malloc((size_t)(natoms ? natoms : 1) * sizeof *lf->is_fluent);
    memcpy(lf->is_fluent, is_fluent, (size_t)(natoms ? natoms : 1) * sizeof *lf->is_fluent);
    lf->is_import = calloc((size_t)(natoms ? natoms : 1), sizeof *lf->is_import);
    if (is_import)
        memcpy(lf->is_import, is_import, (size_t)(natoms ? natoms : 1) * sizeof *lf->is_import);

    /* Index each ground atom back to its lane cell so world_query can route —
     * for join families (niter>1) as well as single-variable ones. A join
     * family repeats role-0-only atoms across iterations and role-1-only atoms
     * across lanes, so a ground atom can name more than one cell; such an atom
     * has no single verdict to route to and stays on the N=1 path. Only atoms
     * UNIQUE within the family are routable — which, for a join, are exactly the
     * ones mentioning both variables (the relational head and binary bodies).
     * Single-variable families (niter==1) are unique by construction. First
     * family to claim an atom keeps it; a later family's cell for the same atom
     * is redundant (both validated against the same N=1 verdict). */
    size_t ncells = (size_t)natoms * (size_t)niter * (size_t)nent;
    uint32_t maxat = 0;
    for (size_t k = 0; k < ncells; k++)
        if (ground[k] > maxat) maxat = ground[k];
    uint8_t *mult = calloc((size_t)maxat + 1, 1);   /* 0 unseen, 1 once, 2 = dup */
    for (size_t k = 0; k < ncells; k++)
        if (mult[ground[k]] < 2) mult[ground[k]]++;

    for (int a = 0; a < natoms; a++) {
        if (is_import && is_import[a])
            continue;                          /* not concluded here: never route */
        for (int it = 0; it < niter; it++)
            for (int e = 0; e < nent; e++) {
                uint32_t at = ground[((size_t)a * niter + it) * nent + e];
                if (mult[at] != 1) continue;   /* ambiguous within family -> jfam */
                if (at >= w->lane_map_cap) {
                    uint32_t nc = at + 1;
                    w->lane_map = realloc(w->lane_map, (size_t)nc * sizeof *w->lane_map);
                    for (uint32_t k = w->lane_map_cap; k < nc; k++) w->lane_map[k].fam = -1;
                    w->lane_map_cap = nc;
                }
                if (w->lane_map[at].fam < 0)         /* unclaimed: first wins */
                    w->lane_map[at] = (lane_ref){ fi, a, e, it };
            }
    }
    free(mult);
    w->lanes_ok = true;
}

int world_lane_family_count(const world *w) { return w->nlanes; }

/* Load one iteration's fact slice into a lane family and solve it (all lanes at
 * once). For niter==1 (single-variable) `it` is 0 and the solve is the whole
 * family; for a join it is the current non-lane entity. */
static void solve_lane_iter(world *w, lane_family *lf, int it)
{
    dlcol_clear_facts(lf->fam);
    for (int a = 0; a < lf->natoms; a++) {
        if (lf->is_fluent[a]) {
            for (int e = 0; e < lf->nent; e++) {
                uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                dl_lit l = { (uint32_t)a, !world_get(w, g) };  /* a if true, ~a else */
                dlcol_add_fact(lf->fam, l, e);
            }
        } else if (lf->is_import[a]) {
            /* a derived body atom, concluded elsewhere: query it and inject the
             * conclusion for each cell. Inject a literal ONLY when it is genuinely
             * proved (+∂): +a if a is PROVED, ~a if the complement is PROVED. Do
             * NOT map REFUTED to a ~a fact — REFUTED means -∂a (a finitely failed),
             * which does not make ~a provable; injecting it would force the wrong
             * verdict on the complement. With nothing injected the atom finitely
             * fails here too (no local rule concludes it), matching the N=1 path.
             * Equivalent to a defeasible import (§5.5), since nothing local
             * concludes `a`. (A cyclic UNDECIDED import can't be reproduced this
             * way — the differential oracle would flag it; the compiler rejects
             * cycles.) */
            for (int e = 0; e < lf->nent; e++) {
                uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                if (world_query(w, (dl_lit){ g, false }) == DL_PROVED)
                    dlcol_add_fact(lf->fam, (dl_lit){ (uint32_t)a, false }, e);
                else if (world_query(w, (dl_lit){ g, true }) == DL_PROVED)
                    dlcol_add_fact(lf->fam, (dl_lit){ (uint32_t)a, true }, e);
            }
        }
    }
    dlcol_solve(lf->fam);
}

int world_lanes_check(world *w, bool *ok)
{
    int checks = 0;
    if (ok) *ok = true;
    for (int i = 0; i < w->nlanes; i++) {
        lane_family *lf = &w->lanes[i];
        for (int it = 0; it < lf->niter; it++) {
            solve_lane_iter(w, lf, it);
            /* every (predicate, lane) verdict at this iteration must match the
             * N=1 query path on the equivalent named ground atom */
            for (int a = 0; a < lf->natoms; a++)
                for (int e = 0; e < lf->nent; e++) {
                    uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                    for (int neg = 0; neg < 2; neg++) {
                        dl_lit la = { (uint32_t)a, neg != 0 };
                        dl_lit gq = { g, neg != 0 };
                        /* compare to the N=1 path directly, not world_query —
                         * which routes back to lanes and would be circular */
                        if (dlcol_defeasible(lf->fam, la, e) != query_jfam(w, gq))
                            if (ok) *ok = false;
                        checks++;
                    }
                }
        }
        lf->solved = false;   /* left dirty for the router (it re-solves iter 0) */
    }
    return checks;
}

/* ---- step lane families (DoD thesis: the transition layer, bit-parallel) ---- */

void world_add_step_lane_family(world *w, dlcol *fam, int nloc, int nent,
                                const uint32_t *ground, const uint8_t *kind)
{
    GROW(w->steplanes, w->nsteplanes, w->capsteplanes);
    step_lane_family *sf = &w->steplanes[w->nsteplanes++];
    sf->fam = fam;
    sf->nloc = nloc;
    sf->nent = nent;
    size_t g = (size_t)nloc * (size_t)nent;
    sf->ground = malloc((g ? g : 1) * sizeof *sf->ground);
    memcpy(sf->ground, ground, (g ? g : 1) * sizeof *sf->ground);
    sf->kind = malloc((size_t)(nloc ? nloc : 1) * sizeof *sf->kind);
    memcpy(sf->kind, kind, (size_t)(nloc ? nloc : 1) * sizeof *sf->kind);

    /* Map each cell to a world fluent index (CUR = fact source, PRIMED = commit
     * target) and each ground action atom to its cell, so a step reads w->vals
     * and its action list in O(cells)/O(k) — never the linear fluent_index scan.
     * A reverse index over the fluent + primed atoms makes the build O(cells)
     * too (a scan per cell would be O(nfl^2)). */
    sf->fl_of = malloc((g ? g : 1) * sizeof *sf->fl_of);
    for (size_t k = 0; k < g; k++) sf->fl_of[k] = -1;

    uint32_t maxf = 0;
    for (int i = 0; i < w->nfl; i++) {
        if (w->fluents[i] > maxf) maxf = w->fluents[i];
        if (w->primed[i]  > maxf) maxf = w->primed[i];
    }
    int *revf = malloc(((size_t)maxf + 1) * sizeof *revf);
    for (uint32_t k = 0; k <= maxf; k++) revf[k] = -1;
    for (int i = 0; i < w->nfl; i++) {
        revf[w->fluents[i]] = i;      /* the CUR ground atom is the fluent itself */
        revf[w->primed[i]]  = i;      /* the PRIMED ground atom is its f' twin    */
    }

    uint32_t maxa = 0;
    for (int a = 0; a < nloc; a++)
        if (kind[a] == WORLD_STEP_ACTION)
            for (int e = 0; e < nent; e++) {
                uint32_t ga = ground[(size_t)a * nent + e];
                if (ga > maxa) maxa = ga;
            }
    sf->act_of_cap = maxa + 1;
    sf->act_of = malloc((size_t)sf->act_of_cap * sizeof *sf->act_of);
    for (uint32_t k = 0; k < sf->act_of_cap; k++) sf->act_of[k] = -1;

    for (int a = 0; a < nloc; a++)
        for (int e = 0; e < nent; e++) {
            uint32_t ga = ground[(size_t)a * nent + e];
            size_t cell = (size_t)a * nent + e;
            if (kind[a] == WORLD_STEP_ACTION)
                sf->act_of[ga] = (int)cell;
            else
                sf->fl_of[cell] = ga <= maxf ? revf[ga] : -1;
        }
    free(revf);
    w->lanes_ok = true;           /* step lanes now reflect the current structure */
}

int world_step_lane_family_count(const world *w) { return w->nsteplanes; }

/* Load a step lane family's fact columns from the current state and the given
 * action list, and solve it bit-parallel across lanes. Current fluents are
 * closed-world; an action local's lane bit is set iff its ground action atom is
 * in `actions`; primed locals carry no facts (they are the readout). */
static void solve_step_lane_family(world *w, step_lane_family *sf,
                                   const uint32_t *actions, int nactions)
{
    int W = (sf->nent + 63) / 64;
    dlcol_clear_facts(sf->fam);
    for (int a = 0; a < sf->nloc; a++) {
        if (sf->kind[a] == WORLD_STEP_CUR) {
            /* closed-world current state, read straight from w->vals via fl_of */
            for (int e = 0; e < sf->nent; e++) {
                int i = sf->fl_of[(size_t)a * sf->nent + e];
                dl_lit l = { (uint32_t)a, !(i >= 0 && w->vals[i]) };
                dlcol_add_fact(sf->fam, l, e);
            }
        } else if (sf->kind[a] == WORLD_STEP_ACTION) {
            /* default every lane to "action did not occur" (~action); the
             * occurring ones are flipped below in O(#actions), not O(lanes) */
            uint64_t *pos = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, false });
            uint64_t *neg = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, true });
            memset(pos, 0x00, (size_t)W * sizeof *pos);
            memset(neg, 0xFF, (size_t)W * sizeof *neg);
        }
    }
    /* flip the occurring actions to true at their lanes (reverse map, O(k)) */
    for (int i = 0; i < nactions; i++) {
        uint32_t at = actions[i];
        if (at >= sf->act_of_cap) continue;
        int cell = sf->act_of[at];
        if (cell < 0) continue;
        int a = cell / sf->nent, e = cell % sf->nent;
        uint64_t *pos = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, false });
        uint64_t *neg = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, true });
        pos[e / 64] |=  (1ull << (e % 64));
        neg[e / 64] &= ~(1ull << (e % 64));
    }
    dlcol_solve(sf->fam);
}

int world_step_lanes_check(world *w, const uint32_t *actions, int nactions,
                           bool *ok)
{
    int checks = 0;
    if (ok) *ok = true;
    ensure_families(w);
    solve_step_family(w, actions, nactions);       /* the N=1 oracle */

    for (int i = 0; i < w->nsteplanes; i++) {
        step_lane_family *sf = &w->steplanes[i];
        solve_step_lane_family(w, sf, actions, nactions);
        /* every fluent's next-state verdict per lane must match the N=1 step
         * family on that fluent's primed twin */
        for (int a = 0; a < sf->nloc; a++) {
            if (sf->kind[a] != WORLD_STEP_PRIMED)
                continue;
            for (int e = 0; e < sf->nent; e++) {
                uint32_t pa = sf->ground[(size_t)a * sf->nent + e];   /* the f' atom */
                for (int neg = 0; neg < 2; neg++) {
                    dl_lit la = { (uint32_t)a, neg != 0 };
                    dl_verdict n1 = DL_UNDECIDED;
                    if (pa < w->loc_cap && w->loc_of[pa] != LOC_NONE) {
                        dl_lit gq = { w->loc_of[pa], neg != 0 };
                        n1 = dlcol_defeasible(w->fam, gq, 0);
                    }
                    if (dlcol_defeasible(sf->fam, la, e) != n1)
                        if (ok) *ok = false;
                    checks++;
                }
            }
        }
    }
    return checks;
}

/* Trace how a fluent got its value in the last step: the step theory (causal
 * rules, ramifications, generated inertia) as solved by the most recent
 * world_step. With `next` true, `q`'s atom is read in the next state (its primed
 * form) — the usual question, "why did `door` end up open?"; with `next` false,
 * the current-state value the step saw. Valid only after a world_step (the
 * family holds that transition's solution until the next step or edit). */
void world_step_why(world *w, dl_lit q, bool next, FILE *out)
{
    uint32_t atom = q.atom;
    if (next) {
        int i = fluent_index(w, atom);
        if (i >= 0)
            atom = w->primed[i];
    }
    if (!w->fam || w->fam_dirty ||
        atom >= w->loc_cap || w->loc_of[atom] == LOC_NONE) {
        fprintf(out, "why %s%s? not in the step theory%s\n",
                q.neg ? "~" : "", intern_name(w->syms, atom),
                w->fam_dirty ? " (no step taken since the last edit)" : "");
        return;
    }
    /* if the last step was answered on the lanes, w->fam does not hold that
     * transition — replay it from the snapshot so the trace is the real one. */
    if (w->last_routed)
        solve_step_family_vals(w, w->step_snap, w->last_actions, w->last_nactions);
    dl_lit loc = { w->loc_of[atom], q.neg };
    dlcol_why(w->fam, loc, 0, out);
}

static dl_lit primed_lit(const world *w, dl_lit l)
{
    int i = fluent_index(w, l.atom);
    dl_lit p = { w->primed[i], l.neg };
    return p;
}

/* ---- the columnar schemas ----
 *
 * Both "what's true" (judgments, the query layer) and "what happens next"
 * (inertia + causal, the step) run on the columnar engine — one production
 * engine, the scalar dl kept only as test_col's differential oracle (§6.3).
 * Two N=1 dlcol families share one location map: `jfam` is the judgment rules
 * over current-state facts; `fam` adds generated inertia (f => f', ~f => ~f')
 * and causal rules, each superior to the inertia rule it conflicts with. The
 * structure is fixed between edits, so a step or query just rewrites fact
 * columns and re-solves. The scalar tests pin that the semantics are identical;
 * both families are N=1 today (entities are baked into atom names), so this is
 * columnar-in-structure — the per-entity *lanes* that make it fast are the M3
 * join matcher, a later change on this same family API. */

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

/* pass 1, shared: assign dense schema ids to every atom either family touches
 * (both are sized to the same location space; the judgment family simply leaves
 * the primed/action columns unused). Sets w->nloc. */
static void assign_locs(world *w)
{
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
    w->nloc = n;
}

static void emit_atom_names(dlcol *f, const world *w)
{
    for (uint32_t a = 0; a < w->loc_cap; a++)
        if (w->loc_of[a] != LOC_NONE)
            dlcol_set_atom_name(f, w->loc_of[a], intern_name(w->syms, a));
}

/* Judgment rules + their superiority — the whole judgment family, and the
 * leading slice of the step family (primed bodies may read these conclusions). */
static void emit_judgment_rules(dlcol *f, const world *w)
{
    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    for (int j = 0; j < w->njr; j++) {
        const jrule *r = &w->jrules[j];
        if (r->nbody > bodycap) {
            if (body != lbuf) free(body);
            body = malloc((size_t)r->nbody * sizeof *body);
            bodycap = r->nbody;
        }
        for (int i = 0; i < r->nbody; i++)
            body[i] = loc_lit(w, r->body[i]);
        int h = dlcol_add_rule(f, r->name, r->kind, loc_lit(w, r->head),
                               body, r->nbody);
        if (r->prov)
            dlcol_set_prov(f, h, r->prov);
    }
    if (body != lbuf)
        free(body);
    for (int j = 0; j < w->njs; j++)
        dlcol_add_sup(f, w->jsups[j].winner, w->jsups[j].loser);
}

/* The judgment (query-layer) family: judgment rules only, over current-state
 * facts. Same location space as the step family, just without inertia/causal. */
static void emit_judgment_family(world *w)
{
    if (w->jfam)
        dlcol_free(w->jfam);
    dlcol *f = dlcol_new((int)w->nloc, 1);
    emit_atom_names(f, w);
    emit_judgment_rules(f, w);
    w->jfam = f;
    w->jfam_solved = false;
}

/* The step family: judgments + generated inertia + causal rules/ramifications. */
static void emit_step_family(world *w)
{
    if (w->fam)
        dlcol_free(w->fam);
    dlcol *f = dlcol_new((int)w->nloc, 1);
    emit_atom_names(f, w);
    emit_judgment_rules(f, w);

    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    char buf[300];

    /* generated inertia, ids recorded for causal superiority */
    int *inertia_pos = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_pos);
    int *inertia_neg = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_neg);
    for (int i = 0; i < w->nfl; i++) {
        const char *fname = intern_name(w->syms, w->fluents[i]);
        dl_lit now = { w->fl_loc[i], false }, nxt = { w->pr_loc[i], false };
        /* generated inertia reads in source terms (§6.3): a name the author
         * recognizes, and a provenance pointing at the fluent's declaration. */
        char prov[320];
        if (w->fl_prov[i])
            snprintf(prov, sizeof prov, "generated; declared %s", w->fl_prov[i]);
        else
            snprintf(prov, sizeof prov, "generated");
        snprintf(buf, sizeof buf, "inertia on %s", fname);
        inertia_pos[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nxt, &now, 1);
        dlcol_set_prov(f, inertia_pos[i], prov);
        dl_lit nnow = dl_complement(now), nnxt = dl_complement(nxt);
        inertia_neg[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nnxt, &nnow, 1);
        dlcol_set_prov(f, inertia_neg[i], prov);
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
            /* the causal rule is this authored step rule's effect — carry its
             * source span (generated inertia's own provenance is a later slice) */
            if (r->prov)
                dlcol_set_prov(f, rid, r->prov);
            /* effect f' conflicts with inertia-f ; effect ~f' with inertia+f */
            dlcol_add_sup(f, rid, eff.neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
    }
    if (body != lbuf)
        free(body);
    free(inertia_pos);
    free(inertia_neg);

    w->fam = f;
}

/* Rebuild both families when the theory structure is stale (a rule or fluent
 * was added). Facts are set later — per step for `fam`, per query for `jfam`. */
static void ensure_families(world *w)
{
    if (w->fam && w->jfam && !w->fam_dirty)
        return;
    assign_locs(w);
    emit_step_family(w);
    emit_judgment_family(w);
    w->fam_dirty = false;
    w->jfam_solved = false;
}

/* Load current-state facts (closed-world fluents + numeric guard atoms) into the
 * judgment family and solve it — the columnar analog of the scalar theory
 * world_query used to rebuild per call. Cached until a state edit (jfam_solved). */
static void solve_judgment_family(world *w)
{
    dlcol *f = w->jfam;
    dlcol_clear_facts(f);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit l = { w->fl_loc[i], !w->vals[i] };   /* f if true, ~f if false */
        dlcol_add_fact(f, l, 0);
    }
    for (int g = 0; g < w->ng; g++) {
        uint32_t ga = w->guards[g].guard;
        if (ga < w->loc_cap && w->loc_of[ga] != LOC_NONE) {
            dl_lit l = { w->loc_of[ga], !guard_holds(w, g) };
            dlcol_add_fact(f, l, 0);
        }
    }
    dlcol_solve(f);
    w->jfam_solved = true;
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

/* Load a state (closed-world fluents from `vals` + numeric guard atoms) and the
 * occurring actions into the N=1 step family, and solve it. Shared by world_step
 * and world_step_lanes_check (the latter reads the primed columns without
 * committing), and by world_step_why replaying a routed step's snapshot. An
 * action atom no rule mentions is semantically inert; skip it. */
static void solve_step_family_vals(world *w, const bool *vals,
                                   const uint32_t *actions, int nactions)
{
    dlcol *f = w->fam;
    dlcol_clear_facts(f);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit l = { w->fl_loc[i], !vals[i] };
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
}

static void solve_step_family(world *w, const uint32_t *actions, int nactions)
{
    solve_step_family_vals(w, w->vals, actions, nactions);
}

/* Snapshot the pre-step state + actions so a subsequent world_step_why can
 * replay a routed transition on the N=1 family (which the fast path skips). */
static void save_step_snapshot(world *w, const uint32_t *actions, int nactions)
{
    if (w->step_snap_cap < w->nfl) {
        w->step_snap = realloc(w->step_snap, (size_t)w->nfl * sizeof *w->step_snap);
        w->step_snap_cap = w->nfl;
    }
    if (w->nfl)
        memcpy(w->step_snap, w->vals, (size_t)w->nfl * sizeof *w->vals);
    if (w->last_actions_cap < nactions) {
        w->last_actions = realloc(w->last_actions,
                                  (size_t)nactions * sizeof *w->last_actions);
        w->last_actions_cap = nactions;
    }
    if (nactions)
        memcpy(w->last_actions, actions, (size_t)nactions * sizeof *actions);
    w->last_nactions = nactions;
}

/* The routed transition: solve the step lane family bit-parallel and commit the
 * next state per lane, instead of the N=1 per-named-atom path. Contested/undecided
 * next values are an authoring error — return -1 without mutating (as N=1 does).
 * Precondition: exactly one step lane family covering every fluent, no numerics. */
static int world_step_lanes(world *w, const uint32_t *actions, int nactions,
                            char *err, size_t errsz)
{
    step_lane_family *sf = &w->steplanes[0];
    solve_step_lane_family(w, sf, actions, nactions);

    int rc = 0;
    bool *next = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *next);
    for (int i = 0; i < w->nfl; i++) next[i] = w->vals[i];   /* full coverage anyway */
    for (int a = 0; a < sf->nloc && rc == 0; a++) {
        if (sf->kind[a] != WORLD_STEP_PRIMED) continue;
        for (int e = 0; e < sf->nent; e++) {
            int i = sf->fl_of[(size_t)a * sf->nent + e];
            if (i < 0) continue;
            dl_lit p = { (uint32_t)a, false };
            if (dlcol_defeasible(sf->fam, p, e) == DL_PROVED) {
                next[i] = true;
            } else if (dlcol_defeasible(sf->fam, dl_complement(p), e) == DL_PROVED) {
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
    }

    if (rc == 0) {
        save_step_snapshot(w, actions, nactions);   /* before overwriting vals */
        if (w->nfl)
            memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);
        invalidate_state_solved(w);
        w->last_routed = true;
    }
    free(next);
    return rc;
}

int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz)
{
    ensure_families(w);

    /* the hot path: a homogeneous boolean step world lanes its whole transition,
     * so solve it bit-parallel across entities. (w->lanes_ok guards post-compile
     * structural edits; the step lanes only ever form when nnum==0.) */
    if (w->lanes_ok && w->nsteplanes == 1 && w->nnum == 0)
        return world_step_lanes(w, actions, nactions, err, errsz);

    dlcol *f = w->fam;

    solve_step_family(w, actions, nactions);

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
    /* One pass over the step rules — NOT nnum × nsr (that double scan, matching
     * every fluent against every rule's effects, was the O(N²) crowd wall). Each
     * fired numeric effect routes to its fluent's accumulator via the O(1) num
     * index; a second pass over the fluents runs the pipeline (base + Σ deltas,
     * clamp) and finishes the receipts. Total is O(nsr + effects + nnum). */
    if (rc == 0 && w->nnum > 0) {
        struct nacc { long delta, assign_val; const char *rule;
                      bool have, conflict; } *acc =
            calloc((size_t)w->nnum, sizeof *acc);
        for (int i = 0; i < w->nnum; i++) w->rcpt[i].n = 0;

        for (int s = 0; s < w->nsr; s++) {
            const srule *r = &w->srules[s];
            if (r->nneff == 0 || !srule_fired(w, r, actions, nactions))
                continue;
            for (int e = 0; e < r->nneff; e++) {
                const num_effect *ef = &r->neffs[e];
                int i = num_index(w, ef->num_atom);
                if (i < 0) continue;
                long v = eval_expr(w, ef->code, ef->ncode);
                if (ef->op == WORLD_OP_ASSIGN) {
                    if (acc[i].have) {
                        if (v != acc[i].assign_val) acc[i].conflict = true;
                    } else {
                        acc[i].have = true;
                        acc[i].assign_val = v;
                        acc[i].rule = r->name;
                    }
                } else {
                    long d = (ef->op == WORLD_OP_ADD) ? v : -v;
                    acc[i].delta += d;
                    rcpt_push(&w->rcpt[i], r->name, ef->op, d);
                }
            }
        }

        for (int i = 0; rc == 0 && i < w->nnum; i++) {
            num_receipt *rcp = &w->rcpt[i];
            if (acc[i].conflict) {
                if (err)
                    snprintf(err, errsz,
                             "conflicting `:=` effects on numeric fluent '%s'",
                             intern_name(w->syms, w->nums[i].atom));
                rc = -1;
                break;
            }
            /* receipt order: winning assign first, then the deltas in scan order */
            if (acc[i].have) {
                rcpt_push(rcp, NULL, WORLD_OP_ASSIGN, 0);   /* grow, then shift */
                memmove(&rcp->items[1], &rcp->items[0],
                        (size_t)(rcp->n - 1) * sizeof *rcp->items);
                rcp->items[0].rule = acc[i].rule;
                rcp->items[0].op = WORLD_OP_ASSIGN;
                rcp->items[0].amount = acc[i].assign_val;
            }
            long base = acc[i].have ? acc[i].assign_val : w->nums[i].value;
            rcp->base = base;
            long val = base + acc[i].delta;
            if (w->nums[i].has_range) {
                if (val < w->nums[i].min) val = w->nums[i].min;
                if (val > w->nums[i].max) val = w->nums[i].max;
            }
            nextnum[i] = val;
        }
        free(acc);
    }

    if (rc == 0) {
        if (w->nfl)
            memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);
        for (int i = 0; i < w->nnum; i++)
            w->nums[i].value = nextnum[i];
        invalidate_state_solved(w);                /* new state: judgments stale */
        w->last_routed = false;                    /* w->fam holds this transition */
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
