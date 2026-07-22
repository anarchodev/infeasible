#include "lang/story.h"
#include "lang/lexer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NAME       64      /* labels, sort/var identifiers */
#define MAX_ARGS       6       /* args per atom / vars per rule */
#define MAX_BODY       32      /* atoms per conjunction */
#define MAX_SORTS      32
#define MAX_ENTS       512
#define MAX_FLUENTS    256     /* fluent *predicate* schemas */
#define MAX_PREDS      512     /* predicate registry (fluents + heads) */
#define MAX_RULES      256
#define MAX_ACTIONS    128
#define MAX_SUPS       256
#define MAX_INITS      256
#define MAX_GROUND     256     /* ground atom name buffer */
#define MAX_INSTANCES  (1 << 20)   /* per-rule grounding blow-up guard */
#define CARD_WARN      100000      /* cross-product cardinality warning (§5.2) */

/* ---- AST ------------------------------------------------------------ */

typedef struct { uint32_t name; int line, col; } ast_arg;

typedef struct {
    uint32_t pred;
    bool     neg;
    int      nargs;
    ast_arg  args[MAX_ARGS];
    int      line, col;
} ast_atom;

typedef struct { uint32_t name; int sort; int line, col; } var_bind;

typedef struct {
    char         label[MAX_NAME];
    int          line, col;
    dl_rule_kind kind;
    var_bind     vars[MAX_ARGS];
    int          nvars;
    ast_atom     body[MAX_BODY];
    int          nbody;
    ast_atom     head;
    bool         has_guard;
    ast_atom     guard[MAX_BODY];
    int          nguard;
    /* grounding results, in odometer order (var 0 most significant) */
    struct { int handle; } *insts;
    int          ninst;
} ast_rule;

typedef struct {
    char      name[MAX_NAME];
    int       line, col;
    var_bind  vars[MAX_ARGS];
    int       nvars;
    ast_atom  requires[MAX_BODY];
    int       nreq;
    ast_atom  effects[MAX_BODY];
    int       neff;
} ast_action;

typedef struct {
    uint32_t pred;
    int      nargs;
    uint32_t argsort[MAX_ARGS];   /* declared sort name atoms, resolved later */
    int      line, col;
} ast_fluent;

typedef struct { char a[MAX_NAME], b[MAX_NAME]; int aline, acol, bline, bcol; } ast_sup;

/* predicate registry entry: a name is a fluent (with arg sorts) and/or a
 * conclusion head. Arity must be consistent across all its uses. */
typedef struct {
    uint32_t pred;
    int      arity;
    bool     is_fluent;
    bool     is_head;
    int      argsort[MAX_ARGS];   /* sort indices; valid when is_fluent */
} pred_info;

typedef struct {
    lexer        lx;
    token        cur;
    intern      *syms;
    world       *w;
    story_diags *diags;
    int          nerrors;
    bool         err_flag;        /* an error hit in the current declaration */

    struct { char name[MAX_NAME]; int line, col; } sorts[MAX_SORTS];
    int nsorts;
    struct { uint32_t atom; int sort; int line, col; } ents[MAX_ENTS];
    int nents;
    ast_fluent  fluents[MAX_FLUENTS];
    int nfluents;
    ast_rule   *rules;            /* heap; MAX_RULES */
    int nrules;
    ast_action *actions;          /* heap; MAX_ACTIONS */
    int nactions;
    ast_sup     sups[MAX_SUPS];
    int nsups;
    ast_atom    inits[MAX_INITS];
    int ninits;

    pred_info   preds[MAX_PREDS];
    int npreds;

    /* orphan/typo analysis (§6.1), predicate-level: a body/guard/requires
     * predicate that is neither a declared fluent nor any rule head is
     * always-false — the Osiris typo bug. First use location is kept. */
    struct { uint32_t pred; int line, col; } refs[MAX_PREDS];
    int nrefs;
} parser;

/* ---- diagnostics ---------------------------------------------------- */

static void add_diag(parser *p, story_severity sev, int line, int col,
                     const char *fmt, va_list ap)
{
    if (sev == STORY_ERROR) p->nerrors++;
    story_diags *d = p->diags;
    if (!d) return;
    int idx = d->count++;
    if (sev == STORY_ERROR) d->nerrors++;
    if (d->items && idx < d->cap) {
        story_diag *dg = &d->items[idx];
        dg->sev = sev;
        dg->line = line;
        dg->col = col;
        vsnprintf(dg->msg, sizeof dg->msg, fmt, ap);
    }
}

/* Parse-time error: at most one per declaration (err_flag); the top-level
 * loop synchronises to the next declaration boundary and clears it. */
static void fail(parser *p, int line, int col, const char *fmt, ...)
{
    if (p->err_flag) return;
    p->err_flag = true;
    va_list ap;
    va_start(ap, fmt);
    add_diag(p, STORY_ERROR, line, col, fmt, ap);
    va_end(ap);
}

/* Semantic-pass error: no per-declaration gating — every distinct problem in
 * the well-formed AST is reported. */
static void serr(parser *p, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_diag(p, STORY_ERROR, line, col, fmt, ap);
    va_end(ap);
}

static void warn(parser *p, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_diag(p, STORY_WARNING, line, col, fmt, ap);
    va_end(ap);
}

static void tok_desc(token t, char *buf, size_t n)
{
    if (t.kind == TK_IDENT || t.kind == TK_INT)
        snprintf(buf, n, "'%.*s'", t.len, t.start);
    else
        snprintf(buf, n, "%s", tok_kind_name(t.kind));
}

/* ---- token stream --------------------------------------------------- */

static void advance(parser *p) { p->cur = lexer_next(&p->lx); }

static bool expect(parser *p, tok_kind k)
{
    if (p->cur.kind == k) { advance(p); return true; }
    char d[64];
    tok_desc(p->cur, d, sizeof d);
    fail(p, p->cur.line, p->cur.col, "expected %s, found %s",
         tok_kind_name(k), d);
    return false;
}

static uint32_t intern_tok(parser *p, token t)
{
    char buf[256];
    int n = t.len < (int)sizeof buf - 1 ? t.len : (int)sizeof buf - 1;
    memcpy(buf, t.start, (size_t)n);
    buf[n] = '\0';
    return intern_id(p->syms, buf);
}

static void copy_ident(char *dst, size_t cap, token t)
{
    int n = t.len < (int)cap - 1 ? t.len : (int)cap - 1;
    memcpy(dst, t.start, (size_t)n);
    dst[n] = '\0';
}

/* ---- declaration parsing -------------------------------------------- */

/* sort := 'sort' ( IDENT | '(' (','? IDENT)* ')' ) */
static void parse_sort(parser *p)
{
    advance(p);                                    /* 'sort' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return;
        }
        if (p->nsorts >= MAX_SORTS) {
            fail(p, p->cur.line, p->cur.col, "too many sorts (max %d)", MAX_SORTS);
            return;
        }
        copy_ident(p->sorts[p->nsorts].name, MAX_NAME, p->cur);
        p->sorts[p->nsorts].line = p->cur.line;
        p->sorts[p->nsorts].col = p->cur.col;
        p->nsorts++;
        advance(p);
        if (p->cur.kind == TK_COMMA) advance(p);    /* optional separator */
    } while (p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* entity := 'entity' ( ebind | '(' ebind* ')' ); ebind := IDENT (',' IDENT)* ':' IDENT */
static void parse_entity(parser *p)
{
    advance(p);                                    /* 'entity' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        token names[MAX_ENTS];                     /* names sharing one sort */
        int nn = 0;
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an entity name, found %s", d);
                return;
            }
            if (nn < MAX_ENTS) names[nn++] = p->cur;
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_COLON)) return;
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return;
        }
        uint32_t sort_atom = intern_tok(p, p->cur);
        int sortline = p->cur.line, sortcol = p->cur.col;
        advance(p);
        for (int i = 0; i < nn; i++) {
            if (p->nents >= MAX_ENTS) {
                fail(p, sortline, sortcol, "too many entities (max %d)", MAX_ENTS);
                return;
            }
            /* sort resolves in the semantic pass; store the sort name atom in
             * a temporary sort slot of -1 tagged via argsort trick — instead
             * keep the sort name for resolution below. */
            p->ents[p->nents].atom = intern_tok(p, names[i]);
            p->ents[p->nents].sort = -1;           /* filled after sorts known */
            p->ents[p->nents].line = names[i].line;
            p->ents[p->nents].col = names[i].col;
            /* stash the declared sort name in a parallel field via argsort0 */
            /* (resolved in resolve_entities using ents_sortname[]) */
            p->nents++;
        }
        /* record the sort name for these entities */
        for (int i = p->nents - nn; i < p->nents; i++)
            p->ents[i].sort = -(int)sort_atom - 2;  /* encode name atom, decode later */
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* atom := [ '~' ] IDENT [ '(' arg (',' arg)* ')' ] */
static bool parse_atom(parser *p, ast_atom *out)
{
    memset(out, 0, sizeof *out);
    if (p->cur.kind == TK_TILDE) { out->neg = true; advance(p); }
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an atom name, found %s", d);
        return false;
    }
    token id = p->cur;
    out->pred = intern_tok(p, id);
    out->line = id.line;
    out->col = id.col;
    advance(p);
    if (p->cur.kind == TK_LPAREN) {
        advance(p);
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an argument name, found %s", d);
                return false;
            }
            if (out->nargs >= MAX_ARGS) {
                fail(p, p->cur.line, p->cur.col,
                     "too many arguments (max %d)", MAX_ARGS);
                return false;
            }
            out->args[out->nargs].name = intern_tok(p, p->cur);
            out->args[out->nargs].line = p->cur.line;
            out->args[out->nargs].col = p->cur.col;
            out->nargs++;
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_RPAREN)) return false;
    }
    return true;
}

/* conj := atom ( '&' atom )* ; greedy, newline-insensitive (a bare atom with
 * no leading '&' begins the next construct). */
static int parse_conj(parser *p, ast_atom *out, int cap)
{
    if (!parse_atom(p, &out[0])) return -1;
    int n = 1;
    while (p->cur.kind == TK_AMP) {
        advance(p);
        if (n >= cap) {
            fail(p, p->cur.line, p->cur.col,
                 "conjunction too long (max %d atoms)", cap);
            return -1;
        }
        if (!parse_atom(p, &out[n])) return -1;
        n++;
    }
    return n;
}

/* fdecl := IDENT [ '(' IDENT (',' IDENT)* ')' ]; a ':' after it is a typed or
 * multi-valued fluent, out of this slice. */
static bool parse_fdecl(parser *p, ast_fluent *f)
{
    memset(f, 0, sizeof *f);
    token id = p->cur;
    f->pred = intern_tok(p, id);
    f->line = id.line;
    f->col = id.col;
    advance(p);
    if (p->cur.kind == TK_LPAREN) {
        advance(p);
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected a sort name, found %s", d);
                return false;
            }
            if (f->nargs >= MAX_ARGS) {
                fail(p, p->cur.line, p->cur.col,
                     "too many fluent arguments (max %d)", MAX_ARGS);
                return false;
            }
            f->argsort[f->nargs++] = intern_tok(p, p->cur);
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_RPAREN)) return false;
    }
    if (p->cur.kind == TK_COLON) {
        fail(p, p->cur.line, p->cur.col,
             "numeric and multi-valued fluents are not supported in this slice "
             "(booleans only; §5.7/5.8 domains land later in M1)");
        return false;
    }
    return true;
}

/* state := 'state' ( fdecl | '(' fdecl* ')' ) */
static void parse_state(parser *p)
{
    advance(p);                                    /* 'state' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a fluent name, found %s", d);
            return;
        }
        if (p->nfluents >= MAX_FLUENTS) {
            fail(p, p->cur.line, p->cur.col, "too many fluents (max %d)", MAX_FLUENTS);
            return;
        }
        if (!parse_fdecl(p, &p->fluents[p->nfluents])) return;
        p->nfluents++;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* init := 'init' ( atom | '(' atom* ')' ); atoms are ground (entity args). */
static void parse_init(parser *p)
{
    advance(p);                                    /* 'init' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a fluent name, found %s", d);
            return;
        }
        if (p->ninits >= MAX_INITS) {
            fail(p, p->cur.line, p->cur.col, "too many init facts (max %d)", MAX_INITS);
            return;
        }
        ast_atom a;
        if (!parse_atom(p, &a)) return;
        if (a.neg) {
            fail(p, a.line, a.col,
                 "init lists facts that start true; a negated init is redundant "
                 "(everything unlisted is closed-world false)");
            return;
        }
        p->inits[p->ninits++] = a;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* params := '(' vbind (',' vbind)* ')'; vbind := IDENT ':' IDENT */
static bool parse_params(parser *p, var_bind *vars, int *nvars)
{
    *nvars = 0;
    if (p->cur.kind != TK_LPAREN) return true;     /* no params */
    advance(p);
    for (;;) {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a variable name, found %s", d);
            return false;
        }
        if (*nvars >= MAX_ARGS) {
            fail(p, p->cur.line, p->cur.col, "too many variables (max %d)", MAX_ARGS);
            return false;
        }
        var_bind *v = &vars[*nvars];
        v->name = intern_tok(p, p->cur);
        v->line = p->cur.line;
        v->col = p->cur.col;
        v->sort = -1;
        advance(p);
        if (!expect(p, TK_COLON)) return false;
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return false;
        }
        /* encode the sort name atom for resolution in the semantic pass */
        v->sort = -(int)intern_tok(p, p->cur) - 2;
        (*nvars)++;
        advance(p);
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    return expect(p, TK_RPAREN);
}

/* rule := 'rule' IDENT [ params ] ':' conj OP atom [ 'unless' conj ] */
static void parse_rule(parser *p)
{
    advance(p);                                    /* 'rule' */
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a rule label, found %s", d);
        return;
    }
    if (p->nrules >= MAX_RULES) {
        fail(p, p->cur.line, p->cur.col, "too many rules (max %d)", MAX_RULES);
        return;
    }
    ast_rule *r = &p->rules[p->nrules];
    memset(r, 0, sizeof *r);
    copy_ident(r->label, MAX_NAME, p->cur);
    r->line = p->cur.line;
    r->col = p->cur.col;
    advance(p);

    if (!parse_params(p, r->vars, &r->nvars)) return;
    if (!expect(p, TK_COLON)) return;

    int nb = parse_conj(p, r->body, MAX_BODY);
    if (nb < 0) return;
    r->nbody = nb;

    switch (p->cur.kind) {
    case TK_ARROW:    r->kind = DL_STRICT;     break;
    case TK_FATARROW: r->kind = DL_DEFEASIBLE; break;
    case TK_SQARROW:  r->kind = DL_DEFEATER;   break;
    default: {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col,
             "expected a rule arrow ('->', '=>', or '~>'), found %s", d);
        return;
    }
    }
    advance(p);

    if (!parse_atom(p, &r->head)) return;

    if (p->cur.kind == TK_UNLESS) {
        advance(p);
        int ng = parse_conj(p, r->guard, MAX_BODY);
        if (ng < 0) return;
        r->nguard = ng;
        r->has_guard = true;
    }
    p->nrules++;
}

/* action := 'action' IDENT [ params ] ':' [ 'requires' conj ] 'causes' conj */
static void parse_action(parser *p)
{
    advance(p);                                    /* 'action' */
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an action name, found %s", d);
        return;
    }
    if (p->nactions >= MAX_ACTIONS) {
        fail(p, p->cur.line, p->cur.col, "too many actions (max %d)", MAX_ACTIONS);
        return;
    }
    ast_action *a = &p->actions[p->nactions];
    memset(a, 0, sizeof *a);
    copy_ident(a->name, MAX_NAME, p->cur);
    a->line = p->cur.line;
    a->col = p->cur.col;
    advance(p);

    if (!parse_params(p, a->vars, &a->nvars)) return;
    if (!expect(p, TK_COLON)) return;

    if (p->cur.kind == TK_REQUIRES) {
        advance(p);
        int nr = parse_conj(p, a->requires, MAX_BODY);
        if (nr < 0) return;
        a->nreq = nr;
    }
    if (!expect(p, TK_CAUSES)) return;
    int ne = parse_conj(p, a->effects, MAX_BODY);
    if (ne < 0) return;
    a->neff = ne;
    p->nactions++;
}

/* sup := IDENT '>' IDENT (label > label) */
static void parse_sup(parser *p)
{
    if (p->nsups >= MAX_SUPS) {
        fail(p, p->cur.line, p->cur.col, "too many superiority edges (max %d)", MAX_SUPS);
        return;
    }
    ast_sup *s = &p->sups[p->nsups];
    copy_ident(s->a, MAX_NAME, p->cur);
    s->aline = p->cur.line; s->acol = p->cur.col;
    advance(p);
    if (!expect(p, TK_GT)) return;
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a rule label, found %s", d);
        return;
    }
    copy_ident(s->b, MAX_NAME, p->cur);
    s->bline = p->cur.line; s->bcol = p->cur.col;
    advance(p);
    p->nsups++;
}

/* ---- semantic pass: name resolution & schema ------------------------ */

static int find_sort(parser *p, uint32_t name_atom)
{
    const char *name = intern_name(p->syms, name_atom);
    for (int i = 0; i < p->nsorts; i++)
        if (strcmp(p->sorts[i].name, name) == 0) return i;
    return -1;
}

static int find_entity(parser *p, uint32_t atom)
{
    for (int i = 0; i < p->nents; i++)
        if (p->ents[i].atom == atom) return i;
    return -1;
}

/* domain of a sort: entities declared for it, in declaration order */
static int domain_size(parser *p, int sort)
{
    int n = 0;
    for (int i = 0; i < p->nents; i++)
        if (p->ents[i].sort == sort) n++;
    return n;
}
static uint32_t domain_at(parser *p, int sort, int k)
{
    for (int i = 0; i < p->nents; i++)
        if (p->ents[i].sort == sort && k-- == 0) return p->ents[i].atom;
    return INTERN_NONE;
}
static int entity_pos(parser *p, int sort, uint32_t atom)
{
    int pos = 0;
    for (int i = 0; i < p->nents; i++) {
        if (p->ents[i].sort != sort) continue;
        if (p->ents[i].atom == atom) return pos;
        pos++;
    }
    return -1;
}

static pred_info *find_pred(parser *p, uint32_t pred)
{
    for (int i = 0; i < p->npreds; i++)
        if (p->preds[i].pred == pred) return &p->preds[i];
    return NULL;
}
static pred_info *intern_pred(parser *p, uint32_t pred, int arity)
{
    pred_info *pi = find_pred(p, pred);
    if (pi) return pi;
    if (p->npreds >= MAX_PREDS) return NULL;
    pi = &p->preds[p->npreds++];
    memset(pi, 0, sizeof *pi);
    pi->pred = pred;
    pi->arity = arity;
    return pi;
}

static bool is_head_pred(parser *p, uint32_t pred)
{
    pred_info *pi = find_pred(p, pred);
    return pi && pi->is_head;
}
static bool is_fluent_pred(parser *p, uint32_t pred)
{
    pred_info *pi = find_pred(p, pred);
    return pi && pi->is_fluent;
}

static void note_ref(parser *p, uint32_t pred, int line, int col)
{
    for (int i = 0; i < p->nrefs; i++)
        if (p->refs[i].pred == pred) return;       /* keep first location */
    if (p->nrefs < MAX_PREDS) {
        p->refs[p->nrefs].pred = pred;
        p->refs[p->nrefs].line = line;
        p->refs[p->nrefs].col = col;
        p->nrefs++;
    }
}

/* Resolve the sort-name encoding stashed by the parser (see the -(atom)-2
 * trick) into a real sort index, reporting unknown sorts. */
static int decode_sort(parser *p, int encoded, int line, int col, const char *what)
{
    if (encoded >= 0) return encoded;              /* already resolved */
    uint32_t name_atom = (uint32_t)(-(encoded) - 2);
    int s = find_sort(p, name_atom);
    if (s < 0)
        serr(p, line, col, "unknown sort '%s' in %s (declare it with `sort`)",
             intern_name(p->syms, name_atom), what);
    return s;
}

/* Resolve entity sort assignments, then validate uniqueness. */
static void resolve_entities(parser *p)
{
    for (int i = 0; i < p->nents; i++) {
        int s = decode_sort(p, p->ents[i].sort, p->ents[i].line, p->ents[i].col,
                            "an entity declaration");
        p->ents[i].sort = s;                       /* may be -1 on error */
    }
    for (int i = 0; i < p->nents; i++)
        for (int j = i + 1; j < p->nents; j++)
            if (p->ents[i].atom == p->ents[j].atom)
                serr(p, p->ents[j].line, p->ents[j].col,
                     "duplicate entity '%s'", intern_name(p->syms, p->ents[i].atom));
}

/* Build the predicate registry: fluents (with arg sorts) plus rule heads. */
static void build_pred_registry(parser *p)
{
    for (int i = 0; i < p->nsorts; i++)
        for (int j = i + 1; j < p->nsorts; j++)
            if (strcmp(p->sorts[i].name, p->sorts[j].name) == 0)
                serr(p, p->sorts[j].line, p->sorts[j].col,
                     "duplicate sort '%s'", p->sorts[i].name);

    for (int i = 0; i < p->nfluents; i++) {
        ast_fluent *f = &p->fluents[i];
        pred_info *pi = find_pred(p, f->pred);
        if (pi && pi->is_fluent) {
            serr(p, f->line, f->col, "duplicate fluent '%s'",
                 intern_name(p->syms, f->pred));
            continue;
        }
        pi = intern_pred(p, f->pred, f->nargs);
        if (!pi) { serr(p, f->line, f->col, "too many predicates"); return; }
        if (pi->arity != f->nargs)
            serr(p, f->line, f->col,
                 "'%s' is used with %d and %d arguments",
                 intern_name(p->syms, f->pred), pi->arity, f->nargs);
        pi->is_fluent = true;
        pi->arity = f->nargs;
        for (int k = 0; k < f->nargs; k++)
            pi->argsort[k] = decode_sort(p, -(int)f->argsort[k] - 2,
                                         f->line, f->col, "a fluent declaration");
    }

    /* rule heads register the conclusion predicates (arity from the head). */
    for (int i = 0; i < p->nrules; i++) {
        ast_atom *h = &p->rules[i].head;
        pred_info *pi = intern_pred(p, h->pred, h->nargs);
        if (!pi) { serr(p, h->line, h->col, "too many predicates"); return; }
        if (pi->arity != h->nargs)
            serr(p, h->line, h->col, "'%s' is used with %d and %d arguments",
                 intern_name(p->syms, h->pred), pi->arity, h->nargs);
        pi->is_head = true;
    }
}

/* Resolve a rule/action's variable sorts and check for duplicate names. */
static void resolve_vars(parser *p, var_bind *vars, int nvars, const char *what)
{
    for (int i = 0; i < nvars; i++)
        vars[i].sort = decode_sort(p, vars[i].sort, vars[i].line, vars[i].col, what);
    for (int i = 0; i < nvars; i++)
        for (int j = i + 1; j < nvars; j++)
            if (vars[i].name == vars[j].name)
                serr(p, vars[j].line, vars[j].col,
                     "duplicate variable '%s' in %s",
                     intern_name(p->syms, vars[i].name), what);
}

static int var_index(var_bind *vars, int nvars, uint32_t name)
{
    for (int i = 0; i < nvars; i++)
        if (vars[i].name == name) return i;
    return -1;
}

/* Validate one atom against the schema: predicate known, arity matches, and
 * every argument is a bound variable or a declared entity (with a sort check
 * for fluent atoms). `note` records condition refs for orphan analysis. */
static void check_atom(parser *p, ast_atom *at, var_bind *vars, int nvars,
                       bool note, const char *ctx)
{
    if (note) note_ref(p, at->pred, at->line, at->col);
    pred_info *pi = find_pred(p, at->pred);
    if (pi && pi->arity != at->nargs) {
        serr(p, at->line, at->col,
             "'%s' takes %d argument%s but %d given",
             intern_name(p->syms, at->pred), pi->arity,
             pi->arity == 1 ? "" : "s", at->nargs);
        return;
    }
    for (int k = 0; k < at->nargs; k++) {
        ast_arg *arg = &at->args[k];
        int vi = var_index(vars, nvars, arg->name);
        int ei = find_entity(p, arg->name);
        if (vi < 0 && ei < 0) {
            serr(p, arg->line, arg->col,
                 "'%s' in %s is neither a bound variable nor a declared entity",
                 intern_name(p->syms, arg->name), ctx);
            continue;
        }
        if (pi && pi->is_fluent) {              /* sort-check against schema */
            int want = pi->argsort[k];
            int got = vi >= 0 ? vars[vi].sort : p->ents[ei].sort;
            if (want >= 0 && got >= 0 && want != got)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' expects sort '%s' but got '%s'",
                     k + 1, intern_name(p->syms, at->pred),
                     p->sorts[want].name, p->sorts[got].name);
        }
    }
}

/* Every rule variable must occur in the body — the safety / range-restriction
 * discipline (§5.2 item 1). Typed vars bound the domain (item 2), so an unused
 * var is a probable authoring slip, not an unsafe grounding: warn, don't fail. */
static void check_safety(parser *p, ast_rule *r)
{
    for (int i = 0; i < r->nvars; i++) {
        bool used = false;
        for (int b = 0; b < r->nbody && !used; b++)
            for (int k = 0; k < r->body[b].nargs; k++)
                if (r->body[b].args[k].name == r->vars[i].name) { used = true; break; }
        if (!used)
            warn(p, r->vars[i].line, r->vars[i].col,
                 "variable '%s' of rule '%s' does not occur in the body — "
                 "it grounds over the whole '%s' sort",
                 intern_name(p->syms, r->vars[i].name), r->label,
                 r->vars[i].sort >= 0 ? p->sorts[r->vars[i].sort].name : "?");
    }
}

static void semantic_pass(parser *p)
{
    resolve_entities(p);
    build_pred_registry(p);

    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        resolve_vars(p, r->vars, r->nvars, "a rule");
        for (int b = 0; b < r->nbody; b++)
            check_atom(p, &r->body[b], r->vars, r->nvars, true, "a rule body");
        check_atom(p, &r->head, r->vars, r->nvars, false, "a rule head");
        for (int b = 0; b < r->nguard; b++)
            check_atom(p, &r->guard[b], r->vars, r->nvars, true, "an `unless` guard");
        check_safety(p, r);
        for (int j = i + 1; j < p->nrules; j++)
            if (strcmp(r->label, p->rules[j].label) == 0)
                serr(p, p->rules[j].line, p->rules[j].col,
                     "duplicate rule label '%s'", r->label);
    }

    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        resolve_vars(p, a->vars, a->nvars, "an action");
        for (int b = 0; b < a->nreq; b++)
            check_atom(p, &a->requires[b], a->vars, a->nvars, true, "a `requires` clause");
        for (int b = 0; b < a->neff; b++)
            check_atom(p, &a->effects[b], a->vars, a->nvars, false, "a `causes` clause");
    }

    /* init facts: predicate is a declared fluent, args are ground entities. */
    for (int i = 0; i < p->ninits; i++) {
        ast_atom *a = &p->inits[i];
        if (!is_fluent_pred(p, a->pred)) {
            serr(p, a->line, a->col,
                 "init names '%s', which is not a declared fluent",
                 intern_name(p->syms, a->pred));
            continue;
        }
        check_atom(p, a, NULL, 0, false, "an init fact");
        for (int k = 0; k < a->nargs; k++)
            if (find_entity(p, a->args[k].name) < 0)
                serr(p, a->args[k].line, a->args[k].col,
                     "init argument '%s' must be a declared entity",
                     intern_name(p->syms, a->args[k].name));
    }
}

/* ---- grounding: emit ground rules into world_* ---------------------- */

/* Build the interned ground atom "pred(e1,e2)" (bare "pred" at arity 0). */
static uint32_t ground_pred(parser *p, uint32_t pred, const uint32_t *args, int n)
{
    if (n == 0) return pred;
    char buf[MAX_GROUND];
    int off = snprintf(buf, sizeof buf, "%s(", intern_name(p->syms, pred));
    for (int i = 0; i < n && off < (int)sizeof buf; i++)
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%s%s",
                        i ? "," : "", intern_name(p->syms, args[i]));
    if (off < (int)sizeof buf) snprintf(buf + off, sizeof buf - (size_t)off, ")");
    return intern_id(p->syms, buf);
}

/* Resolve an argument name to a concrete entity atom under `binding`
 * (binding[i] is the entity chosen for vars[i]). */
static uint32_t resolve_arg(var_bind *vars, int nvars,
                            const uint32_t *binding, ast_arg arg)
{
    int vi = var_index(vars, nvars, arg.name);
    if (vi >= 0) return binding[vi];
    return arg.name;                               /* a declared entity */
}

static dl_lit ground_lit(parser *p, ast_atom *at, var_bind *vars, int nvars,
                         const uint32_t *binding)
{
    uint32_t args[MAX_ARGS];
    for (int k = 0; k < at->nargs; k++)
        args[k] = resolve_arg(vars, nvars, binding, at->args[k]);
    uint32_t g = ground_pred(p, at->pred, args, at->nargs);
    return at->neg ? dl_neg(g) : dl_pos(g);
}

/* Readable instance name for `dl_why` traces: "label[X=hero,Y=key]". */
static void inst_name(parser *p, char *buf, size_t n, const char *label,
                      var_bind *vars, int nvars, const uint32_t *binding)
{
    if (nvars == 0) { snprintf(buf, n, "%s", label); return; }
    int off = snprintf(buf, n, "%s[", label);
    for (int i = 0; i < nvars && off < (int)n; i++)
        off += snprintf(buf + off, n - (size_t)off, "%s%s=%s", i ? "," : "",
                        intern_name(p->syms, vars[i].name),
                        intern_name(p->syms, binding[i]));
    if (off < (int)n) snprintf(buf + off, n - (size_t)off, "]");
}

/* Total instances for a var list; 0 if any sort is empty or an error left a
 * sort unresolved. Guards against blow-up past MAX_INSTANCES. */
static long instance_count(parser *p, var_bind *vars, int nvars, bool *overflow)
{
    long prod = 1;
    for (int i = 0; i < nvars; i++) {
        if (vars[i].sort < 0) return 0;
        long d = domain_size(p, vars[i].sort);
        if (d == 0) return 0;
        prod *= d;
        if (prod > MAX_INSTANCES) { *overflow = true; return 0; }
    }
    return prod;
}

/* Decode odometer index -> binding entities (var 0 most significant). */
static void decode_binding(parser *p, var_bind *vars, int nvars, long idx,
                           uint32_t *binding)
{
    for (int i = nvars - 1; i >= 0; i--) {
        int d = domain_size(p, vars[i].sort);
        binding[i] = domain_at(p, vars[i].sort, (int)(idx % d));
        idx /= d;
    }
}

static void declare_ground_fluents(parser *p)
{
    for (int i = 0; i < p->nfluents; i++) {
        ast_fluent *f = &p->fluents[i];
        var_bind vb[MAX_ARGS];                     /* borrow the odometer path */
        for (int k = 0; k < f->nargs; k++) {
            vb[k].name = INTERN_NONE;
            vb[k].sort = decode_sort(p, -(int)f->argsort[k] - 2, f->line, f->col, "");
        }
        bool of = false;
        long total = instance_count(p, vb, f->nargs, &of);
        uint32_t binding[MAX_ARGS];
        for (long idx = 0; idx < total; idx++) {
            decode_binding(p, vb, f->nargs, idx, binding);
            world_declare_fluent(p->w, ground_pred(p, f->pred, binding, f->nargs));
        }
    }
}

static void ground_inits(parser *p)
{
    for (int i = 0; i < p->ninits; i++) {
        ast_atom *a = &p->inits[i];
        uint32_t args[MAX_ARGS];
        for (int k = 0; k < a->nargs; k++) args[k] = a->args[k].name;
        world_set(p->w, ground_pred(p, a->pred, args, a->nargs), true);
    }
}

static void ground_rule(parser *p, ast_rule *r)
{
    bool of = false;
    long total = instance_count(p, r->vars, r->nvars, &of);
    if (of) {
        warn(p, r->line, r->col,
             "rule '%s' grounds to more than %d instances — add a sparser "
             "anchor or split the sorts (§5.2 cardinality warning)",
             r->label, MAX_INSTANCES);
        return;
    }
    if (total == 0) return;                        /* an empty sort: no ground rules */
    if (total > CARD_WARN)
        warn(p, r->line, r->col,
             "rule '%s' grounds to %ld instances with no sparse anchor "
             "(§5.2 cardinality warning)", r->label, total);

    r->insts = malloc((size_t)total * sizeof *r->insts);
    r->ninst = (int)total;

    uint32_t binding[MAX_ARGS];
    char name[MAX_GROUND];
    for (long idx = 0; idx < total; idx++) {
        decode_binding(p, r->vars, r->nvars, idx, binding);
        dl_lit head = ground_lit(p, &r->head, r->vars, r->nvars, binding);
        dl_lit body[MAX_BODY];
        for (int b = 0; b < r->nbody; b++)
            body[b] = ground_lit(p, &r->body[b], r->vars, r->nvars, binding);
        inst_name(p, name, sizeof name, r->label, r->vars, r->nvars, binding);
        r->insts[idx].handle = world_add_rule(p->w, name, r->kind, head, body, r->nbody);

        /* `unless G` sugars to a defeater blocking this instance's head:
         * G ~> ~head (DESIGN.md §6), reinstated whenever the guard fails. */
        if (r->has_guard) {
            dl_lit guard[MAX_BODY];
            for (int b = 0; b < r->nguard; b++)
                guard[b] = ground_lit(p, &r->guard[b], r->vars, r->nvars, binding);
            char gname[MAX_GROUND + 8];
            snprintf(gname, sizeof gname, "%s.unless", name);
            world_add_rule(p->w, gname, DL_DEFEATER, dl_complement(head),
                           guard, r->nguard);
        }
    }
}

static void ground_action(parser *p, ast_action *a)
{
    bool of = false;
    long total = instance_count(p, a->vars, a->nvars, &of);
    if (of) {
        warn(p, a->line, a->col,
             "action '%s' grounds to more than %d instances", a->name, MAX_INSTANCES);
        return;
    }
    if (total == 0) return;

    uint32_t binding[MAX_ARGS];
    for (long idx = 0; idx < total; idx++) {
        decode_binding(p, a->vars, a->nvars, idx, binding);
        /* the action trigger atom is the ground action term itself */
        uint32_t actargs[MAX_ARGS];
        for (int k = 0; k < a->nvars; k++) actargs[k] = binding[k];
        char aname[MAX_GROUND];
        inst_name(p, aname, sizeof aname, a->name, a->vars, a->nvars, binding);
        uint32_t act;
        {
            /* action atom: "name(e1,..)" over the ground params, bare at arity 0 */
            uint32_t nameatom = intern_id(p->syms, a->name);
            act = ground_pred(p, nameatom, actargs, a->nvars);
        }
        step_cond conds[MAX_BODY];
        for (int b = 0; b < a->nreq; b++) {
            conds[b].lit = ground_lit(p, &a->requires[b], a->vars, a->nvars, binding);
            conds[b].primed = false;               /* current-state guards */
        }
        dl_lit eff[MAX_BODY];
        for (int b = 0; b < a->neff; b++)
            eff[b] = ground_lit(p, &a->effects[b], a->vars, a->nvars, binding);
        world_add_step_rule(p->w, aname, act, conds, a->nreq, eff, a->neff);
    }
}

static ast_rule *find_rule(parser *p, const char *label)
{
    for (int i = 0; i < p->nrules; i++)
        if (strcmp(p->rules[i].label, label) == 0) return &p->rules[i];
    return NULL;
}

/* Encode a binding of `r`'s own variables (as entity atoms) into its odometer
 * index, so a superiority edge can find the exact ground instance. */
static long encode_rule_index(parser *p, ast_rule *r, const uint32_t *ent_for_var)
{
    long idx = 0;
    for (int i = 0; i < r->nvars; i++) {
        int d = domain_size(p, r->vars[i].sort);
        int pos = entity_pos(p, r->vars[i].sort, ent_for_var[i]);
        if (pos < 0) return -1;
        idx = idx * d + pos;
    }
    return idx;
}

/* Ground `A > B` over the union of both rules' variables, matching shared
 * names. `too_weak(X) > can_force(X)` becomes one edge per actor, not the
 * cross product; unshared vars range independently. */
static void ground_sup(parser *p, ast_sup *s)
{
    ast_rule *ra = find_rule(p, s->a);
    ast_rule *rb = find_rule(p, s->b);
    if (!ra) {
        serr(p, s->aline, s->acol, "unknown rule label '%s' in superiority", s->a);
        return;
    }
    if (!rb) {
        serr(p, s->bline, s->bcol, "unknown rule label '%s' in superiority", s->b);
        return;
    }
    if (!ra->insts || !rb->insts) return;          /* a rule failed to ground */

    /* union variable list, shared by name (sorts must agree) */
    var_bind uni[2 * MAX_ARGS];
    int nuni = 0;
    for (int i = 0; i < ra->nvars; i++) uni[nuni++] = ra->vars[i];
    for (int i = 0; i < rb->nvars; i++) {
        int j = var_index(uni, nuni, rb->vars[i].name);
        if (j < 0) uni[nuni++] = rb->vars[i];
        else if (uni[j].sort != rb->vars[i].sort) {
            serr(p, s->bline, s->bcol,
                 "superiority '%s > %s' shares variable '%s' at different sorts",
                 s->a, s->b, intern_name(p->syms, rb->vars[i].name));
            return;
        }
    }

    bool of = false;
    long total = instance_count(p, uni, nuni, &of);
    if (of || total == 0) {
        /* nuni==0 -> total==1 handled below; only reachable if a sort empty */
        if (nuni == 0) total = 1; else return;
    }
    uint32_t ubind[2 * MAX_ARGS], abind[MAX_ARGS], bbind[MAX_ARGS];
    for (long idx = 0; idx < total; idx++) {
        decode_binding(p, uni, nuni, idx, ubind);
        for (int i = 0; i < ra->nvars; i++)
            abind[i] = ubind[var_index(uni, nuni, ra->vars[i].name)];
        for (int i = 0; i < rb->nvars; i++)
            bbind[i] = ubind[var_index(uni, nuni, rb->vars[i].name)];
        long ai = encode_rule_index(p, ra, abind);
        long bi = encode_rule_index(p, rb, bbind);
        if (ai < 0 || bi < 0 || ai >= ra->ninst || bi >= rb->ninst) continue;
        world_add_sup(p->w, ra->insts[ai].handle, rb->insts[bi].handle);
    }
}

/* Any predicate used in a condition that is neither a declared fluent nor a
 * rule head can never be true — the Osiris typo bug (§6.1). */
static void check_orphans(parser *p)
{
    for (int i = 0; i < p->nrefs; i++) {
        uint32_t a = p->refs[i].pred;
        if (is_fluent_pred(p, a) || is_head_pred(p, a)) continue;
        warn(p, p->refs[i].line, p->refs[i].col,
             "'%s' is used as a condition but is never a declared fluent or "
             "concluded by any rule — typo, or a missing declaration?",
             intern_name(p->syms, a));
    }
}

/* ---- entry ---------------------------------------------------------- */

/* Panic-mode recovery (§10): skip to the next declaration boundary. */
static void synchronize(parser *p)
{
    while (p->cur.kind != TK_EOF) {
        switch (p->cur.kind) {
        case TK_SORT: case TK_ENTITY: case TK_STATE:
        case TK_INIT: case TK_RULE:   case TK_ACTION:
            return;
        default:
            advance(p);
        }
    }
}

world *story_compile(const char *src, intern *syms, story_diags *diags)
{
    parser *p = calloc(1, sizeof *p);
    p->rules = calloc(MAX_RULES, sizeof *p->rules);
    p->actions = calloc(MAX_ACTIONS, sizeof *p->actions);
    lexer_init(&p->lx, src);
    p->syms = syms;
    p->w = world_new(syms);
    p->diags = diags;
    if (diags) { diags->count = 0; diags->nerrors = 0; }

    /* pass 1: parse into the AST */
    advance(p);
    while (p->cur.kind != TK_EOF) {
        p->err_flag = false;
        switch (p->cur.kind) {
        case TK_SORT:   parse_sort(p);   break;
        case TK_ENTITY: parse_entity(p); break;
        case TK_STATE:  parse_state(p);  break;
        case TK_INIT:   parse_init(p);   break;
        case TK_RULE:   parse_rule(p);   break;
        case TK_ACTION: parse_action(p); break;
        case TK_IDENT:  parse_sup(p);    break;
        default: {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a declaration (sort/entity/state/init/rule/action) "
                 "or a superiority statement, found %s", d);
            break;
        }
        }
        if (p->err_flag) synchronize(p);
    }

    world *result = NULL;
    if (p->nerrors == 0) {
        /* pass 2: semantic analysis, then build-time grounding */
        semantic_pass(p);
        if (p->nerrors == 0) {
            declare_ground_fluents(p);
            ground_inits(p);
            for (int i = 0; i < p->nrules; i++)   ground_rule(p, &p->rules[i]);
            for (int i = 0; i < p->nactions; i++) ground_action(p, &p->actions[i]);
            for (int i = 0; i < p->nsups; i++)    ground_sup(p, &p->sups[i]);
            check_orphans(p);
        }
    }

    if (p->nerrors == 0) result = p->w;
    else world_free(p->w);

    for (int i = 0; i < p->nrules; i++) free(p->rules[i].insts);
    free(p->rules);
    free(p->actions);
    free(p);
    return result;
}
