#include "state/world.h"
#include "core/arena.h"

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
    const char *name;
    uint32_t action;      /* INTERN_NONE = ramification */
    step_cond *body;
    int nbody;
    dl_lit *effects;      /* primed heads */
    int neffects;
} srule;

struct world {
    arena a;
    intern *syms;
    uint32_t *fluents; bool *vals; uint32_t *primed;
    int nfl, capfl;
    jrule *jrules; int njr, capjr;
    jsup *jsups; int njs, capjs;
    srule *srules; int nsr, capsr;
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
    arena_release(&w->a);
    free(w->fluents);
    free(w->vals);
    free(w->primed);
    free(w->jrules);
    free(w->jsups);
    free(w->srules);
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
    return w->njr++;
}

void world_add_sup(world *w, int winner, int loser)
{
    GROW(w->jsups, w->njs, w->capjs);
    w->jsups[w->njs].winner = winner;
    w->jsups[w->njs].loser = loser;
    w->njs++;
}

void world_add_step_rule(world *w, const char *name, uint32_t action,
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
    w->nsr++;
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

int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz)
{
    dl_theory *th = dl_theory_new(w->syms);
    add_state_and_judgments(w, th);
    for (int i = 0; i < nactions; i++)
        dl_add_fact(th, dl_pos(actions[i]));

    /* Generated inertia: f => f' and ~f => ~f', one pair per fluent.
     * inertia rule ids recorded so causal rules can be made superior. */
    int *inertia_pos = malloc((size_t)w->nfl * sizeof *inertia_pos);
    int *inertia_neg = malloc((size_t)w->nfl * sizeof *inertia_neg);
    char buf[300];
    for (int i = 0; i < w->nfl; i++) {
        const char *fname = intern_name(w->syms, w->fluents[i]);
        dl_lit now = dl_pos(w->fluents[i]);
        dl_lit nxt = dl_pos(w->primed[i]);

        snprintf(buf, sizeof buf, "inertia+%s", fname);
        inertia_pos[i] = dl_add_rule(th, buf, DL_DEFEASIBLE, nxt, &now, 1);

        dl_lit nnow = dl_complement(now), nnxt = dl_complement(nxt);
        snprintf(buf, sizeof buf, "inertia-%s", fname);
        inertia_neg[i] = dl_add_rule(th, buf, DL_DEFEASIBLE, nnxt, &nnow, 1);
    }

    /* Causal rules and ramifications, one dl rule per effect, each superior
     * to the inertia rule it conflicts with. */
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        int nbody = r->nbody + (r->action != INTERN_NONE ? 1 : 0);
        dl_lit *body = malloc((size_t)(nbody ? nbody : 1) * sizeof *body);
        int bi = 0;
        for (int i = 0; i < r->nbody; i++)
            body[bi++] = r->body[i].primed ? primed_lit(w, r->body[i].lit)
                                           : r->body[i].lit;
        if (r->action != INTERN_NONE)
            body[bi++] = dl_pos(r->action);

        for (int e = 0; e < r->neffects; e++) {
            dl_lit eff = r->effects[e];
            int fi = fluent_index(w, eff.atom);
            dl_lit head = primed_lit(w, eff);
            snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                     eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
            int rid = dl_add_rule(th, buf, DL_DEFEASIBLE, head, body, nbody);
            /* effect f'  conflicts with inertia-f ; effect ~f' with inertia+f */
            dl_add_sup(th, rid, eff.neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
        free(body);
    }

    dl_result *res = dl_solve(th);

    int rc = 0;
    bool *next = malloc((size_t)w->nfl * sizeof *next);
    for (int i = 0; i < w->nfl; i++) {
        dl_lit p = dl_pos(w->primed[i]);
        if (dl_defeasible(res, p) == DL_PROVED) {
            next[i] = true;
        } else if (dl_defeasible(res, dl_complement(p)) == DL_PROVED) {
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
    if (rc == 0)
        memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);

    free(next);
    free(inertia_pos);
    free(inertia_neg);
    dl_result_free(res);
    dl_theory_free(th);
    return rc;
}
