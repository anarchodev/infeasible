#include "logic/dl.h"
#include "core/arena.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
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

struct dl_result {
    int nlits;
    signed char *delta;  /* dl_verdict per literal index */
    signed char *part;
    /* CSR index: rules grouped by head literal (borrowed from theory) */
    int *head_off;       /* nlits + 1 */
    int *head_rules;     /* nrules */
    bool *is_fact;
};

static int lit_idx(dl_lit l) { return (int)l.atom * 2 + (l.neg ? 1 : 0); }

#define GROW(arr, n, cap) \
    do { \
        if ((n) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 16; \
            (arr) = realloc((arr), (size_t)(cap) * sizeof *(arr)); \
        } \
    } while (0)

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
    r->kind = kind;
    r->head = head;
    r->nbody = nbody;
    r->body = arena_alloc(&t->a, (size_t)(nbody ? nbody : 1) * sizeof(dl_lit));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(dl_lit));
    return t->nrules++;
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

static bool beats(const dl_theory *t, int winner, int loser)
{
    for (int i = 0; i < t->nsups; i++)
        if (t->sups[i].winner == winner && t->sups[i].loser == loser)
            return true;
    return false;
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

/* AND over body of +d(a) : +1 iff all body literals defeasibly proved,
 * -1 iff some body literal is defeasibly refuted. */
static int ts_applicable(const dl_result *res, const rule *r)
{
    int acc = 1;
    for (int i = 0; i < r->nbody; i++) {
        acc = ts_min(acc, ts_of(res->part[lit_idx(r->body[i])], DL_PROVED));
        if (acc == -1)
            break;
    }
    return acc;
}

#define FOR_HEAD_RULES(res, qi, var) \
    for (int _hi = (res)->head_off[qi]; _hi < (res)->head_off[(qi) + 1] && ((var) = (res)->head_rules[_hi], 1); _hi++)

/* supported(q): some strict/defeasible rule for q is applicable */
static int ts_supported(const dl_theory *t, const dl_result *res, int qi)
{
    int acc = -1, ri;
    FOR_HEAD_RULES(res, qi, ri) {
        const rule *r = &t->rules[ri];
        if (r->kind == DL_DEFEATER)
            continue;
        acc = ts_max(acc, ts_applicable(res, r));
        if (acc == 1)
            break;
    }
    return acc;
}

/* countered(s, q): attacker s (a rule for ~q) is inapplicable, or beaten by
 * some applicable strict/defeasible rule for q (team defeat). */
static int ts_countered(const dl_theory *t, const dl_result *res, int si, int qi)
{
    int acc = -ts_applicable(res, &t->rules[si]);
    int ti;
    FOR_HEAD_RULES(res, qi, ti) {
        const rule *tr = &t->rules[ti];
        if (tr->kind == DL_DEFEATER || !beats(t, ti, si))
            continue;
        acc = ts_max(acc, ts_applicable(res, tr));
        if (acc == 1)
            break;
    }
    return acc;
}

static int ts_all_attackers_countered(const dl_theory *t, const dl_result *res,
                                      int qi, int nqi)
{
    int acc = 1, si;
    FOR_HEAD_RULES(res, nqi, si) {
        acc = ts_min(acc, ts_countered(t, res, si, qi));
        if (acc == -1)
            break;
    }
    return acc;
}

/* uncountered attacker exists: some rule for ~q is applicable and no
 * applicable strict/defeasible rule for q beats it. */
static int ts_attacker_uncountered(const dl_theory *t, const dl_result *res,
                                   int qi, int nqi)
{
    int acc = -1, si;
    FOR_HEAD_RULES(res, nqi, si) {
        int this = ts_applicable(res, &t->rules[si]);
        int ti;
        FOR_HEAD_RULES(res, qi, ti) {
            const rule *tr = &t->rules[ti];
            if (tr->kind == DL_DEFEATER || !beats(t, ti, si))
                continue;
            this = ts_min(this, -ts_applicable(res, tr));
            if (this == -1)
                break;
        }
        acc = ts_max(acc, this);
        if (acc == 1)
            break;
    }
    return acc;
}

/* notsupported(q): every strict/defeasible rule for q has a -d body literal */
static int ts_notsupported(const dl_theory *t, const dl_result *res, int qi)
{
    int acc = 1, ri;
    FOR_HEAD_RULES(res, qi, ri) {
        const rule *r = &t->rules[ri];
        if (r->kind == DL_DEFEATER)
            continue;
        acc = ts_min(acc, -ts_applicable(res, r));
        if (acc == -1)
            break;
    }
    return acc;
}

/* ---- solve ---- */

static void solve_delta(const dl_theory *t, dl_result *res)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (int qi = 0; qi < res->nlits; qi++) {
            if (res->delta[qi] != DL_UNDECIDED)
                continue;
            if (res->is_fact[qi]) {
                res->delta[qi] = DL_PROVED;
                changed = true;
                continue;
            }
            bool proved = false, alive = false;
            int ri;
            FOR_HEAD_RULES(res, qi, ri) {
                const rule *r = &t->rules[ri];
                if (r->kind != DL_STRICT)
                    continue;
                bool all = true, dead = false;
                for (int i = 0; i < r->nbody; i++) {
                    signed char v = res->delta[lit_idx(r->body[i])];
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
                changed = true;
            } else if (!alive) {
                res->delta[qi] = DL_REFUTED;
                changed = true;
            }
        }
    }
}

static void solve_part(const dl_theory *t, dl_result *res)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t atom = 0; (int)atom * 2 < res->nlits; atom++) {
            for (int neg = 0; neg < 2; neg++) {
                dl_lit q = { atom, neg != 0 };
                int qi = lit_idx(q), nqi = lit_idx(dl_complement(q));
                if (res->part[qi] != DL_UNDECIDED)
                    continue;
                /* +d q */
                int pos = ts_of(res->delta[qi], DL_PROVED);
                if (pos != 1) {
                    int alt = ts_of(res->delta[nqi], DL_REFUTED);
                    alt = ts_min(alt, ts_supported(t, res, qi));
                    alt = ts_min(alt, ts_all_attackers_countered(t, res, qi, nqi));
                    pos = ts_max(pos, alt);
                }
                if (pos == 1) {
                    res->part[qi] = DL_PROVED;
                    changed = true;
                    continue;
                }
                /* -d q */
                int negv = ts_of(res->delta[qi], DL_REFUTED);
                if (negv != -1) {
                    int inner = ts_of(res->delta[nqi], DL_PROVED);
                    inner = ts_max(inner, ts_notsupported(t, res, qi));
                    inner = ts_max(inner, ts_attacker_uncountered(t, res, qi, nqi));
                    negv = ts_min(negv, inner);
                }
                if (negv == 1) {
                    res->part[qi] = DL_REFUTED;
                    changed = true;
                }
            }
        }
    }
}

dl_result *dl_solve(dl_theory *t)
{
    dl_result *res = calloc(1, sizeof *res);
    res->nlits = (int)intern_count(t->syms) * 2;
    res->delta = calloc((size_t)res->nlits, 1);
    res->part = calloc((size_t)res->nlits, 1);
    res->is_fact = calloc((size_t)res->nlits, 1);
    for (int i = 0; i < t->nfacts; i++)
        res->is_fact[lit_idx(t->facts[i])] = true;

    /* CSR index of rules by head literal */
    res->head_off = calloc((size_t)res->nlits + 1, sizeof *res->head_off);
    res->head_rules = calloc((size_t)(t->nrules ? t->nrules : 1),
                             sizeof *res->head_rules);
    for (int r = 0; r < t->nrules; r++)
        res->head_off[lit_idx(t->rules[r].head) + 1]++;
    for (int i = 0; i < res->nlits; i++)
        res->head_off[i + 1] += res->head_off[i];
    int *fill = calloc((size_t)res->nlits, sizeof *fill);
    for (int r = 0; r < t->nrules; r++) {
        int qi = lit_idx(t->rules[r].head);
        res->head_rules[res->head_off[qi] + fill[qi]++] = r;
    }
    free(fill);

    solve_delta(t, res);
    solve_part(t, res);
    return res;
}

void dl_result_free(dl_result *r)
{
    free(r->delta);
    free(r->part);
    free(r->is_fact);
    free(r->head_off);
    free(r->head_rules);
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

static const char *verdict_str(dl_verdict v)
{
    switch (v) {
    case DL_PROVED:  return "PROVED";
    case DL_REFUTED: return "REFUTED";
    default:         return "UNDECIDED";
    }
}

static void print_lit(const dl_theory *t, dl_lit l, FILE *out)
{
    fprintf(out, "%s%s", l.neg ? "~" : "", intern_name(t->syms, l.atom));
}

static void print_rule_line(const dl_theory *t, const dl_result *res, int ri,
                            int indent, FILE *out)
{
    static const char *kinds[] = { "strict", "defeasible", "defeater" };
    const rule *r = &t->rules[ri];
    fprintf(out, "%*s%s (%s): ", indent, "", r->name, kinds[r->kind]);
    if (r->nbody == 0)
        fprintf(out, "(no conditions)");
    for (int i = 0; i < r->nbody; i++) {
        if (i)
            fprintf(out, ", ");
        print_lit(t, r->body[i], out);
        fprintf(out, "[%s]",
                verdict_str(dl_defeasible(res, r->body[i])));
    }
    int app = ts_applicable(res, r);
    fprintf(out, "  -- %s\n",
            app == 1 ? "applicable" : app == -1 ? "inapplicable" : "undecided");
}

void dl_why(const dl_theory *t, const dl_result *res, dl_lit q, FILE *out)
{
    int qi = lit_idx(q), nqi = lit_idx(dl_complement(q));
    fprintf(out, "why ");
    print_lit(t, q, out);
    fprintf(out, "?\n  definite: %s   defeasible: %s\n",
            verdict_str(dl_definite(res, q)),
            verdict_str(dl_defeasible(res, q)));
    if (res->is_fact[qi])
        fprintf(out, "  it is a base fact\n");

    if (res->head_off[qi] < res->head_off[qi + 1]) {
        fprintf(out, "  rules for it:\n");
        int ri;
        FOR_HEAD_RULES(res, qi, ri)
            print_rule_line(t, res, ri, 4, out);
    }
    if (res->head_off[nqi] < res->head_off[nqi + 1]) {
        fprintf(out, "  rules against it:\n");
        int si;
        FOR_HEAD_RULES(res, nqi, si) {
            print_rule_line(t, res, si, 4, out);
            int ti;
            FOR_HEAD_RULES(res, qi, ti) {
                if (t->rules[ti].kind != DL_DEFEATER && beats(t, ti, si))
                    fprintf(out, "      (beaten by %s if applicable)\n",
                            t->rules[ti].name);
                if (beats(t, si, ti))
                    fprintf(out, "      (superior to %s: %s > %s)\n",
                            t->rules[ti].name, t->rules[si].name,
                            t->rules[ti].name);
            }
        }
    }
}
