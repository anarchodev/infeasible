#include "logic/dl.h"
#include "logic/dl_trace.h"
#include "logic/dl_graph.h"
#include "core/arena.h"
#include "core/grow.h"

#include <stdlib.h>
#include <string.h>

/* ---- builder-side theory (append-friendly AoS; not walked during solve) ----
 *
 * dl_solve compiles this into the struct-of-arrays, head-sorted form below,
 * which is what the fixpoint actually reads (see DESIGN.md 5.2: the theory is
 * the source form, the compiled form is the execution form). */

typedef struct {
    const char *name;
    const char *prov;    /* provenance suffix (§6.3), or NULL */
    dl_rule_kind kind;
    dl_lit head;
    dl_lit *body;
    int nbody;
} rule;

typedef struct { int winner, loser; } sup;

struct dl_theory {
    arena a;
    intern *syms;
    rule *rules; int nrules, caprules;
    sup  *sups;  int nsups,  capsups;
    dl_lit *facts; int nfacts, capfacts;
};

/* Literals are addressed by a dense index atom*2 + neg throughout the compiled
 * form and the status arrays; the low bit is the sign, so complement == idx^1. */
static int lit_idx(dl_lit l) { return (int)l.atom * 2 + (l.neg ? 1 : 0); }
static dl_lit lit_from_idx(int i) { dl_lit l = { (uint32_t)(i >> 1), (i & 1) != 0 }; return l; }

struct dl_result {
    int nlits;
    signed char *delta;  /* dl_verdict per literal index (+/-Delta)  */
    signed char *part;   /* dl_verdict per literal index (+/-partial) */
    bool *is_fact;

    /* Compiled rules, permuted into head-literal order so that head_off[]
     * ranges index the rule arrays directly (no per-rule gather) and one
     * head's rules — with their bodies — are contiguous. Hot fields only;
     * `rname` is cold, touched solely by dl_why. */
    int nrules;
    uint8_t     *rkind;      /* [nrules]     dl_rule_kind, 1 byte           */
    int32_t     *rhead;      /* [nrules]     head lit index                 */
    int32_t     *rbody_off;  /* [nrules+1]   slices into body[]             */
    int32_t     *body;       /* [total_body] precomputed lit indices        */
    const char **rname;      /* [nrules]     cold                           */
    const char **rprov;      /* [nrules]     cold; provenance suffix or NULL */
    int32_t     *head_off;   /* [nlits+1]    ranges into the permuted rules */

    /* Superiority as a reverse index: beat_by[beat_off[s]..beat_off[s+1]) are
     * the rules that beat rule s. Replaces the O(nsups) linear beats() scan. */
    int32_t *beat_off;       /* [nrules+1] */
    int32_t *beat_by;        /* [nsups]    */

    /* Dependency index for worklist evaluation: dep_to[dep_off[b]..dep_off[b+1])
     * are the literals whose status can change once literal b's status is
     * decided (i.e. b appears in the body of a rule whose head or its
     * complement is that literal). Lets the fixpoints re-touch only what a new
     * decision can affect instead of rescanning every literal each sweep. */
    int32_t *dep_off;        /* [nlits+1]      */
    int32_t *dep_to;         /* [2*total_body] */

    /* #109 cycle rule (§5.2): SCC condensation of the SUPPORT graph (body ->
     * head over strict + defeasible rules; defeaters attack, never support).
     * scc_cyclic marks size>1 or self-loop; scc_attacked marks an SCC some
     * rule (any kind, defeaters included) concludes a member's complement
     * into. `completed` records literals the completion pass refuted, for the
     * why-trace's loop line. */
    int32_t *scc;            /* [nlits] SCC id */
    int32_t *scc_off;        /* [nscc+1] members, grouped */
    int32_t *scc_lit;        /* [nlits]  member literal indices */
    bool    *scc_cyclic;     /* [nscc] */
    bool    *scc_attacked;   /* [nscc] */
    bool    *completed;      /* [nlits] */
    int      nscc;

    /* §5.2 item 2: the evaluator's SCC schedule — a condensation of the
     * DEPENDENCY graph (dep_to above), which is a different and strictly
     * larger graph than the support one: an attacker's body decides its
     * target too, so `body -> head^1` edges belong here and not in the
     * cycle rule's support graph. Components carry Tarjan's numbering, which
     * is reverse topological, so evaluating them in DESCENDING id order
     * visits every literal after the literals it reads. */
    int32_t *sched_of;       /* [nlits]    component id */
    int32_t *sched_off;      /* [nsched+1] members, grouped */
    int32_t *sched_lit;      /* [nlits]    member literal indices */
    int      nsched;
};

dl_theory *dl_theory_new(intern *syms)
{
    dl_theory *t = calloc(1, sizeof *t);
    arena_init(&t->a);
    t->syms = syms;
    return t;
}

void dl_theory_free(dl_theory *t)
{
    arena_release(&t->a);
    free(t->rules);
    free(t->sups);
    free(t->facts);
    free(t);
}

int dl_add_rule(dl_theory *t, const char *name, dl_rule_kind kind,
                dl_lit head, const dl_lit *body, int nbody)
{
    GROW(t->rules, t->nrules, t->caprules);
    rule *r = &t->rules[t->nrules];
    r->name = arena_strdup(&t->a, name ? name : "?");
    r->prov = NULL;
    r->kind = kind;
    r->head = head;
    r->nbody = nbody;
    r->body = arena_alloc(&t->a, (size_t)(nbody ? nbody : 1) * sizeof(dl_lit));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(dl_lit));
    return t->nrules++;
}

void dl_set_prov(dl_theory *t, int rule_id, const char *prov)
{
    if (rule_id >= 0 && rule_id < t->nrules)
        t->rules[rule_id].prov = prov ? arena_strdup(&t->a, prov) : NULL;
}

void dl_add_sup(dl_theory *t, int winner, int loser)
{
    GROW(t->sups, t->nsups, t->capsups);
    t->sups[t->nsups].winner = winner;
    t->sups[t->nsups].loser = loser;
    t->nsups++;
}

void dl_add_fact(dl_theory *t, dl_lit fact)
{
    GROW(t->facts, t->nfacts, t->capfacts);
    t->facts[t->nfacts++] = fact;
}

/* ---- tri-valued helpers: -1 false, 0 unknown, +1 true ---- */

static int ts_min(int a, int b) { return a < b ? a : b; }
static int ts_max(int a, int b) { return a > b ? a : b; }

static int ts_of(signed char v, dl_verdict want)
{
    if (v == (signed char)want)
        return 1;
    return v == DL_UNDECIDED ? 0 : -1;
}

/* Walk one head literal's rules: [head_off[qi], head_off[qi+1]) index the
 * permuted rule arrays directly, so `ri` IS the loop variable. */
#define FOR_HEAD_RULES(res, qi, var) \
    for (int _hi = (res)->head_off[qi]; _hi < (res)->head_off[(qi) + 1] && ((var) = _hi, 1); _hi++)

/* AND over body of +d(head of ri): +1 iff every body literal is +partial,
 * -1 iff some body literal is -partial. */
static int ts_applicable(const dl_result *res, int ri)
{
    int acc = 1;
    for (int i = res->rbody_off[ri]; i < res->rbody_off[ri + 1]; i++) {
        acc = ts_min(acc, ts_of(res->part[res->body[i]], DL_PROVED));
        if (acc == -1)
            break;
    }
    return acc;
}

/* supported(q): some strict/defeasible rule for q is applicable */
static int ts_supported(const dl_result *res, int qi)
{
    int acc = -1, ri;
    FOR_HEAD_RULES(res, qi, ri) {
        if (res->rkind[ri] == DL_DEFEATER)
            continue;
        acc = ts_max(acc, ts_applicable(res, ri));
        if (acc == 1)
            break;
    }
    return acc;
}

/* notsupported(q): every strict/defeasible rule for q has a -d body literal */
static int ts_notsupported(const dl_result *res, int qi)
{
    int acc = 1, ri;
    FOR_HEAD_RULES(res, qi, ri) {
        if (res->rkind[ri] == DL_DEFEATER)
            continue;
        acc = ts_min(acc, -ts_applicable(res, ri));
        if (acc == -1)
            break;
    }
    return acc;
}

/* Team defeat: max applicability over the non-defeater rules for qi that beat
 * rule si. The candidates come straight from the superiority reverse index
 * (winners over si), filtered to those whose head is qi — far fewer than the
 * old "scan every rule for q and test beats()". */
static int ts_beaten_by_supporter(const dl_result *res, int si, int qi)
{
    int acc = -1;
    for (int k = res->beat_off[si]; k < res->beat_off[si + 1]; k++) {
        int ti = res->beat_by[k];
        if (res->rkind[ti] == DL_DEFEATER || res->rhead[ti] != qi)
            continue;
        acc = ts_max(acc, ts_applicable(res, ti));
        if (acc == 1)
            break;
    }
    return acc;
}

/* countered(s, q): attacker s (a rule for ~q) is inapplicable, or beaten by
 * some applicable strict/defeasible rule for q (team defeat). */
static int ts_countered(const dl_result *res, int si, int qi)
{
    return ts_max(-ts_applicable(res, si), ts_beaten_by_supporter(res, si, qi));
}

static int ts_all_attackers_countered(const dl_result *res, int qi, int nqi)
{
    int acc = 1, si;
    FOR_HEAD_RULES(res, nqi, si) {
        acc = ts_min(acc, ts_countered(res, si, qi));
        if (acc == -1)
            break;
    }
    return acc;
}

/* uncountered attacker exists: some rule for ~q is applicable and no applicable
 * strict/defeasible rule for q beats it. */
static int ts_attacker_uncountered(const dl_result *res, int qi, int nqi)
{
    int acc = -1, si;
    FOR_HEAD_RULES(res, nqi, si) {
        int this = ts_min(ts_applicable(res, si), -ts_beaten_by_supporter(res, si, qi));
        acc = ts_max(acc, this);
        if (acc == 1)
            break;
    }
    return acc;
}

/* ---- solve ----
 *
 * Each eval_* decides one literal's status if it can now be decided, returning
 * true when it just moved from UNDECIDED. Statuses are monotone (they never
 * revert). Two drivers share these evals and reach the same fixpoint:
 *
 *  - sweep()        the default. Rescans every literal until a pass makes no
 *                   change; a decided literal is skipped with one byte read, so
 *                   a pass is a sequential scan of the status array. Fastest on
 *                   the dense, shallow theories the scene tier recomputes (few
 *                   passes), but O(passes * nlits) where passes tracks the
 *                   longest dependency chain in scan order (see bench_dl).
 *  - run_worklist() order-independent: a decision re-queues only its dependents
 *                   (dep_to), so work is proportional to decisions, not
 *                   nlits per pass. No ordering cliff, and the substrate for
 *                   incremental global-tier re-solve (DESIGN 4.1). Carries a
 *                   constant-factor tax (dep-index build + random-access
 *                   re-evaluation) that regresses the dense case; retained
 *                   behind dl_solve_wl until a counter-based eval recovers it. */

/* +Delta / -Delta for one literal (strict rules only). */
static bool eval_delta(const dl_result *res, int qi)
{
    if (res->is_fact[qi]) {
        res->delta[qi] = DL_PROVED;
        return true;
    }
    bool proved = false, alive = false;
    int ri;
    FOR_HEAD_RULES(res, qi, ri) {
        if (res->rkind[ri] != DL_STRICT)
            continue;
        bool all = true, dead = false;
        for (int i = res->rbody_off[ri]; i < res->rbody_off[ri + 1]; i++) {
            signed char v = res->delta[res->body[i]];
            if (v != DL_PROVED)
                all = false;
            if (v == DL_REFUTED)
                dead = true;
        }
        if (all) {
            proved = true;
            break;
        }
        if (!dead)
            alive = true;
    }
    if (proved) {
        res->delta[qi] = DL_PROVED;
        return true;
    }
    if (!alive) {
        res->delta[qi] = DL_REFUTED;
        return true;
    }
    return false;
}

/* +partial / -partial for one literal. */
static bool eval_part(const dl_result *res, int qi)
{
    int nqi = qi ^ 1;
    /* +d q */
    int pos = ts_of(res->delta[qi], DL_PROVED);
    if (pos != 1) {
        int alt = ts_of(res->delta[nqi], DL_REFUTED);
        alt = ts_min(alt, ts_supported(res, qi));
        alt = ts_min(alt, ts_all_attackers_countered(res, qi, nqi));
        pos = ts_max(pos, alt);
    }
    if (pos == 1) {
        res->part[qi] = DL_PROVED;
        return true;
    }
    /* -d q */
    int negv = ts_of(res->delta[qi], DL_REFUTED);
    if (negv != -1) {
        int inner = ts_of(res->delta[nqi], DL_PROVED);
        inner = ts_max(inner, ts_notsupported(res, qi));
        inner = ts_max(inner, ts_attacker_uncountered(res, qi, nqi));
        negv = ts_min(negv, inner);
    }
    if (negv == 1) {
        res->part[qi] = DL_REFUTED;
        return true;
    }
    return false;
}

/* Default driver: rescan to fixpoint. */
/* ---- #109 cycle rule (§5.2): SCC condensation + Datalog completion ------
 *
 * Build the support-graph SCCs once per solve (iterative Tarjan over literal
 * nodes; edges body -> head for every strict/defeasible rule). A cyclic SCC
 * whose members are attacked NOWHERE (no rule of any kind concludes a
 * member's complement) is plain Datalog: after the monotone fixpoint stalls,
 * every still-UNDECIDED member whose stall is purely in-SCC completes to
 * REFUTED. The gate that keeps this sound: if any member's rule reads an
 * OUT-of-SCC literal that is itself UNDECIDED (a tied attack elsewhere, an
 * open #116 guard), the whole SCC stays undecided — completion only ever
 * consumes decided inputs, so the two causes of UNDECIDED never blur.
 * Attacked cycles are untouched here (the language layer rejects them at
 * compile time; hand-built theories keep today's honest stall). */

static void build_scc(dl_result *res)
{
    int n = res->nlits;
    res->scc = malloc((size_t)(n ? n : 1) * sizeof *res->scc);
    res->completed = calloc((size_t)(n ? n : 1), 1);

    /* adjacency: body-literal -> head-literal, support rules only */
    int nedge = 0;
    for (int r = 0; r < res->nrules; r++)
        if (res->rkind[r] != DL_DEFEATER)
            nedge += res->rbody_off[r + 1] - res->rbody_off[r];
    int32_t *aoff = calloc((size_t)n + 2, sizeof *aoff);
    int32_t *ato = malloc((size_t)(nedge ? nedge : 1) * sizeof *ato);
    for (int r = 0; r < res->nrules; r++) {
        if (res->rkind[r] == DL_DEFEATER) continue;
        for (int b = res->rbody_off[r]; b < res->rbody_off[r + 1]; b++)
            aoff[res->body[b] + 1]++;
    }
    for (int i = 0; i < n; i++) aoff[i + 1] += aoff[i];
    int32_t *fill = malloc((size_t)(n ? n : 1) * sizeof *fill);
    memcpy(fill, aoff, (size_t)n * sizeof *fill);
    for (int r = 0; r < res->nrules; r++) {
        if (res->rkind[r] == DL_DEFEATER) continue;
        for (int b = res->rbody_off[r]; b < res->rbody_off[r + 1]; b++)
            ato[fill[res->body[b]]++] = res->rhead[r];
    }
    free(fill);

    int nscc = dl_tarjan(n, aoff, ato, res->scc);
    res->nscc = nscc;
    dl_group_by_comp(n, nscc, res->scc, &res->scc_off, &res->scc_lit);

    /* cyclic: size > 1, or a self-loop edge */
    res->scc_cyclic = calloc((size_t)(nscc ? nscc : 1), 1);
    for (int c = 0; c < nscc; c++)
        if (res->scc_off[c + 1] - res->scc_off[c] > 1)
            res->scc_cyclic[c] = true;
    for (int i = 0; i < n && nedge; i++)
        for (int k = aoff[i]; k < aoff[i + 1]; k++)
            if (ato[k] == i) res->scc_cyclic[res->scc[i]] = true;

    /* attacked: some rule (defeaters included) concludes a member's complement */
    res->scc_attacked = calloc((size_t)(nscc ? nscc : 1), 1);
    for (int r = 0; r < res->nrules; r++) {
        int comp = res->rhead[r] ^ 1;
        res->scc_attacked[res->scc[comp]] = true;
    }
    free(aoff); free(ato);
}

/* One completion pass over `status` (delta or part). `strict_only` marks the
 * delta layer, where only strict rules support (a defeasible rule's stalled
 * body must not block a -Delta completion it cannot influence). Returns true
 * if any literal was refuted — the caller re-runs the sweep so the new
 * decisions propagate (which can in turn unlock further SCCs). */
static bool complete_pass(dl_result *res, signed char *status, bool strict_only)
{
    bool any = false;
    for (int c = 0; c < res->nscc; c++) {
        if (!res->scc_cyclic[c] || res->scc_attacked[c]) continue;
        bool stalled = false, blocked = false;
        for (int k = res->scc_off[c]; k < res->scc_off[c + 1] && !blocked; k++) {
            int m = res->scc_lit[k], ri;
            if (status[m] == DL_UNDECIDED) stalled = true;
            FOR_HEAD_RULES(res, m, ri) {
                if (res->rkind[ri] == DL_DEFEATER) continue;
                if (strict_only && res->rkind[ri] != DL_STRICT) continue;
                for (int b = res->rbody_off[ri]; b < res->rbody_off[ri + 1]; b++) {
                    int bl = res->body[b];
                    if (res->scc[bl] != c && status[bl] == DL_UNDECIDED)
                        { blocked = true; break; }   /* undecided INPUT: honest
                                                      * stall, never complete */
                }
            }
        }
        if (blocked || !stalled) continue;
        for (int k = res->scc_off[c]; k < res->scc_off[c + 1]; k++) {
            int m = res->scc_lit[k];
            if (status[m] == DL_UNDECIDED) {
                status[m] = DL_REFUTED;
                res->completed[m] = true;
                any = true;
            }
        }
    }
    return any;
}

static void sweep(const dl_result *res, signed char *status,
                  bool (*eval)(const dl_result *, int))
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (int qi = 0; qi < res->nlits; qi++)
            if (status[qi] == DL_UNDECIDED && eval(res, qi))
                changed = true;
    }
}

/* SCC-ordered ("weak-topological") sweep — DESIGN.md §5.2 item 2.
 *
 * Walk the dependency condensation in topological order (descending component
 * id, see build_sched) and iterate only INSIDE a component. Every literal is
 * therefore evaluated after everything it reads is final, so an acyclic theory
 * decides in exactly one ordered pass — the plain sweep's O(passes * nlits)
 * scan-order cliff (passes ~ the longest chain running against index order)
 * disappears without the worklist's random-access dependency-chasing.
 *
 * Same least fixpoint as sweep(): the evals are monotone in the statuses they
 * read, so iterating a component to a local fixpoint over final inputs yields
 * that component's final values (chaotic iteration). A singleton needs exactly
 * one eval even when it carries a self-loop: a second call would see identical
 * inputs and an unchanged status, so it can only repeat the first answer. */
static void sweep_scc(const dl_result *res, signed char *status,
                      bool (*eval)(const dl_result *, int))
{
    for (int c = res->nsched - 1; c >= 0; c--) {
        int lo = res->sched_off[c], hi = res->sched_off[c + 1];
        if (hi - lo == 1) {
            int qi = res->sched_lit[lo];
            if (status[qi] == DL_UNDECIDED)
                eval(res, qi);
            continue;
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (int k = lo; k < hi; k++) {
                int qi = res->sched_lit[k];
                if (status[qi] == DL_UNDECIDED && eval(res, qi))
                    changed = true;
            }
        }
    }
}

/* Drive one status array (delta or part) to fixpoint. `status` is the array the
 * eval writes; seeding every literal once covers first evaluation, and each
 * decision re-queues only its dependents. Requires build_dep(). A literal is
 * queued at most once at a time (the `inq` guard), so the stack never exceeds
 * nlits. */
static void run_worklist(const dl_result *res, signed char *status,
                         bool (*eval)(const dl_result *, int),
                         int *stack, bool *inq)
{
    int nlits = res->nlits, top = 0;
    for (int i = 0; i < nlits; i++) {
        stack[i] = i;
        inq[i] = true;
    }
    top = nlits;

    while (top > 0) {
        int qi = stack[--top];
        inq[qi] = false;
        if (status[qi] != DL_UNDECIDED)
            continue;
        if (!eval(res, qi))
            continue;
        for (int k = res->dep_off[qi]; k < res->dep_off[qi + 1]; k++) {
            int x = res->dep_to[k];
            if (status[x] == DL_UNDECIDED && !inq[x]) {
                stack[top++] = x;
                inq[x] = true;
            }
        }
    }
}

/* Compile the theory into the head-sorted SoA form solve/why read. */
static void compile(const dl_theory *t, dl_result *res)
{
    int nlits = res->nlits, nrules = t->nrules;
    res->nrules = nrules;

    /* Counting sort of rules by head lit index -> head_off ranges + the
     * new-index permutation. head_off[h+1] first holds the per-head count. */
    res->head_off = calloc((size_t)nlits + 1, sizeof *res->head_off);
    for (int r = 0; r < nrules; r++)
        res->head_off[lit_idx(t->rules[r].head) + 1]++;
    for (int i = 0; i < nlits; i++)
        res->head_off[i + 1] += res->head_off[i];

    int *new_of_old = malloc((size_t)(nrules ? nrules : 1) * sizeof *new_of_old);
    int *fill = calloc((size_t)nlits, sizeof *fill);
    for (int r = 0; r < nrules; r++) {
        int h = lit_idx(t->rules[r].head);
        new_of_old[r] = res->head_off[h] + fill[h]++;
    }
    free(fill);

    /* Emit compiled arrays in permuted order; flatten bodies into one slab. */
    res->rkind     = malloc((size_t)(nrules ? nrules : 1) * sizeof *res->rkind);
    res->rhead     = malloc((size_t)(nrules ? nrules : 1) * sizeof *res->rhead);
    res->rname     = malloc((size_t)(nrules ? nrules : 1) * sizeof *res->rname);
    res->rprov     = malloc((size_t)(nrules ? nrules : 1) * sizeof *res->rprov);
    res->rbody_off = malloc(((size_t)nrules + 1) * sizeof *res->rbody_off);

    int total_body = 0;
    for (int r = 0; r < nrules; r++)
        total_body += t->rules[r].nbody;
    res->body = malloc((size_t)(total_body ? total_body : 1) * sizeof *res->body);

    /* Body offsets: first lay down each permuted rule's length, then prefix-sum. */
    res->rbody_off[0] = 0;
    for (int r = 0; r < nrules; r++)
        res->rbody_off[new_of_old[r] + 1] = t->rules[r].nbody;
    for (int n = 0; n < nrules; n++)
        res->rbody_off[n + 1] += res->rbody_off[n];

    for (int r = 0; r < nrules; r++) {
        const rule *src = &t->rules[r];
        int n = new_of_old[r];
        res->rkind[n] = (uint8_t)src->kind;
        res->rhead[n] = (int32_t)lit_idx(src->head);
        res->rname[n] = src->name;
        res->rprov[n] = src->prov;
        int32_t off = res->rbody_off[n];
        for (int i = 0; i < src->nbody; i++)
            res->body[off + i] = (int32_t)lit_idx(src->body[i]);
    }

    /* Superiority reverse index, in permuted rule ids: beat_off/beat_by group
     * winners by their loser. */
    res->beat_off = calloc((size_t)nrules + 1, sizeof *res->beat_off);
    res->beat_by  = malloc((size_t)(t->nsups ? t->nsups : 1) * sizeof *res->beat_by);
    for (int s = 0; s < t->nsups; s++)
        res->beat_off[new_of_old[t->sups[s].loser] + 1]++;
    for (int n = 0; n < nrules; n++)
        res->beat_off[n + 1] += res->beat_off[n];
    int *bfill = calloc((size_t)(nrules ? nrules : 1), sizeof *bfill);
    for (int s = 0; s < t->nsups; s++) {
        int loser = new_of_old[t->sups[s].loser];
        res->beat_by[res->beat_off[loser] + bfill[loser]++] =
            (int32_t)new_of_old[t->sups[s].winner];
    }
    free(bfill);
    free(new_of_old);
}

/* Build the dependency index the worklist driver needs. Only dl_solve_wl calls
 * this; the default sweep path never pays for it. For every body literal b of a
 * rule with head h, a new decision on b can change the status of h and its
 * complement h^1 (b is consulted both when supporting h and when attacking h^1).
 * Two edges per body-literal occurrence, grouped by source literal b. */
static void build_dep(dl_result *res)
{
    int nlits = res->nlits, nrules = res->nrules;
    res->dep_off = calloc((size_t)nlits + 1, sizeof *res->dep_off);
    for (int r = 0; r < nrules; r++)
        for (int i = res->rbody_off[r]; i < res->rbody_off[r + 1]; i++)
            res->dep_off[res->body[i] + 1] += 2;
    for (int i = 0; i < nlits; i++)
        res->dep_off[i + 1] += res->dep_off[i];
    int ndep = res->dep_off[nlits];
    res->dep_to = malloc((size_t)(ndep ? ndep : 1) * sizeof *res->dep_to);
    int *dfill = calloc((size_t)nlits, sizeof *dfill);
    for (int r = 0; r < nrules; r++) {
        int h = res->rhead[r];
        for (int i = res->rbody_off[r]; i < res->rbody_off[r + 1]; i++) {
            int b = res->body[i];
            res->dep_to[res->dep_off[b] + dfill[b]++] = h;
            res->dep_to[res->dep_off[b] + dfill[b]++] = h ^ 1;
        }
    }
    free(dfill);
}

/* Condense the dependency graph into the evaluator's schedule (§5.2 item 2).
 * The adjacency is dep_off/dep_to itself — the graph a decision propagates
 * along is exactly the graph the sweep must be ordered by — so this is one
 * Tarjan pass on top of build_dep(), which it requires. */
static void build_sched(dl_result *res)
{
    int n = res->nlits;
    res->sched_of = malloc((size_t)(n ? n : 1) * sizeof *res->sched_of);
    res->nsched = dl_tarjan(n, res->dep_off, res->dep_to, res->sched_of);
    dl_group_by_comp(n, res->nsched, res->sched_of, &res->sched_off, &res->sched_lit);
}

static dl_result *result_new(dl_theory *t)
{
    dl_result *res = calloc(1, sizeof *res);
    res->nlits = (int)intern_count(t->syms) * 2;
    res->delta = calloc((size_t)res->nlits, 1);
    res->part = calloc((size_t)res->nlits, 1);
    res->is_fact = calloc((size_t)res->nlits, 1);
    for (int i = 0; i < t->nfacts; i++)
        res->is_fact[lit_idx(t->facts[i])] = true;
    compile(t, res);
    return res;
}

dl_result *dl_solve(dl_theory *t)
{
    dl_result *res = result_new(t);
    build_scc(res);                      /* #109: cycle rule support */
    do sweep(res, res->delta, eval_delta);  /* delta first: constant during part */
    while (complete_pass(res, res->delta, true));
    do sweep(res, res->part, eval_part);
    while (complete_pass(res, res->part, false));
    return res;
}

/* SCC-ordered sweep (§5.2 item 2) behind the same API. Same fixpoint as
 * dl_solve, reached by scheduling rather than rescanning; pinned against both
 * other drivers by test_dl's differential. */
dl_result *dl_solve_scc(dl_theory *t)
{
    dl_result *res = result_new(t);
    build_scc(res);                      /* #109: cycle rule support */
    build_dep(res);
    build_sched(res);
    do sweep_scc(res, res->delta, eval_delta);
    while (complete_pass(res, res->delta, true));
    do sweep_scc(res, res->part, eval_part);
    while (complete_pass(res, res->part, false));
    return res;
}

/* Experimental order-independent driver; see the solve-section comment and
 * DESIGN.md 5.2. Not the default: it regresses the dense scene-tier workload
 * pending a counter-based eval. Kept exercised by test_dl's differential check
 * and reachable from bench_dl. */
dl_result *dl_solve_wl(dl_theory *t)
{
    dl_result *res = result_new(t);
    build_scc(res);                      /* #109: cycle rule support */
    build_dep(res);
    int  *stack = malloc((size_t)(res->nlits ? res->nlits : 1) * sizeof *stack);
    bool *inq   = calloc((size_t)(res->nlits ? res->nlits : 1), sizeof *inq);
    do run_worklist(res, res->delta, eval_delta, stack, inq);
    while (complete_pass(res, res->delta, true));
    do run_worklist(res, res->part, eval_part, stack, inq);
    while (complete_pass(res, res->part, false));
    free(stack);
    free(inq);
    return res;
}

void dl_result_free(dl_result *r)
{
    free(r->delta);
    free(r->part);
    free(r->is_fact);
    free(r->rkind);
    free(r->rhead);
    free(r->rbody_off);
    free(r->body);
    free(r->rname);
    free(r->rprov);
    free(r->head_off);
    free(r->beat_off);
    free(r->beat_by);
    free(r->dep_off);
    free(r->dep_to);
    free(r->scc);
    free(r->scc_off);
    free(r->scc_lit);
    free(r->scc_cyclic);
    free(r->scc_attacked);
    free(r->completed);
    free(r->sched_of);
    free(r->sched_off);
    free(r->sched_lit);
    free(r);
}

dl_verdict dl_definite(const dl_result *r, dl_lit q)
{
    int qi = lit_idx(q);
    return qi < r->nlits ? (dl_verdict)r->delta[qi] : DL_UNDECIDED;
}

dl_verdict dl_defeasible(const dl_result *r, dl_lit q)
{
    int qi = lit_idx(q);
    return qi < r->nlits ? (dl_verdict)r->part[qi] : DL_UNDECIDED;
}

/* ---- why-trace ---- */

static bool beats_c(const dl_result *res, int winner, int loser)
{
    for (int k = res->beat_off[loser]; k < res->beat_off[loser + 1]; k++)
        if (res->beat_by[k] == winner)
            return true;
    return false;
}

/* ---- why trace: an adapter over the shared renderer (dl_trace.h) ----
 *
 * The scalar result addresses head rules by their permuted index (head_off[qi]
 * .. head_off[qi+1] index the rule arrays directly), so a trace "rule handle" is
 * just that index. The columnar backing supplies the same vtable over its
 * columns; the format is shared, so the two stay identical by construction. */

typedef struct { const dl_theory *t; const dl_result *res; } scalar_trace;

static void sc_put_lit(void *ctx, dl_lit l, FILE *out)
{
    const scalar_trace *s = ctx;
    fprintf(out, "%s%s", l.neg ? "~" : "", intern_name(s->t->syms, l.atom));
}
static void sc_put_rule(void *ctx, int r, FILE *out)
{
    fprintf(out, "%s", ((const scalar_trace *)ctx)->res->rname[r]);
}
static dl_verdict sc_definite(void *ctx, dl_lit l)
{ return dl_definite(((const scalar_trace *)ctx)->res, l); }
static dl_verdict sc_defeasible(void *ctx, dl_lit l)
{ return dl_defeasible(((const scalar_trace *)ctx)->res, l); }
static bool sc_is_fact(void *ctx, dl_lit l)
{ return ((const scalar_trace *)ctx)->res->is_fact[lit_idx(l)]; }
static bool sc_solved(void *ctx) { (void)ctx; return true; }
static int sc_nhead(void *ctx, dl_lit l)
{
    const dl_result *res = ((const scalar_trace *)ctx)->res;
    int qi = lit_idx(l);
    return res->head_off[qi + 1] - res->head_off[qi];
}
static int sc_head_at(void *ctx, dl_lit l, int i)
{ return ((const scalar_trace *)ctx)->res->head_off[lit_idx(l)] + i; }
static dl_rule_kind sc_rule_kind(void *ctx, int r)
{ return (dl_rule_kind)((const scalar_trace *)ctx)->res->rkind[r]; }
static int sc_nbody(void *ctx, int r)
{
    const dl_result *res = ((const scalar_trace *)ctx)->res;
    return res->rbody_off[r + 1] - res->rbody_off[r];
}
static dl_lit sc_body_at(void *ctx, int r, int i)
{
    const dl_result *res = ((const scalar_trace *)ctx)->res;
    return lit_from_idx(res->body[res->rbody_off[r] + i]);
}
static int sc_applicable(void *ctx, int r)
{ return ts_applicable(((const scalar_trace *)ctx)->res, r); }
static bool sc_beats(void *ctx, int w, int l)
{ return beats_c(((const scalar_trace *)ctx)->res, w, l); }
static const char *sc_rule_prov(void *ctx, int r)
{ return ((const scalar_trace *)ctx)->res->rprov[r]; }
static int sc_cycle_members(void *ctx, dl_lit l, dl_lit *out, int cap)
{
    const dl_result *res = ((const scalar_trace *)ctx)->res;
    int qi = lit_idx(l);
    if (!res->completed || qi >= res->nlits || !res->completed[qi])
        return 0;
    int c = res->scc[qi], n = 0;
    for (int k = res->scc_off[c]; k < res->scc_off[c + 1]; k++) {
        if (n < cap) out[n] = lit_from_idx(res->scc_lit[k]);
        n++;
    }
    return n;
}

static const dl_trace_vtbl scalar_vtbl = {
    .put_lit = sc_put_lit, .put_rule = sc_put_rule,
    .definite = sc_definite, .defeasible = sc_defeasible,
    .is_fact = sc_is_fact, .solved = sc_solved,
    .nhead = sc_nhead, .head_at = sc_head_at,
    .rule_kind = sc_rule_kind, .nbody = sc_nbody, .body_at = sc_body_at,
    .applicable = sc_applicable, .beats = sc_beats,
    .rule_prov = sc_rule_prov,
    .cycle_members = sc_cycle_members,
};

void dl_why(const dl_theory *t, const dl_result *res, dl_lit q, FILE *out)
{
    scalar_trace ctx = { t, res };
    dl_trace_render(&scalar_vtbl, &ctx, q, out);
}
