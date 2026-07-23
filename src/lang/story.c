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
#define MAX_BODY       32      /* atoms per conjunction (post-family expansion) */
#define MAX_DOMAIN     32      /* values in a multi-valued fluent domain */
#define MAX_SORTS      32
#define MAX_ENTS       (1 << 20)   /* sanity ceiling; entities grow on the heap */
#define MAX_FLUENTS    256     /* fluent *predicate* schemas */
#define MAX_PREDS      512     /* predicate registry (fluents + heads) */
#define MAX_RULES      256
#define MAX_ACTIONS    128
#define MAX_SUPS       256
#define MAX_INITS      256
#define MAX_GROUND     256     /* ground atom name buffer */
#define MAX_EXPRS      4096    /* effect-expression AST node pool */
#define MAX_CODE       64      /* VM bytecode per ground effect */
#define MAX_INSTANCES  (1 << 20)   /* per-rule grounding blow-up guard */
#define CARD_WARN      100000      /* cross-product cardinality warning (§5.2) */

/* ---- AST ------------------------------------------------------------ */

typedef struct { uint32_t name; int line, col; } ast_arg;

/* An atom is either boolean (`p`, `p(a)`) or a multi-valued reference/assignment
 * (`f = v`, `f(a) = v`): `value` is 0 for boolean, else the interned value
 * symbol. Multi-valued atoms erase at ground time to a boolean value-atom
 * "f(a)=v" (§5.7); in a `causes` effect they expand to the whole family. */
typedef struct {
    uint32_t  pred;
    bool      neg;
    bool      primed;         /* postfix `'`: read in the next state (§5.4);
                               * legal only in a ramification body */
    int       nargs;
    ast_arg   args[MAX_ARGS];
    uint32_t  value;          /* MV value symbol, else 0 */
    bool      is_guard;       /* numeric comparison `f <op> n` */
    world_cmp cmp;
    long      threshold;
    bool        is_num_effect; /* numeric write `f := / += / -= expr` (§5.8) */
    world_numop numop;
    int         expr_root;     /* index into parser.exprs, when is_num_effect */
    int       line, col;
} ast_atom;

/* Effect-RHS expression tree (§5.8), interned into a parser-owned node pool.
 * A leaf is a constant (`4`) or a numeric-fluent read (`hp`, `hp(X)`); interior
 * nodes are the closed arithmetic set. Grounding walks the tree per instance,
 * folding constant subtrees and emitting VM bytecode for the rest. */
typedef enum {
    EX_CONST, EX_LOAD, EX_ADD, EX_SUB, EX_MUL, EX_NEG, EX_MIN, EX_MAX
} ex_kind;

typedef struct {
    ex_kind  kind;
    long     konst;           /* EX_CONST */
    uint32_t pred;            /* EX_LOAD: numeric fluent */
    int      nargs;
    ast_arg  args[MAX_ARGS];  /* EX_LOAD */
    int      lhs, rhs;        /* child node indices (rhs unused for CONST/LOAD/NEG) */
    int      line, col;
} ex_node;

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
    bool      is_ramif;           /* a `rule … causes` ramification: no action
                                   * trigger (act = INTERN_NONE), `requires`
                                   * holds the match condition (§5.4, §11 M1) */
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
    bool     is_mv;               /* declared with a `: { … }` value domain */
    uint32_t values[MAX_DOMAIN];  /* the domain's value symbols, in order */
    int      nvalues;
    bool     is_num;              /* declared `: int` (§5.8) */
    bool     has_range;           /* declared `in lo..hi` — the clamp range */
    long     rmin, rmax;
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
    bool     is_mv;               /* a multi-valued fluent */
    uint32_t values[MAX_DOMAIN];  /* its domain, for value-in-domain checks */
    int      nvalues;
    bool     is_num;              /* a numeric fluent (§5.8) */
} pred_info;

typedef struct {
    lexer        lx;
    token        cur;
    intern      *syms;
    const char  *srcname;         /* source file name, for provenance (§6.3) */
    world       *w;
    story_diags *diags;
    int          nerrors;
    bool         err_flag;        /* an error hit in the current declaration */

    struct { char name[MAX_NAME]; int line, col; } sorts[MAX_SORTS];
    int nsorts;
    struct ent_rec { uint32_t atom; int sort; int line, col; } *ents;  /* heap, grown */
    int nents, capents;
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

    ex_node    *exprs;            /* heap; MAX_EXPRS effect-expression nodes */
    int nexprs;

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

static bool ident_is(token t, const char *word)
{
    return t.kind == TK_IDENT && (int)strlen(word) == t.len &&
           memcmp(t.start, word, (size_t)t.len) == 0;
}

/* Parse an integer literal with an optional leading minus. */
static bool parse_int(parser *p, long *out)
{
    bool neg = false;
    if (p->cur.kind == TK_MINUS) { neg = true; advance(p); }
    if (p->cur.kind != TK_INT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an integer, found %s", d);
        return false;
    }
    *out = neg ? -p->cur.ival : p->cur.ival;
    advance(p);
    return true;
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
        token *names = NULL;                        /* names sharing one sort (heap) */
        int nn = 0, ncap = 0;
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an entity name, found %s", d);
                free(names);
                return;
            }
            if (nn == ncap) {
                ncap = ncap ? ncap * 2 : 16;
                names = realloc(names, (size_t)ncap * sizeof *names);
            }
            names[nn++] = p->cur;
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_COLON)) { free(names); return; }
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            free(names);
            return;
        }
        uint32_t sort_atom = intern_tok(p, p->cur);
        int sortline = p->cur.line, sortcol = p->cur.col;
        advance(p);
        for (int i = 0; i < nn; i++) {
            if (p->nents >= MAX_ENTS) {             /* runaway guard — scream, don't drop */
                fail(p, sortline, sortcol, "too many entities (max %d)", MAX_ENTS);
                free(names);
                return;
            }
            if (p->nents == p->capents) {
                p->capents = p->capents ? p->capents * 2 : 64;
                p->ents = realloc(p->ents, (size_t)p->capents * sizeof *p->ents);
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
        free(names);
        /* record the sort name for these entities */
        for (int i = p->nents - nn; i < p->nents; i++)
            p->ents[i].sort = -(int)sort_atom - 2;  /* encode name atom, decode later */
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* ---- effect-expression parser (§5.8) --------------------------------
 *
 *   expr   := term (('+'|'-') term)*
 *   term   := factor ('*' factor)*
 *   factor := '-' factor | INT | ('min'|'max') '(' expr ',' expr ')'
 *           | IDENT [ '(' arg (',' arg)* ')' ]        -- a numeric fluent read
 *           | '(' expr ')'
 * Returns a node index into p->exprs, or -1 on error. */

static int alloc_expr(parser *p, ex_kind k, int line, int col)
{
    if (p->nexprs >= MAX_EXPRS) {
        fail(p, line, col, "effect expression too complex (max %d nodes)", MAX_EXPRS);
        return -1;
    }
    int i = p->nexprs++;
    memset(&p->exprs[i], 0, sizeof p->exprs[i]);
    p->exprs[i].kind = k;
    p->exprs[i].lhs = p->exprs[i].rhs = -1;
    p->exprs[i].line = line;
    p->exprs[i].col = col;
    return i;
}

static int parse_expr(parser *p);

static int parse_factor(parser *p)
{
    if (p->cur.kind == TK_MINUS) {
        token m = p->cur; advance(p);
        int c = parse_factor(p);
        if (c < 0) return -1;
        int n = alloc_expr(p, EX_NEG, m.line, m.col);
        if (n < 0) return -1;
        p->exprs[n].lhs = c;
        return n;
    }
    if (p->cur.kind == TK_INT) {
        int n = alloc_expr(p, EX_CONST, p->cur.line, p->cur.col);
        if (n < 0) return -1;
        p->exprs[n].konst = p->cur.ival;
        advance(p);
        return n;
    }
    if (p->cur.kind == TK_LPAREN) {
        advance(p);
        int e = parse_expr(p);
        if (e < 0) return -1;
        if (!expect(p, TK_RPAREN)) return -1;
        return e;
    }
    if (p->cur.kind == TK_IDENT) {
        token id = p->cur;
        bool ismin = ident_is(id, "min"), ismax = ident_is(id, "max");
        advance(p);
        if ((ismin || ismax) && p->cur.kind == TK_LPAREN) {   /* min/max(a, b) */
            advance(p);
            int a = parse_expr(p);
            if (a < 0) return -1;
            if (!expect(p, TK_COMMA)) return -1;
            int b = parse_expr(p);
            if (b < 0) return -1;
            if (!expect(p, TK_RPAREN)) return -1;
            int n = alloc_expr(p, ismin ? EX_MIN : EX_MAX, id.line, id.col);
            if (n < 0) return -1;
            p->exprs[n].lhs = a;
            p->exprs[n].rhs = b;
            return n;
        }
        int n = alloc_expr(p, EX_LOAD, id.line, id.col);        /* fluent read */
        if (n < 0) return -1;
        p->exprs[n].pred = intern_tok(p, id);
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            for (;;) {
                if (p->cur.kind != TK_IDENT) {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected an argument name, found %s", d);
                    return -1;
                }
                if (p->exprs[n].nargs >= MAX_ARGS) {
                    fail(p, p->cur.line, p->cur.col, "too many arguments (max %d)", MAX_ARGS);
                    return -1;
                }
                p->exprs[n].args[p->exprs[n].nargs].name = intern_tok(p, p->cur);
                p->exprs[n].args[p->exprs[n].nargs].line = p->cur.line;
                p->exprs[n].args[p->exprs[n].nargs].col = p->cur.col;
                p->exprs[n].nargs++;
                advance(p);
                if (p->cur.kind == TK_COMMA) { advance(p); continue; }
                break;
            }
            if (!expect(p, TK_RPAREN)) return -1;
        }
        return n;
    }
    char d[64]; tok_desc(p->cur, d, sizeof d);
    fail(p, p->cur.line, p->cur.col,
         "expected a number, fluent, or '(' in an effect expression, found %s", d);
    return -1;
}

static int parse_term(parser *p)
{
    int l = parse_factor(p);
    if (l < 0) return -1;
    while (p->cur.kind == TK_STAR) {
        token o = p->cur; advance(p);
        int r = parse_factor(p);
        if (r < 0) return -1;
        int n = alloc_expr(p, EX_MUL, o.line, o.col);
        if (n < 0) return -1;
        p->exprs[n].lhs = l; p->exprs[n].rhs = r; l = n;
    }
    return l;
}

static int parse_expr(parser *p)
{
    int l = parse_term(p);
    if (l < 0) return -1;
    while (p->cur.kind == TK_PLUS || p->cur.kind == TK_MINUS) {
        ex_kind k = p->cur.kind == TK_PLUS ? EX_ADD : EX_SUB;
        token o = p->cur; advance(p);
        int r = parse_term(p);
        if (r < 0) return -1;
        int n = alloc_expr(p, k, o.line, o.col);
        if (n < 0) return -1;
        p->exprs[n].lhs = l; p->exprs[n].rhs = r; l = n;
    }
    return l;
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
    /* Postfix `'`: read this atom in the next state (§5.4). Parsed anywhere;
     * the semantic pass confines it to ramification bodies (and to boolean /
     * multi-valued fluents — primed numeric guards and judgments are the §5.8
     * stratification case, not yet supported). */
    if (p->cur.kind == TK_PRIME) { out->primed = true; advance(p); }
    /* A comparison operator makes this a numeric guard `f <op> n`; a `=` is
     * overloaded — `f = value` (multi-valued) vs `f = 12` (numeric equality),
     * disambiguated by whether an identifier or an integer follows. */
    world_cmp op = WORLD_CMP_EQ;
    bool cmp = true;
    switch (p->cur.kind) {
    case TK_LE: op = WORLD_CMP_LE; break;
    case TK_LT: op = WORLD_CMP_LT; break;
    case TK_GE: op = WORLD_CMP_GE; break;
    case TK_GT: op = WORLD_CMP_GT; break;
    default:    cmp = false; break;
    }
    if (cmp) {
        advance(p);
        if (!parse_int(p, &out->threshold)) return false;
        out->is_guard = true;
        out->cmp = op;
    } else if (p->cur.kind == TK_EQ) {
        advance(p);
        if (p->cur.kind == TK_IDENT) {         /* multi-valued: f = value */
            out->value = intern_tok(p, p->cur);
            advance(p);
        } else {                               /* numeric equality: f = 12 */
            if (!parse_int(p, &out->threshold)) return false;
            out->is_guard = true;
            out->cmp = WORLD_CMP_EQ;
        }
    } else if (p->cur.kind == TK_ASSIGN || p->cur.kind == TK_PLUSEQ ||
               p->cur.kind == TK_MINUSEQ) {    /* numeric effect (§5.8) */
        out->numop = p->cur.kind == TK_ASSIGN ? WORLD_OP_ASSIGN
                   : p->cur.kind == TK_PLUSEQ ? WORLD_OP_ADD
                                              : WORLD_OP_SUB;
        advance(p);
        int e = parse_expr(p);
        if (e < 0) return false;
        out->is_num_effect = true;
        out->expr_root = e;
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
        advance(p);
        if (ident_is(p->cur, "int")) {             /* `: int` numeric fluent */
            advance(p);
            if (p->cur.kind == TK_IN) {            /* `in lo..hi` clamp range */
                advance(p);
                if (!parse_int(p, &f->rmin)) return false;
                if (!expect(p, TK_DOTDOT)) return false;
                if (!parse_int(p, &f->rmax)) return false;
                if (f->rmax < f->rmin) {
                    fail(p, f->line, f->col,
                         "numeric range is empty: hi (%ld) is below lo (%ld)",
                         f->rmax, f->rmin);
                    return false;
                }
                f->has_range = true;
            }
            f->is_num = true;
            return true;
        }
        if (p->cur.kind != TK_LBRACE) {
            /* `: cell`, `: tile default …` — entity-domain/functional, later */
            fail(p, p->cur.line, p->cur.col,
                 "only `: int` and `: { v1, v2, … }` fluent domains are "
                 "supported (entity-domain fluents land later)");
            return false;
        }
        advance(p);                                /* '{' */
        f->is_mv = true;
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col, "expected a value name, found %s", d);
                return false;
            }
            if (f->nvalues >= MAX_DOMAIN) {
                fail(p, p->cur.line, p->cur.col,
                     "too many domain values (max %d)", MAX_DOMAIN);
                return false;
            }
            f->values[f->nvalues++] = intern_tok(p, p->cur);
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_RBRACE)) return false;
        if (f->nvalues < 2) {
            fail(p, f->line, f->col,
                 "a value domain needs at least two values");
            return false;
        }
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

/* rule := 'rule' IDENT [ params ] ':' conj ( OP atom [ 'unless' conj ]
 *                                          | 'causes' conj )
 * A `causes` clause (in place of a rule arrow) makes it a ramification: a step
 * rule with no action trigger, its body the match condition (§5.4, §11 M1). */
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

    /* `causes` instead of an arrow: this `rule` is a ramification. Re-home the
     * label, params, and body into the actions array (a trigger-less step
     * rule); the judgment-rule slot `r` stays scratch (nrules not bumped). */
    if (p->cur.kind == TK_CAUSES) {
        if (p->nactions >= MAX_ACTIONS) {
            fail(p, r->line, r->col, "too many actions (max %d)", MAX_ACTIONS);
            return;
        }
        ast_action *a = &p->actions[p->nactions];
        memset(a, 0, sizeof *a);
        memcpy(a->name, r->label, MAX_NAME);
        a->line = r->line;
        a->col = r->col;
        a->is_ramif = true;
        a->nvars = r->nvars;
        for (int i = 0; i < r->nvars; i++) a->vars[i] = r->vars[i];
        a->nreq = r->nbody;
        for (int b = 0; b < r->nbody; b++) a->requires[b] = r->body[b];
        advance(p);                                /* 'causes' */
        int ne = parse_conj(p, a->effects, MAX_BODY);
        if (ne < 0) return;
        a->neff = ne;
        p->nactions++;
        return;
    }

    switch (p->cur.kind) {
    case TK_ARROW:    r->kind = DL_STRICT;     break;
    case TK_FATARROW: r->kind = DL_DEFEASIBLE; break;
    case TK_SQARROW:  r->kind = DL_DEFEATER;   break;
    default: {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col,
             "expected a rule arrow ('->', '=>', '~>') or 'causes' "
             "(a ramification), found %s", d);
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
        pi->is_num = f->is_num;
        pi->is_mv = f->is_mv;
        pi->nvalues = f->nvalues;
        for (int k = 0; k < f->nvalues; k++) {
            pi->values[k] = f->values[k];
            for (int j = 0; j < k; j++)
                if (f->values[j] == f->values[k])
                    serr(p, f->line, f->col,
                         "duplicate value '%s' in the domain of '%s'",
                         intern_name(p->syms, f->values[k]),
                         intern_name(p->syms, f->pred));
        }
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

/* Every argument is a bound variable or a declared entity, with a sort check
 * against the fluent schema. Shared by atoms, effect targets, and fluent reads
 * inside effect expressions. */
static void check_pred_args(parser *p, uint32_t pred, pred_info *pi,
                            const ast_arg *args, int nargs,
                            var_bind *vars, int nvars, const char *ctx)
{
    for (int k = 0; k < nargs; k++) {
        const ast_arg *arg = &args[k];
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
                     k + 1, intern_name(p->syms, pred),
                     p->sorts[want].name, p->sorts[got].name);
        }
    }
}

/* Validate an effect-RHS expression tree (§5.8): every fluent read resolves to
 * a declared numeric fluent of matching arity with in-scope args. */
static void check_expr(parser *p, int e, var_bind *vars, int nvars)
{
    if (e < 0) return;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST:
        return;
    case EX_LOAD: {
        note_ref(p, n->pred, n->line, n->col);
        pred_info *pi = find_pred(p, n->pred);
        if (!pi || !pi->is_fluent || !pi->is_num) {
            serr(p, n->line, n->col,
                 "'%s' is read in an effect expression but is not a declared "
                 "numeric fluent (`%s : int`)",
                 intern_name(p->syms, n->pred), intern_name(p->syms, n->pred));
            return;
        }
        if (pi->arity != n->nargs) {
            serr(p, n->line, n->col, "'%s' takes %d argument%s but %d given",
                 intern_name(p->syms, n->pred), pi->arity,
                 pi->arity == 1 ? "" : "s", n->nargs);
            return;
        }
        check_pred_args(p, n->pred, pi, n->args, n->nargs, vars, nvars,
                        "an effect expression");
        return;
    }
    case EX_NEG:
        check_expr(p, n->lhs, vars, nvars);
        return;
    default:
        check_expr(p, n->lhs, vars, nvars);
        check_expr(p, n->rhs, vars, nvars);
        return;
    }
}

/* Validate one atom against the schema: predicate known, arity matches, and
 * every argument is a bound variable or a declared entity (with a sort check
 * for fluent atoms). `note` records condition refs for orphan analysis;
 * `in_effect` is true only for atoms in a `causes` clause, where numeric
 * effect operators (`:=`/`+=`/`-=`) are legal. `allow_prime` is true only in a
 * ramification body, the one context where a postfix `'` (next-state, §5.4) is
 * meaningful. */
static void check_atom(parser *p, ast_atom *at, var_bind *vars, int nvars,
                       bool note, bool in_effect, bool allow_prime, const char *ctx)
{
    if (note) note_ref(p, at->pred, at->line, at->col);
    if (at->primed) {
        if (!allow_prime) {
            serr(p, at->line, at->col,
                 "the next-state mark `'` is only allowed in a ramification "
                 "body (a `rule … causes …`), not in %s", ctx);
            return;
        }
        /* Deferred §5.8 stratification case: a primed numeric guard or a primed
         * judgment needs next-state arithmetic/derivation mid-fixpoint. Only a
         * boolean or multi-valued fluent read may prime for now. */
        if (at->is_guard) {
            serr(p, at->line, at->col,
                 "a primed numeric guard (`%s … '`) is not supported yet — it "
                 "needs the §5.8 primed-guard stratification; test the current "
                 "value instead", intern_name(p->syms, at->pred));
            return;
        }
        pred_info *pf = find_pred(p, at->pred);
        if (!pf || !pf->is_fluent) {
            serr(p, at->line, at->col,
                 "`%s'` primes a judgment (a derived conclusion in the next "
                 "state), not supported yet — it needs §5.8 stratification; "
                 "prime the fluents it is concluded from instead",
                 intern_name(p->syms, at->pred));
            return;
        }
    }
    pred_info *pi = find_pred(p, at->pred);
    if (pi && pi->arity != at->nargs) {
        serr(p, at->line, at->col,
             "'%s' takes %d argument%s but %d given",
             intern_name(p->syms, at->pred), pi->arity,
             pi->arity == 1 ? "" : "s", at->nargs);
        return;
    }
    /* numeric write discipline (§5.8): an effect operator assigns a numeric
     * fluent and is legal only in a `causes` clause. */
    if (at->is_num_effect) {
        if (!in_effect) {
            serr(p, at->line, at->col,
                 "an effect operator (`:=`/`+=`/`-=`) can only appear in a "
                 "`causes` clause");
            return;
        }
        if (!pi || !pi->is_fluent || !pi->is_num) {
            serr(p, at->line, at->col,
                 "'%s' is assigned numerically but is not a declared numeric "
                 "fluent (`%s : int`)",
                 intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
            return;
        }
        check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars, ctx);
        check_expr(p, at->expr_root, vars, nvars);
        return;
    }
    /* in a `causes` clause a numeric fluent is *written*, never compared:
     * `hp = 5`, `hp <= 0`, or a bare `hp` are all read-forms, not effects. */
    if (in_effect && (at->is_guard || (pi && pi->is_num))) {
        serr(p, at->line, at->col,
             "to change '%s' in a `causes` clause use an effect operator "
             "(`%s := …`, `+=`, `-=`), not a comparison",
             intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
        return;
    }
    /* numeric discipline: a guard needs a numeric fluent; a numeric fluent
     * must be read through a comparison, never as a bare or boolean atom. */
    if (at->is_guard) {
        if (!pi || !pi->is_fluent || !pi->is_num)
            serr(p, at->line, at->col,
                 "'%s' is compared numerically but is not a declared numeric "
                 "fluent (`%s : int`)",
                 intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
        return;
    }
    if (pi && pi->is_num) {
        serr(p, at->line, at->col,
             "'%s' is numeric — read it with a comparison (e.g. `%s <= 0`)",
             intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
        return;
    }
    /* multi-valued discipline: an MV fluent must be written `f = v`, a boolean
     * one must not; a value must belong to the declared domain. */
    if (pi && pi->is_mv && at->value == INTERN_NONE) {
        serr(p, at->line, at->col,
             "'%s' is multi-valued — write `%s = <value>`, not a bare atom",
             intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
        return;
    }
    if (at->value != INTERN_NONE) {
        if (!pi || !pi->is_fluent || !pi->is_mv) {
            serr(p, at->line, at->col,
                 "'%s = …' but '%s' is not a declared multi-valued fluent",
                 intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
            return;
        }
        bool in_domain = false;
        for (int i = 0; i < pi->nvalues; i++)
            if (pi->values[i] == at->value) { in_domain = true; break; }
        if (!in_domain)
            serr(p, at->line, at->col,
                 "'%s' is not a value of '%s'",
                 intern_name(p->syms, at->value), intern_name(p->syms, at->pred));
    }
    check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars, ctx);
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
            check_atom(p, &r->body[b], r->vars, r->nvars, true, false, false, "a rule body");
        check_atom(p, &r->head, r->vars, r->nvars, false, false, false, "a rule head");
        if (r->head.value != INTERN_NONE)
            serr(p, r->head.line, r->head.col,
                 "concluding a multi-valued value ('%s = %s') from a judgment "
                 "rule is not supported yet — it needs the §5.7 family "
                 "reification; set the value with an `action … causes` instead",
                 intern_name(p->syms, r->head.pred),
                 intern_name(p->syms, r->head.value));
        if (r->head.is_guard)
            serr(p, r->head.line, r->head.col,
                 "a rule cannot conclude a numeric comparison — guards are "
                 "read-only inputs derived from the value store (§5.8)");
        for (int b = 0; b < r->nguard; b++)
            check_atom(p, &r->guard[b], r->vars, r->nvars, true, false, false, "an `unless` guard");
        check_safety(p, r);
        for (int j = i + 1; j < p->nrules; j++)
            if (strcmp(r->label, p->rules[j].label) == 0)
                serr(p, p->rules[j].line, p->rules[j].col,
                     "duplicate rule label '%s'", r->label);
    }

    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        resolve_vars(p, a->vars, a->nvars, a->is_ramif ? "a ramification" : "an action");
        const char *bctx = a->is_ramif ? "a ramification body" : "a `requires` clause";
        for (int b = 0; b < a->nreq; b++)
            check_atom(p, &a->requires[b], a->vars, a->nvars, true, false, a->is_ramif, bctx);
        for (int b = 0; b < a->neff; b++) {
            check_atom(p, &a->effects[b], a->vars, a->nvars, false, true, false, "a `causes` clause");
            if (a->effects[b].value != INTERN_NONE && a->effects[b].neg)
                serr(p, a->effects[b].line, a->effects[b].col,
                     "a negative multi-valued effect ('~(%s = %s)') is not "
                     "supported yet — it needs the §5.7 family reification; "
                     "assign the intended value instead",
                     intern_name(p->syms, a->effects[b].pred),
                     intern_name(p->syms, a->effects[b].value));
        }
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
        check_atom(p, a, NULL, 0, false, false, false, "an init fact");
        if (a->is_guard && a->cmp != WORLD_CMP_EQ)
            serr(p, a->line, a->col,
                 "init sets a numeric fluent's value with `=` (e.g. `%s = 10`), "
                 "not a comparison", intern_name(p->syms, a->pred));
        for (int k = 0; k < a->nargs; k++)
            if (find_entity(p, a->args[k].name) < 0)
                serr(p, a->args[k].line, a->args[k].col,
                     "init argument '%s' must be a declared entity",
                     intern_name(p->syms, a->args[k].name));
    }
}

/* ---- grounding: emit ground rules into world_* ---------------------- */

/* Write the ground term "pred(e1,e2)" (bare "pred" at arity 0) into buf. */
static int build_term(parser *p, uint32_t pred, const uint32_t *args, int n,
                      char *buf, size_t cap)
{
    int off = snprintf(buf, cap, "%s", intern_name(p->syms, pred));
    if (n == 0) return off;
    off += snprintf(buf + off, cap - (size_t)off, "(");
    for (int i = 0; i < n && off < (int)cap; i++)
        off += snprintf(buf + off, cap - (size_t)off, "%s%s",
                        i ? "," : "", intern_name(p->syms, args[i]));
    if (off < (int)cap) off += snprintf(buf + off, cap - (size_t)off, ")");
    return off;
}

/* Build the interned ground atom "pred(e1,e2)" (bare "pred" at arity 0). */
static uint32_t ground_pred(parser *p, uint32_t pred, const uint32_t *args, int n)
{
    if (n == 0) return pred;
    char buf[MAX_GROUND];
    build_term(p, pred, args, n, buf, sizeof buf);
    return intern_id(p->syms, buf);
}

/* Build the interned value-atom "pred(e1,e2)=v" — a multi-valued fluent's
 * value erases to this boolean atom (§5.7). */
static uint32_t ground_mv_atom(parser *p, uint32_t pred, const uint32_t *args,
                               int n, uint32_t value)
{
    char buf[MAX_GROUND];
    int off = build_term(p, pred, args, n, buf, sizeof buf);
    if (off < (int)sizeof buf)
        snprintf(buf + off, sizeof buf - (size_t)off, "=%s",
                 intern_name(p->syms, value));
    return intern_id(p->syms, buf);
}

static const char *cmp_spelling(world_cmp op)
{
    switch (op) {
    case WORLD_CMP_LE: return "<=";
    case WORLD_CMP_LT: return "<";
    case WORLD_CMP_GE: return ">=";
    case WORLD_CMP_GT: return ">";
    case WORLD_CMP_EQ: return "=";
    }
    return "?";
}

/* Build the interned guard atom "pred(e1,e2)<op>n" — a numeric comparison
 * erases to this boolean landmark atom, asserted closed-world from the value
 * store each evaluation (§5.8). */
static uint32_t ground_guard_atom(parser *p, uint32_t pred, const uint32_t *args,
                                  int n, world_cmp op, long threshold)
{
    char buf[MAX_GROUND];
    int off = build_term(p, pred, args, n, buf, sizeof buf);
    if (off < (int)sizeof buf)
        snprintf(buf + off, sizeof buf - (size_t)off, "%s%ld",
                 cmp_spelling(op), threshold);
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

/* Fold a fully-constant expression subtree to its value; false if it reads a
 * fluent (EX_LOAD), which must stay dynamic (§5.8: "constant folding, then
 * bytecode"). */
static bool expr_fold(parser *p, int e, long *out)
{
    ex_node *n = &p->exprs[e];
    long a, b;
    switch (n->kind) {
    case EX_CONST: *out = n->konst; return true;
    case EX_LOAD:  return false;
    case EX_NEG:
        if (!expr_fold(p, n->lhs, &a)) return false;
        *out = -a; return true;
    default:
        if (!expr_fold(p, n->lhs, &a) || !expr_fold(p, n->rhs, &b)) return false;
        switch (n->kind) {
        case EX_ADD: *out = a + b; break;
        case EX_SUB: *out = a - b; break;
        case EX_MUL: *out = a * b; break;
        case EX_MIN: *out = a < b ? a : b; break;
        case EX_MAX: *out = a > b ? a : b; break;
        default: return false;
        }
        return true;
    }
}

/* Emit RPN bytecode for expr node `e` under `binding`, folding constant
 * subtrees and resolving fluent reads to their ground value-store atom. */
static void emit_expr(parser *p, int e, var_bind *vars, int nvars,
                      const uint32_t *binding, expr_ins *code, int *pos)
{
    long cv;
    if (expr_fold(p, e, &cv)) {
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_CONST; code[(*pos)++].arg = cv; }
        return;
    }
    ex_node *n = &p->exprs[e];
    if (n->kind == EX_LOAD) {
        uint32_t args[MAX_ARGS];
        for (int k = 0; k < n->nargs; k++)
            args[k] = resolve_arg(vars, nvars, binding, n->args[k]);
        uint32_t g = ground_pred(p, n->pred, args, n->nargs);
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_LOAD; code[(*pos)++].arg = (long)g; }
        return;
    }
    if (n->kind == EX_NEG) {
        emit_expr(p, n->lhs, vars, nvars, binding, code, pos);
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_NEG; code[(*pos)++].arg = 0; }
        return;
    }
    emit_expr(p, n->lhs, vars, nvars, binding, code, pos);
    emit_expr(p, n->rhs, vars, nvars, binding, code, pos);
    expr_op op = n->kind == EX_ADD ? EXPR_ADD : n->kind == EX_SUB ? EXPR_SUB
               : n->kind == EX_MUL ? EXPR_MUL : n->kind == EX_MIN ? EXPR_MIN
                                                                  : EXPR_MAX;
    if (*pos < MAX_CODE) { code[*pos].op = op; code[(*pos)++].arg = 0; }
}

static dl_lit ground_lit(parser *p, ast_atom *at, var_bind *vars, int nvars,
                         const uint32_t *binding)
{
    uint32_t args[MAX_ARGS];
    for (int k = 0; k < at->nargs; k++)
        args[k] = resolve_arg(vars, nvars, binding, at->args[k]);
    uint32_t g;
    if (at->is_guard) {                            /* numeric landmark guard */
        uint32_t term = ground_pred(p, at->pred, args, at->nargs);
        g = ground_guard_atom(p, at->pred, args, at->nargs, at->cmp, at->threshold);
        world_add_guard(p->w, g, term, at->cmp, at->threshold);
    } else if (at->value != INTERN_NONE) {
        g = ground_mv_atom(p, at->pred, args, at->nargs, at->value);
    } else {
        g = ground_pred(p, at->pred, args, at->nargs);
    }
    return at->neg ? dl_neg(g) : dl_pos(g);
}

/* Readable instance name for why-traces: "label[X=hero,Y=key]". */
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

/* "srcname:line" — the provenance suffix rendered in a why-trace (§6.3), so an
 * author can jump from a generated rule to the source construct it came from. */
static const char *prov_str(parser *p, int line, char *buf, size_t n)
{
    snprintf(buf, n, "%s:%d", p->srcname ? p->srcname : "<story>", line);
    return buf;
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
        char pbuf[MAX_NAME + 24];
        const char *decl = prov_str(p, f->line, pbuf, sizeof pbuf);
        for (long idx = 0; idx < total; idx++) {
            decode_binding(p, vb, f->nargs, idx, binding);
            if (f->is_num)                         /* value-store slot, not an atom */
                world_declare_num(p->w, ground_pred(p, f->pred, binding, f->nargs),
                                  f->rmin, f->rmax, f->has_range);
            else if (f->is_mv) {                   /* one boolean atom per value */
                for (int v = 0; v < f->nvalues; v++) {
                    uint32_t a = ground_mv_atom(p, f->pred, binding, f->nargs,
                                                f->values[v]);
                    world_declare_fluent(p->w, a);
                    world_set_fluent_prov(p->w, a, decl);
                }
            } else {
                uint32_t a = ground_pred(p, f->pred, binding, f->nargs);
                world_declare_fluent(p->w, a);
                world_set_fluent_prov(p->w, a, decl);
            }
        }
    }
}

static void ground_inits(parser *p)
{
    for (int i = 0; i < p->ninits; i++) {
        ast_atom *a = &p->inits[i];
        uint32_t args[MAX_ARGS];
        for (int k = 0; k < a->nargs; k++) args[k] = a->args[k].name;
        if (a->is_guard) {                         /* numeric: `f = n` sets the store */
            world_set_num(p->w, ground_pred(p, a->pred, args, a->nargs), a->threshold);
            continue;
        }
        uint32_t atom = a->value != INTERN_NONE
            ? ground_mv_atom(p, a->pred, args, a->nargs, a->value)
            : ground_pred(p, a->pred, args, a->nargs);
        world_set(p->w, atom, true);               /* siblings stay closed-world false */
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
        char pbuf[MAX_NAME + 24];
        world_set_rule_prov(p->w, r->insts[idx].handle,
                            prov_str(p, r->line, pbuf, sizeof pbuf));

        /* `unless G` sugars to a defeater blocking this instance's head:
         * G ~> ~head (DESIGN.md §6), reinstated whenever the guard fails. */
        if (r->has_guard) {
            dl_lit guard[MAX_BODY];
            for (int b = 0; b < r->nguard; b++)
                guard[b] = ground_lit(p, &r->guard[b], r->vars, r->nvars, binding);
            char gname[MAX_GROUND + 8];
            snprintf(gname, sizeof gname, "%s.unless", name);
            int gh = world_add_rule(p->w, gname, DL_DEFEATER, dl_complement(head),
                                    guard, r->nguard);
            world_set_rule_prov(p->w, gh, prov_str(p, r->line, pbuf, sizeof pbuf));
        }
    }
}

static void ground_action(parser *p, ast_action *a)
{
    bool of = false;
    long total = instance_count(p, a->vars, a->nvars, &of);
    if (of) {
        warn(p, a->line, a->col, "%s '%s' grounds to more than %d instances",
             a->is_ramif ? "ramification" : "action", a->name, MAX_INSTANCES);
        return;
    }
    if (total == 0) return;

    uint32_t binding[MAX_ARGS];
    for (long idx = 0; idx < total; idx++) {
        decode_binding(p, a->vars, a->nvars, idx, binding);
        char aname[MAX_GROUND];
        inst_name(p, aname, sizeof aname, a->name, a->vars, a->nvars, binding);
        /* A ramification has no trigger (act = INTERN_NONE): it fires in any
         * step whose state matches its body. An action's trigger atom is the
         * ground action term "name(e1,..)" (bare at arity 0). */
        uint32_t act = INTERN_NONE;
        if (!a->is_ramif) {
            uint32_t actargs[MAX_ARGS];
            for (int k = 0; k < a->nvars; k++) actargs[k] = binding[k];
            uint32_t nameatom = intern_id(p->syms, a->name);
            act = ground_pred(p, nameatom, actargs, a->nvars);
        }
        step_cond conds[MAX_BODY];
        for (int b = 0; b < a->nreq; b++) {
            conds[b].lit = ground_lit(p, &a->requires[b], a->vars, a->nvars, binding);
            /* Bare atom = current state; a postfix `'` (ramification bodies
             * only) reads the next state (§5.4). Action `requires` are always
             * current-state — the parser forbids `'` there. */
            conds[b].primed = a->requires[b].primed;
        }
        /* A multi-valued assignment `f = v` expands to the whole family: the
         * chosen value plus a negation of every sibling, so exactly one value
         * holds next tick and a flip-flop against a sibling is a contested step
         * (§5.7). A boolean effect is a single literal. */
        dl_lit eff[MAX_BODY];
        int ne = 0;
        for (int b = 0; b < a->neff && ne < MAX_BODY; b++) {
            ast_atom *e = &a->effects[b];
            if (e->is_num_effect)                  /* numeric: emitted below */
                continue;
            if (e->value == INTERN_NONE) {         /* boolean effect */
                eff[ne++] = ground_lit(p, e, a->vars, a->nvars, binding);
                continue;
            }
            uint32_t args[MAX_ARGS];
            for (int k = 0; k < e->nargs; k++)
                args[k] = resolve_arg(a->vars, a->nvars, binding, e->args[k]);
            pred_info *pi = find_pred(p, e->pred);
            eff[ne++] = dl_pos(ground_mv_atom(p, e->pred, args, e->nargs, e->value));
            for (int v = 0; v < pi->nvalues && ne < MAX_BODY; v++)
                if (pi->values[v] != e->value)
                    eff[ne++] = dl_neg(ground_mv_atom(p, e->pred, args, e->nargs,
                                                      pi->values[v]));
        }
        int h = world_add_step_rule(p->w, aname, act, conds, a->nreq, eff, ne);
        char pbuf[MAX_NAME + 24];
        world_set_step_prov(p->w, h, prov_str(p, a->line, pbuf, sizeof pbuf));

        /* numeric effects (§5.8): ground the target value-store atom and
         * compile the RHS expression to VM bytecode for this instance. */
        for (int b = 0; b < a->neff; b++) {
            ast_atom *e = &a->effects[b];
            if (!e->is_num_effect) continue;
            uint32_t nargs[MAX_ARGS];
            for (int k = 0; k < e->nargs; k++)
                nargs[k] = resolve_arg(a->vars, a->nvars, binding, e->args[k]);
            uint32_t num = ground_pred(p, e->pred, nargs, e->nargs);
            expr_ins code[MAX_CODE];
            int nc = 0;
            emit_expr(p, e->expr_root, a->vars, a->nvars, binding, code, &nc);
            world_add_num_effect(p->w, h, num, e->numop, code, nc);
        }
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

/* ---- lane grounding (the DoD thesis, increments 2a + partial coverage) ----
 *
 * Emit the lane-eligible slice of the judgment program as per-sort N-lane dl_col
 * families: a predicate over sort S becomes a column over S's entities, a
 * single-variable rule over S becomes ONE schema rule run bit-parallel across 64
 * lanes per word — not grounded per entity. The rest of the program (numeric,
 * MV, multi-var, guarded, cross-sort) stays on the N=1 judgment family; a query
 * routes to whichever holds its atom. Lane families are validated against the
 * N=1 path (world_lanes_check) — the same differential discipline test_col
 * applies to dl vs dl_col.
 *
 * A predicate may lane only if it is *dependency-closed*: every rule concluding
 * it — and, since attackers must resolve together, its complement — is
 * lane-eligible, and every predicate they read is itself lane-clean. Anything
 * that fails taints the predicate, and taint propagates to its dependents and
 * across superiority edges that would otherwise split a conflict across the
 * lane/N=1 boundary. What survives is a closed subset that derives identically
 * either way. Per-sort axis, forced (one variable = one axis): no plan, no cost
 * model — the plan is a pure local function of each rule's text. */

static bool lane_atom_ok(parser *p, const ast_atom *a, int S, uint32_t var,
                         bool is_head)
{
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect)
        return false;                              /* MV / numeric: out */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num)
        return false;
    if (pi->arity == 0)
        return !is_head;                           /* globals: broadcast body only */
    if (pi->arity != 1 || pi->argsort[0] != S)
        return false;
    return a->nargs == 1 && a->args[0].name == var; /* arg is the quantified var */
}

/* A rule can lane iff it is single-variable and every atom — body, head, and any
 * `unless` guard — is unary over that one sort (arg = the variable) or an
 * arity-0 global input. An `unless` guard lowers to a defeater `guard ~> ~head`
 * (§6), emitted as its own schema rule, so its atoms must lane too. */
static bool rule_eligible(parser *p, ast_rule *r)
{
    if (r->nvars != 1)
        return false;
    int S = r->vars[0].sort;
    uint32_t var = r->vars[0].name;
    if (!lane_atom_ok(p, &r->head, S, var, true))
        return false;
    for (int b = 0; b < r->nbody; b++)
        if (!lane_atom_ok(p, &r->body[b], S, var, false))
            return false;
    for (int g = 0; g < r->nguard; g++)
        if (!lane_atom_ok(p, &r->guard[g], S, var, false))
            return false;
    return true;
}

static int pred_idx(parser *p, uint32_t pred)
{
    pred_info *pi = find_pred(p, pred);
    return pi ? (int)(pi - p->preds) : -1;
}

static int rule_index(parser *p, const char *label)
{
    for (int i = 0; i < p->nrules; i++)
        if (strcmp(p->rules[i].label, label) == 0) return i;
    return -1;
}

/* The taint fixpoint: taint[pi] true iff predicate pi cannot lane. Polarity is
 * merged (a head `~P` shares P's registry entry), so P and its attackers taint
 * together. */
static void compute_taint(parser *p, bool *taint)
{
    for (int i = 0; i < p->npreds; i++) taint[i] = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < p->nrules; i++) {
            ast_rule *r = &p->rules[i];
            int hp = pred_idx(p, r->head.pred);
            if (hp < 0 || taint[hp]) continue;
            bool bad = !rule_eligible(p, r);
            for (int b = 0; b < r->nbody && !bad; b++) {
                int bp = pred_idx(p, r->body[b].pred);
                if (bp >= 0 && p->preds[bp].is_head && taint[bp])
                    bad = true;                    /* reads a tainted conclusion */
            }
            for (int g = 0; g < r->nguard && !bad; g++) {
                int gp = pred_idx(p, r->guard[g].pred);
                if (gp >= 0 && p->preds[gp].is_head && taint[gp])
                    bad = true;                    /* the defeater reads a tainted pred */
            }
            if (bad) { taint[hp] = true; changed = true; }
        }
        /* a superiority edge must not split a conflict across the boundary */
        for (int s = 0; s < p->nsups; s++) {
            int ri = rule_index(p, p->sups[s].a), rj = rule_index(p, p->sups[s].b);
            if (ri < 0 || rj < 0) continue;
            int pi = pred_idx(p, p->rules[ri].head.pred);
            int pj = pred_idx(p, p->rules[rj].head.pred);
            if (pi < 0 || pj < 0 || taint[pi] == taint[pj]) continue;
            taint[pi] = taint[pj] = true;
            changed = true;
        }
    }
}

/* Emit one N-lane family for the untainted rules over sort S (if any). */
static void emit_sort_lanes(parser *p, int S, const bool *taint)
{
    int nent = domain_size(p, S);
    if (nent == 0)
        return;

    /* the laned rules: untainted, single-variable, head over S */
    int laned[MAX_RULES], nlaned = 0;
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        int hp = pred_idx(p, r->head.pred);
        if (hp < 0 || taint[hp] || r->nvars != 1 || r->vars[0].sort != S)
            continue;
        laned[nlaned++] = i;
    }
    if (nlaned == 0)
        return;

    /* distinct predicates (head + bodies) as family-local atoms */
    uint32_t preds[MAX_PREDS];
    bool pf[MAX_PREDS];
    int npred = 0;
    for (int li = 0; li < nlaned; li++) {
        ast_rule *r = &p->rules[laned[li]];
        const ast_atom *atoms[1 + 2 * MAX_BODY];
        int na = 0;
        atoms[na++] = &r->head;
        for (int b = 0; b < r->nbody; b++)  atoms[na++] = &r->body[b];
        for (int g = 0; g < r->nguard; g++) atoms[na++] = &r->guard[g];
        for (int k = 0; k < na; k++) {
            uint32_t pr = atoms[k]->pred;
            int found = -1;
            for (int j = 0; j < npred; j++) if (preds[j] == pr) { found = j; break; }
            if (found < 0) {
                if (npred >= MAX_PREDS) return;
                preds[npred] = pr;
                pf[npred] = find_pred(p, pr)->is_fluent;
                npred++;
            }
        }
    }

    dlcol *f = dlcol_new(npred, nent);
    for (int a = 0; a < npred; a++)
        dlcol_set_atom_name(f, (uint32_t)a, intern_name(p->syms, preds[a]));

    int schema_id[MAX_RULES];                      /* rule index -> schema id */
    for (int i = 0; i < p->nrules; i++) schema_id[i] = -1;
    for (int li = 0; li < nlaned; li++) {
        ast_rule *r = &p->rules[laned[li]];
        int hl = -1;
        for (int j = 0; j < npred; j++) if (preds[j] == r->head.pred) { hl = j; break; }
        dl_lit head = { (uint32_t)hl, r->head.neg };
        dl_lit body[MAX_BODY];
        for (int b = 0; b < r->nbody; b++) {
            int bl = -1;
            for (int j = 0; j < npred; j++)
                if (preds[j] == r->body[b].pred) { bl = j; break; }
            body[b] = (dl_lit){ (uint32_t)bl, r->body[b].neg };
        }
        char pbuf[MAX_NAME + 24];
        prov_str(p, r->line, pbuf, sizeof pbuf);
        int h = dlcol_add_rule(f, r->label, r->kind, head, body, r->nbody);
        dlcol_set_prov(f, h, pbuf);
        schema_id[laned[li]] = h;

        /* `unless G` lowers to a defeater `G ~> ~head` (§6), one schema rule run
         * across all lanes — the engine's exception mechanism, bit-parallel. */
        if (r->has_guard) {
            dl_lit dhead = { (uint32_t)hl, !r->head.neg };
            dl_lit guard[MAX_BODY];
            for (int g = 0; g < r->nguard; g++) {
                int gl = -1;
                for (int j = 0; j < npred; j++)
                    if (preds[j] == r->guard[g].pred) { gl = j; break; }
                guard[g] = (dl_lit){ (uint32_t)gl, r->guard[g].neg };
            }
            char gname[MAX_NAME + 8];
            snprintf(gname, sizeof gname, "%s.unless", r->label);
            int gh = dlcol_add_rule(f, gname, DL_DEFEATER, dhead, guard, r->nguard);
            dlcol_set_prov(f, gh, pbuf);
        }
    }
    for (int s = 0; s < p->nsups; s++) {
        int wi = rule_index(p, p->sups[s].a), li = rule_index(p, p->sups[s].b);
        if (wi >= 0 && li >= 0 && schema_id[wi] >= 0 && schema_id[li] >= 0)
            dlcol_add_sup(f, schema_id[wi], schema_id[li]);
    }

    /* (predicate-local, lane) -> named ground atom, for facts + the differential
     * check; a global (arity 0) broadcasts the same atom to every lane */
    uint32_t *ground = malloc((size_t)npred * (size_t)nent * sizeof *ground);
    for (int a = 0; a < npred; a++) {
        pred_info *pi = find_pred(p, preds[a]);
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            ground[(size_t)a * nent + e] =
                pi->arity == 0 ? preds[a] : ground_pred(p, preds[a], &ent, 1);
        }
    }
    world_add_lane_family(p->w, f, npred, nent, 1, ground, pf, NULL);
    free(ground);
}

/* ---- the join matcher: multi-variable rules (M3) ----
 *
 * A rule over more than one variable has no single forced lane axis, so the
 * compiler chooses one — structurally, never from cardinality (the
 * never-cost-based rule we settled on): the FIRST variable is the lane axis, the
 * rest are iterated. The predicates slice per iterated assignment; each slice is
 * a single-var lane family solved bit-parallel over the lane axis (lane one,
 * loop the others). For two variables that loop is one sort; for K variables it
 * is the cartesian product of the K-1 non-lane sorts, flattened into the
 * family's `niter` index — so this reuses the same family API, and world_query's
 * per-iteration routing, unchanged. Constraint: every body/guard predicate a
 * BASE fluent (derived-body joins are a later widening). Each such rule gets its
 * own island family — validated against N=1 (world_lanes_check), and routed for
 * the atoms that name a full assignment (the relational head + full-arity bodies). */

/* An atom in a multi-var rule reduces, per fixed iteration, to a lane column: its
 * args must each be one of the rule's variables (any arity/order) or none
 * (global). `roles[k]` receives the variable index the k-th arg binds — 0 for
 * the lane var, 1..nvars-1 for an iterated var. */
static bool join_atom_ok(parser *p, const ast_atom *a, const var_bind *vars,
                         int nvars, bool is_head, int *roles)
{
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect)
        return false;
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num || pi->arity != a->nargs)
        return false;
    (void)is_head;   /* a derived body/guard pred is allowed: it imports (§5.5) */
    for (int k = 0; k < a->nargs; k++) {
        int rho = -1;
        for (int v = 0; v < nvars; v++)
            if (a->args[k].name == vars[v].name) { rho = v; break; }
        if (rho < 0) return false;                 /* constant / other variable */
        roles[k] = rho;
    }
    return true;
}

static void emit_join_family(parser *p, ast_rule *r)
{
    int Sl = r->vars[0].sort;
    int nent = domain_size(p, Sl);
    if (nent == 0)
        return;

    /* the iterated axes: vars 1..nvars-1, their cartesian product flattened into
     * `niter`. vsize[v] is var v's domain size (least-significant last in the
     * mixed-radix decode below); vsize[0] is unused (the lane axis is nent). */
    int vsize[MAX_ARGS];
    long niter = 1;
    for (int v = 1; v < r->nvars; v++) {
        vsize[v] = domain_size(p, r->vars[v].sort);
        if (vsize[v] == 0)
            return;
        niter *= vsize[v];
    }

    /* every atom (body, head, guard) must reduce to a lane column */
    const ast_atom *ats[1 + 2 * MAX_BODY];
    int roleslot[1 + 2 * MAX_BODY][MAX_ARGS];
    int nat = 0;
    ats[nat] = &r->head;
    if (!join_atom_ok(p, &r->head, r->vars, r->nvars, true, roleslot[nat])) return;
    nat++;
    for (int b = 0; b < r->nbody; b++) {
        ats[nat] = &r->body[b];
        if (!join_atom_ok(p, &r->body[b], r->vars, r->nvars, false, roleslot[nat])) return;
        nat++;
    }
    for (int g = 0; g < r->nguard; g++) {
        ats[nat] = &r->guard[g];
        if (!join_atom_ok(p, &r->guard[g], r->vars, r->nvars, false, roleslot[nat])) return;
        nat++;
    }

    /* distinct predicates -> family-local atoms (arg pattern from first use) */
    uint32_t preds[MAX_PREDS];
    bool pf[MAX_PREDS];
    int prole[MAX_PREDS][MAX_ARGS], pnarg[MAX_PREDS], npred = 0;
    for (int k = 0; k < nat; k++) {
        uint32_t pr = ats[k]->pred;
        int found = -1;
        for (int j = 0; j < npred; j++) if (preds[j] == pr) { found = j; break; }
        if (found < 0) {
            if (npred >= MAX_PREDS) return;
            preds[npred] = pr;
            pf[npred] = find_pred(p, pr)->is_fluent;
            pnarg[npred] = ats[k]->nargs;
            for (int m = 0; m < ats[k]->nargs; m++) prole[npred][m] = roleslot[k][m];
            npred++;
        }
    }

    int local_of[1 + 2 * MAX_BODY];
    for (int k = 0; k < nat; k++)
        for (int j = 0; j < npred; j++)
            if (preds[j] == ats[k]->pred) { local_of[k] = j; break; }

    /* classify locals: the head (local_of[0]) is concluded here; a non-fluent
     * body/guard pred is DERIVED elsewhere and imported (its verdict injected
     * per cell at solve time); everything else is a base fluent. */
    bool pimport[MAX_PREDS];
    for (int j = 0; j < npred; j++)
        pimport[j] = !pf[j] && j != local_of[0];

    dlcol *f = dlcol_new(npred, nent);
    for (int a = 0; a < npred; a++)
        dlcol_set_atom_name(f, (uint32_t)a, intern_name(p->syms, preds[a]));

    dl_lit head = { (uint32_t)local_of[0], r->head.neg };
    dl_lit body[MAX_BODY];
    for (int b = 0; b < r->nbody; b++)
        body[b] = (dl_lit){ (uint32_t)local_of[1 + b], r->body[b].neg };
    char pbuf[MAX_NAME + 24];
    prov_str(p, r->line, pbuf, sizeof pbuf);
    int h = dlcol_add_rule(f, r->label, r->kind, head, body, r->nbody);
    dlcol_set_prov(f, h, pbuf);
    if (r->has_guard) {
        dl_lit dhead = { (uint32_t)local_of[0], !r->head.neg };
        dl_lit guard[MAX_BODY];
        for (int g = 0; g < r->nguard; g++)
            guard[g] = (dl_lit){ (uint32_t)local_of[1 + r->nbody + g], r->guard[g].neg };
        char gname[MAX_NAME + 8];
        snprintf(gname, sizeof gname, "%s.unless", r->label);
        int gh = dlcol_add_rule(f, gname, DL_DEFEATER, dhead, guard, r->nguard);
        dlcol_set_prov(f, gh, pbuf);
    }

    /* ground[(local*niter + it)*nent + e]: substitute the lane entity for role-0
     * args, and for each iterated role v the entity picked out of var v's domain
     * by the iteration `it` (decoded mixed-radix over the non-lane sorts). */
    uint32_t *ground = malloc((size_t)npred * (size_t)niter * nent * sizeof *ground);
    for (int a = 0; a < npred; a++)
        for (long it = 0; it < niter; it++) {
            /* decode `it` into a per-iterated-var entity index */
            int vidx[MAX_ARGS];
            long rem = it;
            for (int v = r->nvars - 1; v >= 1; v--) {
                vidx[v] = (int)(rem % vsize[v]);
                rem /= vsize[v];
            }
            for (int e = 0; e < nent; e++) {
                uint32_t args[MAX_ARGS];
                for (int m = 0; m < pnarg[a]; m++) {
                    int rho = prole[a][m];
                    args[m] = rho == 0 ? domain_at(p, Sl, e)
                                       : domain_at(p, r->vars[rho].sort, vidx[rho]);
                }
                ground[((size_t)a * niter + it) * nent + e] =
                    ground_pred(p, preds[a], args, pnarg[a]);
            }
        }
    world_add_lane_family(p->w, f, npred, nent, (int)niter, ground, pf, pimport);
    free(ground);
}

/* ---- step lanes: the transition layer, bit-parallel (M3, thesis) ----
 *
 * The judgment half of the engine lanes "what's true"; this lanes "what happens
 * next". The step theory — generated inertia (f => f', ~f => ~f') plus causal
 * rules and ramifications, causal superior to inertia — becomes ONE dl_col over
 * a single lane sort, so a transition is solved once across all entities instead
 * of grounded per entity into distinct atoms. This is the biggest thesis payoff:
 * in an RTS the per-tick transition runs for everyone, every tick.
 *
 * First cut, conservative bail (mirrors emit_join_family): a homogeneous
 * single-sort boolean step world — every fluent boolean/arity-1 over one sort S,
 * no judgment rules, every action/ramification single-var over S with boolean
 * requires/effects (no numeric, MV, globals, or `unless`). Built and validated
 * against the N=1 step family (world_step_lanes_check) but not yet routed through
 * world_step, the same prototype-before-adopt path the judgment lanes took. */

/* Index of a fluent pred atom in the family's fluent list (all validated to be
 * present before this is called). */
static int step_fidx(parser *p, const int *fpred, int nf, uint32_t pred)
{
    for (int i = 0; i < nf; i++)
        if (p->preds[fpred[i]].pred == pred) return i;
    return -1;
}

/* A boolean fluent read/write of the action's own variable over sort S. */
static bool step_atom_ok(parser *p, const ast_atom *a, int S, uint32_t var)
{
    if (a->is_guard || a->is_num_effect || a->value != INTERN_NONE)
        return false;                              /* numeric guard / MV: out */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || !pi->is_fluent || pi->is_mv || pi->is_num || pi->arity != 1)
        return false;
    if (pi->argsort[0] != S)
        return false;
    return a->nargs == 1 && a->args[0].name == var;
}

static void emit_step_lanes(parser *p)
{
    if (p->nrules != 0)                             /* pure inertia+causal for now */
        return;

    /* the lane sort S: every boolean fluent must be arity-1 over one shared sort;
     * a numeric/MV/global/multi-sort fluent bails the whole family. */
    int S = -1, fpred[MAX_PREDS], nf = 0;
    for (int i = 0; i < p->npreds; i++) {
        pred_info *pi = &p->preds[i];
        if (!pi->is_fluent)
            continue;
        if (pi->is_num || pi->is_mv || pi->arity != 1)
            return;
        if (S < 0) S = pi->argsort[0];
        else if (pi->argsort[0] != S) return;      /* multi-sort: bail */
        fpred[nf++] = i;                            /* index into p->preds */
    }
    if (nf == 0 || S < 0)
        return;
    int nent = domain_size(p, S);
    if (nent == 0)
        return;

    /* validate every action/ramification, and collect the distinct trigger
     * predicates of the (non-ramification) actions */
    uint32_t apred[MAX_ACTIONS];
    int na = 0;
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        if (a->nvars != 1 || a->vars[0].sort != S)
            return;
        uint32_t var = a->vars[0].name;
        for (int b = 0; b < a->nreq; b++)
            if (!step_atom_ok(p, &a->requires[b], S, var)) return;
        for (int b = 0; b < a->neff; b++)
            if (!step_atom_ok(p, &a->effects[b], S, var)) return;
        if (!a->is_ramif) {
            uint32_t tr = intern_id(p->syms, a->name);
            int found = -1;
            for (int j = 0; j < na; j++) if (apred[j] == tr) { found = j; break; }
            if (found < 0) { if (na >= MAX_ACTIONS) return; apred[na++] = tr; }
        }
    }

    /* family locals: per fluent a current + a primed local, per action trigger
     * one action local. cur/pri interleaved so index math stays local. */
    int nloc = 2 * nf + na;
    dlcol *f = dlcol_new(nloc, nent);
    int cur_local[MAX_PREDS], pri_local[MAX_PREDS];
    int inertia_pos[MAX_PREDS], inertia_neg[MAX_PREDS], act_local[MAX_ACTIONS];
    uint8_t *kind = malloc((size_t)nloc * sizeof *kind);
    int n = 0;
    char nbuf[MAX_GROUND + 2];
    for (int i = 0; i < nf; i++) {
        uint32_t P = p->preds[fpred[i]].pred;
        cur_local[i] = n; kind[n] = WORLD_STEP_CUR;
        dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, P));
        n++;
        pri_local[i] = n; kind[n] = WORLD_STEP_PRIMED;
        snprintf(nbuf, sizeof nbuf, "%s'", intern_name(p->syms, P));
        dlcol_set_atom_name(f, (uint32_t)n, nbuf);
        n++;
    }
    for (int j = 0; j < na; j++) {
        act_local[j] = n; kind[n] = WORLD_STEP_ACTION;
        dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, apred[j]));
        n++;
    }

    /* generated inertia, one pair per fluent (ids kept for causal superiority) */
    char rbuf[MAX_NAME + 16];
    for (int i = 0; i < nf; i++) {
        const char *fname = intern_name(p->syms, p->preds[fpred[i]].pred);
        snprintf(rbuf, sizeof rbuf, "inertia on %s", fname);
        dl_lit cur = { (uint32_t)cur_local[i], false }, pri = { (uint32_t)pri_local[i], false };
        inertia_pos[i] = dlcol_add_rule(f, rbuf, DL_DEFEASIBLE, pri, &cur, 1);
        dl_lit ncur = dl_complement(cur), npri = dl_complement(pri);
        inertia_neg[i] = dlcol_add_rule(f, rbuf, DL_DEFEASIBLE, npri, &ncur, 1);
    }

    /* causal rules and ramifications, one per effect, each superior to the
     * inertia rule it conflicts with (matches world.c emit_step_family) */
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        int nbody = a->nreq + (a->is_ramif ? 0 : 1);
        dl_lit body[MAX_BODY + 1];
        int bi = 0;
        for (int b = 0; b < a->nreq; b++) {
            int fi = step_fidx(p, fpred, nf, a->requires[b].pred);
            int loc = a->requires[b].primed ? pri_local[fi] : cur_local[fi];
            body[bi++] = (dl_lit){ (uint32_t)loc, a->requires[b].neg };
        }
        if (!a->is_ramif) {
            uint32_t tr = intern_id(p->syms, a->name);
            int aj = -1;
            for (int j = 0; j < na; j++) if (apred[j] == tr) { aj = j; break; }
            body[bi++] = (dl_lit){ (uint32_t)act_local[aj], false };
        }
        char pbuf[MAX_NAME + 24];
        prov_str(p, a->line, pbuf, sizeof pbuf);
        for (int b = 0; b < a->neff; b++) {
            int fi = step_fidx(p, fpred, nf, a->effects[b].pred);
            dl_lit head = { (uint32_t)pri_local[fi], a->effects[b].neg };
            char cname[MAX_NAME + 8];
            snprintf(cname, sizeof cname, "%s/%s%s", a->name,
                     a->effects[b].neg ? "~" : "",
                     intern_name(p->syms, a->effects[b].pred));
            int rid = dlcol_add_rule(f, cname, DL_DEFEASIBLE, head, body, nbody);
            dlcol_set_prov(f, rid, pbuf);
            dlcol_add_sup(f, rid, a->effects[b].neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
    }

    /* ground map: cur(P)@e -> P(e), pri(P)@e -> P(e)', action(A)@e -> A(e) */
    uint32_t *ground = malloc((size_t)nloc * nent * sizeof *ground);
    for (int i = 0; i < nf; i++) {
        uint32_t P = p->preds[fpred[i]].pred;
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            uint32_t base = ground_pred(p, P, &ent, 1);
            ground[(size_t)cur_local[i] * nent + e] = base;
            snprintf(nbuf, sizeof nbuf, "%s'", intern_name(p->syms, base));
            ground[(size_t)pri_local[i] * nent + e] = intern_id(p->syms, nbuf);
        }
    }
    for (int j = 0; j < na; j++)
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            ground[(size_t)act_local[j] * nent + e] = ground_pred(p, apred[j], &ent, 1);
        }

    world_add_step_lane_family(p->w, f, nloc, nent, ground, kind);
    free(ground);
    free(kind);
}

static void build_lane_families(parser *p)
{
    if (p->nactions > 0) {         /* a step world: lane the transition (first cut) */
        emit_step_lanes(p);        /* bails unless nrules==0 + homogeneous over S */
        return;
    }
    if (p->nrules == 0)            /* nothing to lane */
        return;
    bool taint[MAX_PREDS];
    compute_taint(p, taint);
    for (int S = 0; S < p->nsorts; S++)
        emit_sort_lanes(p, S, taint);
    for (int i = 0; i < p->nrules; i++)
        if (p->rules[i].nvars >= 2)                /* the join matcher (2+ vars) */
            emit_join_family(p, &p->rules[i]);
}

world *story_compile(const char *src, const char *srcname, intern *syms,
                     story_diags *diags)
{
    parser *p = calloc(1, sizeof *p);
    p->rules = calloc(MAX_RULES, sizeof *p->rules);
    p->actions = calloc(MAX_ACTIONS, sizeof *p->actions);
    p->exprs = calloc(MAX_EXPRS, sizeof *p->exprs);
    lexer_init(&p->lx, src);
    p->syms = syms;
    p->srcname = srcname;
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
            if (p->nerrors == 0) build_lane_families(p);   /* the DoD thesis, 2a */
        }
    }

    if (p->nerrors == 0) result = p->w;
    else world_free(p->w);

    for (int i = 0; i < p->nrules; i++) free(p->rules[i].insts);
    free(p->rules);
    free(p->actions);
    free(p->exprs);
    free(p->ents);
    free(p);
    return result;
}
