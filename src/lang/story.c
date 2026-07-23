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
#define MAX_ENUMS      16      /* named value domains (`enum school { … }`, §13) */
#define MAX_WHEN       8       /* conjuncts in a binder `where` / item `when` */
#define MAX_ITEMS      8       /* effect items in one `for each` block */
#define MAX_ACT_BINDERS 4      /* `for each` binders per action */
#define MAX_BINDERS    64      /* binder pool across the whole file */
#define MAX_INSTANCES  (1 << 20)   /* per-rule grounding blow-up guard */
#define CARD_WARN      100000      /* cross-product cardinality warning (§5.2) */
#define MAX_LADDERS    16          /* priority ladders (`bands …`, §6.2) */
#define MAX_BANDS      16          /* bands per ladder */

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
    bool        is_expr_guard; /* `expr <cmp> expr` guard (roll-/int-/paren-led) — a
                                * body-only computed atom, e.g. roll(20)+atk >= ac */
    int         lhs_root, rhs_root;   /* the two expr trees, when is_expr_guard */
    int       line, col;
} ast_atom;

/* Effect-RHS expression tree (§5.8), interned into a parser-owned node pool.
 * A leaf is a constant (`4`) or a numeric-fluent read (`hp`, `hp(X)`); interior
 * nodes are the closed arithmetic set. Grounding walks the tree per instance,
 * folding constant subtrees and emitting VM bytecode for the rest. */
typedef enum {
    EX_CONST, EX_LOAD, EX_ROLL, EX_ADD, EX_SUB, EX_MUL, EX_NEG, EX_MIN, EX_MAX
} ex_kind;
/* EX_ROLL (§5.10): a seeded die. `konst` = sides, `lhs` = an author disambiguator
 * tag (0 default). The roll site is keyed by (this node, the binding, tag). */

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
    char         band[MAX_NAME];  /* `@band` annotation (§6.2), "" if none */
    int          band_line, band_col;
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
    /* `set of SORT` params (§13): transient, host-answered membership relations,
     * kept out of the entity var list (they are not part of the ground action
     * atom or its cross-product). Each registers a provider relation `name(SORT)`;
     * `T in name` / `T not in name` in a guard reads it. */
    struct { uint32_t name, sortname; int line, col; } sets[MAX_ARGS];
    int       nsets;
    ast_atom  requires[MAX_BODY];
    int       nreq;
    ast_atom  effects[MAX_BODY];
    int       neff;
    int       bind_ix[MAX_ACT_BINDERS];   /* indices into parser.binders (§13) */
    int       nbind;
} ast_action;

/* A `for each T [, U] where <guard> [limit n]: { <eff> [when <cond>] , … }`
 * set-quantified effect binder (DESIGN.md §13). Bound vars extend the enclosing
 * action's var list at ground time, so one guarded step-rule is emitted per
 * (cast × inner binding × item) — the where/when conjuncts lower to step
 * conditions, exactly like a `requires`. `limit` is reserved for a later slice. */
typedef struct {
    ast_atom eff;
    ast_atom when[MAX_WHEN];
    int      nwhen;
} binder_item;

typedef struct {
    var_bind    vars[MAX_ARGS];
    int         nvars;
    ast_atom    where[MAX_WHEN];
    int         nwhere;
    int         limit;                    /* -1 = unbounded (reserved) */
    binder_item items[MAX_ITEMS];
    int         nitems;
    int         line, col;
} ast_binder;

typedef struct {
    uint32_t pred;
    int      nargs;
    uint32_t argsort[MAX_ARGS];   /* declared sort name atoms, resolved later */
    bool     is_mv;               /* declared with a `: { … }` value domain */
    uint32_t values[MAX_DOMAIN];  /* the domain's value symbols, in order */
    int      nvalues;
    bool     is_num;              /* declared `: int` (§5.8) */
    bool     has_range;           /* declared `in lo..hi` — the clamp range */
    long     rmin, rmax;          /* constant bounds (when each side folds) */
    int      rmin_expr, rmax_expr;/* dynamic bound ex_node root, else -1 (§5.8) */
    int      line, col;
} ast_fluent;

typedef struct { char a[MAX_NAME], b[MAX_NAME]; int aline, acol, bline, bcol; } ast_sup;

/* A named priority ladder (`bands stat_stack: base < condition < feat`, §6.2):
 * a totally-ordered list of band names, low to high. Bands are pure sugar over
 * pairwise `>` — at ground time a higher-band rule is made superior to a
 * lower-band rule wherever the two conflict (their heads oppose). The engine,
 * dl_why, and the M3 pipeline never learn bands exist. */
typedef struct {
    char name[MAX_NAME];
    char band[MAX_BANDS][MAX_NAME];   /* index = rank, ascending */
    int  nbands;
    int  line, col;
} ast_ladder;

/* A named value domain (`enum school { … }`, §13) — distinct from a `sort`
 * (entities); usable as a fluent type, erasing to the multi-valued machinery. */
typedef struct {
    char     name[MAX_NAME];
    uint32_t values[MAX_DOMAIN];
    int      nvalues;
    int      line, col;
} enum_dom;

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
    bool     is_provider;         /* a computed relation, host-answered (§5.6) */
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
    int          ndecls;          /* declarations parsed so far (header must be first) */

    /* Flat top-level `scene NAME` header (§4.1/§6.4). A single scene is one
     * partition — semantically invisible (no atom is qualified), because there
     * is no second scope to import from yet; the name is recorded for future
     * provenance/imports (§5.5, M4). */
    char         scene_name[MAX_NAME];
    int          scene_line, scene_col;
    bool         has_scene;

    struct { char name[MAX_NAME]; int line, col; } sorts[MAX_SORTS];
    int nsorts;
    struct ent_rec { uint32_t atom; int sort; int line, col; } *ents;  /* heap, grown */
    int nents, capents;
    /* O(1) entity lookups (built in resolve_entities), so grounding is not O(n^2):
     * ent_of maps an entity atom -> its p->ents index (interns are dense, so the
     * direct-indexed array is a perfect hash); ent_pos is that entity's position
     * within its own sort; domain_ents[s]/domain_n[s] is sort s's entity list. */
    int *ent_of; uint32_t ent_of_cap;
    int *ent_pos;
    uint32_t *domain_ents[MAX_SORTS]; int domain_n[MAX_SORTS];
    ast_fluent  fluents[MAX_FLUENTS];
    int nfluents;
    ast_fluent  providers[MAX_FLUENTS];   /* computed relations (§5.6), host-answered */
    int nproviders;
    ast_rule   *rules;            /* heap; MAX_RULES */
    int nrules;
    ast_action *actions;          /* heap; MAX_ACTIONS */
    int nactions;
    ast_binder *binders;          /* heap; MAX_BINDERS — the `for each` pool */
    int nbinders;
    enum_dom enums[MAX_ENUMS];    /* named value domains (§13) */
    int nenums;
    ast_sup     sups[MAX_SUPS];
    int nsups;
    ast_ladder  ladders[MAX_LADDERS];   /* priority ladders (`bands …`, §6.2) */
    int nladders;
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

/* Module/scene header (§6.4). Only the flat top-level `scene NAME` form is
 * implemented in this slice:
 *
 *   scene NAME               -- accepted: names the world's single scope (§4.1)
 *   scene NAME in MODULE     -- rejected: nested scopes are M4 (§5.5)
 *   module NAME / extend M   -- rejected: module extension is M4 (§6.4)
 *
 * A flat scene is a single partition and therefore semantically invisible —
 * it changes no atom's vocabulary. The nested/extension forms need
 * scope-tagged atoms and generated imports, so they fail loudly with a located
 * "not yet" rather than being silently swallowed. The header, if present, must
 * be the first declaration and may appear at most once. */
static void parse_module_header(parser *p)
{
    token kw = p->cur;
    advance(p);                                    /* 'scene' | 'module' | 'extend' */

    if (kw.kind != TK_SCENE) {
        fail(p, kw.line, kw.col,
             "module extension (`%s …`) is not implemented yet (M4, §6.4); "
             "only a flat top-level `scene NAME` header is supported",
             kw.kind == TK_MODULE ? "module" : "extend");
        return;
    }
    if (p->has_scene) {
        fail(p, kw.line, kw.col, "duplicate `scene` header");
        return;
    }
    if (p->ndecls != 0) {
        fail(p, kw.line, kw.col,
             "a `scene` header must be the first declaration in the file");
        return;
    }
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a scene name, found %s", d);
        return;
    }
    copy_ident(p->scene_name, MAX_NAME, p->cur);
    p->scene_line = p->cur.line;
    p->scene_col  = p->cur.col;
    p->has_scene  = true;
    advance(p);

    if (p->cur.kind == TK_IN) {
        fail(p, p->cur.line, p->cur.col,
             "nested scopes (`scene %s in M`) are not implemented yet "
             "(M4, §5.5); a flat top-level `scene %s` is accepted",
             p->scene_name, p->scene_name);
        return;
    }
}

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
static bool expr_fold(parser *p, int e, long *out);
static bool expr_reads_roll(parser *p, int e);

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
        if (ident_is(id, "roll") && p->cur.kind == TK_LPAREN) {   /* roll(sides[, tag]) */
            advance(p);
            long sides, tag = 0;
            if (!parse_int(p, &sides)) return -1;
            if (sides < 1) { fail(p, id.line, id.col, "roll(N): N must be >= 1"); return -1; }
            if (p->cur.kind == TK_COMMA) { advance(p); if (!parse_int(p, &tag)) return -1; }
            if (!expect(p, TK_RPAREN)) return -1;
            int n = alloc_expr(p, EX_ROLL, id.line, id.col);
            if (n < 0) return -1;
            p->exprs[n].konst = sides;
            p->exprs[n].lhs = (int)tag;
            return n;
        }
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
    /* An expression guard `expr <cmp> expr` (§5.8/§5.10) — recognised when the
     * conjunct starts with something a boolean atom can't: a `roll`/`min`/`max`
     * function call, an int, `(`, or `-`. Covers the d20:
     * `roll(20) + atk(A) >= ac(T)` and `max(roll(20,1), roll(20,2)) + atk >= ac`. */
    if (ident_is(p->cur, "roll") || ident_is(p->cur, "min") || ident_is(p->cur, "max") ||
        p->cur.kind == TK_INT || p->cur.kind == TK_LPAREN || p->cur.kind == TK_MINUS) {
        token lead = p->cur;
        int lhs = parse_expr(p);
        if (lhs < 0) return false;
        world_cmp op; bool have = true;
        switch (p->cur.kind) {
        case TK_LE: op = WORLD_CMP_LE; break; case TK_LT: op = WORLD_CMP_LT; break;
        case TK_GE: op = WORLD_CMP_GE; break; case TK_GT: op = WORLD_CMP_GT; break;
        case TK_EQ: op = WORLD_CMP_EQ; break; default: have = false; break;
        }
        if (!have) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a comparison (<=, <, >=, >, =) in a roll/expression guard, found %s", d);
            return false;
        }
        advance(p);
        int rhs = parse_expr(p);
        if (rhs < 0) return false;
        out->is_expr_guard = true;
        out->lhs_root = lhs; out->rhs_root = rhs; out->cmp = op;
        out->line = lead.line; out->col = lead.col;
        return true;
    }
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
    /* set membership: `T in P` / `T not in P` over a `set of` param — the
     * leading id is the element var, P the set (a host-answered provider
     * relation, §5.6/§13). Lowers to a read of P(T): `not in` negates it. */
    if (p->cur.kind == TK_IN || ident_is(p->cur, "not")) {
        bool notin = false;
        if (ident_is(p->cur, "not")) {
            token nt = p->cur; advance(p);
            if (p->cur.kind != TK_IN) {
                fail(p, nt.line, nt.col, "expected `in` after `not` in a set membership test");
                return false;
            }
            notin = true;
        }
        advance(p);                                /* 'in' */
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a `set of` parameter name after `in`, found %s", d);
            return false;
        }
        out->args[0].name = out->pred;             /* the element var T */
        out->args[0].line = id.line;
        out->args[0].col = id.col;
        out->nargs = 1;
        out->pred = intern_tok(p, p->cur);         /* the set/provider name P */
        out->neg ^= notin;
        advance(p);                                /* past P */
        return true;
    }
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

/* A declared `enum` value domain (§13), matched by name; -1 if none. */
static int find_enum(parser *p, token t)
{
    for (int i = 0; i < p->nenums; i++)
        if ((int)strlen(p->enums[i].name) == t.len &&
            memcmp(p->enums[i].name, t.start, (size_t)t.len) == 0)
            return i;
    return -1;
}

/* enum := 'enum' IDENT '{' IDENT (',' IDENT)* '}' — a named value domain,
 * usable as a fluent type (`conc_spell(actor) : spell`). Distinct from `sort`,
 * which is for entities (§13). */
static void parse_enum(parser *p)
{
    advance(p);                                    /* 'enum' */
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an enum name, found %s", d);
        return;
    }
    if (p->nenums >= MAX_ENUMS) {
        fail(p, p->cur.line, p->cur.col, "too many enums (max %d)", MAX_ENUMS);
        return;
    }
    if (find_enum(p, p->cur) >= 0) {
        fail(p, p->cur.line, p->cur.col, "enum '%.*s' is already declared",
             p->cur.len, p->cur.start);
        return;
    }
    enum_dom *e = &p->enums[p->nenums];
    copy_ident(e->name, MAX_NAME, p->cur);
    e->line = p->cur.line; e->col = p->cur.col; e->nvalues = 0;
    advance(p);
    if (!expect(p, TK_LBRACE)) return;
    for (;;) {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a value name, found %s", d);
            return;
        }
        if (e->nvalues >= MAX_DOMAIN) {
            fail(p, p->cur.line, p->cur.col, "too many enum values (max %d)", MAX_DOMAIN);
            return;
        }
        e->values[e->nvalues++] = intern_tok(p, p->cur);
        advance(p);
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    if (!expect(p, TK_RBRACE)) return;
    if (e->nvalues < 2) {
        fail(p, e->line, e->col, "a value domain needs at least two values");
        return;
    }
    p->nenums++;
}

/* fdecl := IDENT [ '(' IDENT (',' IDENT)* ')' ]; a ':' after it is a typed or
 * multi-valued fluent, out of this slice. */
static bool parse_fdecl(parser *p, ast_fluent *f)
{
    memset(f, 0, sizeof *f);
    f->rmin_expr = f->rmax_expr = -1;
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
                /* Each bound is an expression (§5.8): a literal folds to a
                 * constant; `hp_max(X)` stays dynamic, resolved per entity at
                 * commit. A folded bound's AST nodes are reclaimed (nexprs
                 * rewound) so a constant range perturbs no downstream node
                 * indices — keeping roll-site keys (§5.10) stable. The fluent's
                 * key sort name is the implicit key. */
                long lc, hc;
                int e0 = p->nexprs;
                int lo = parse_expr(p);
                if (lo < 0) return false;
                bool lo_const = expr_fold(p, lo, &lc);
                if (lo_const) { f->rmin = lc; p->nexprs = e0; }
                else f->rmin_expr = lo;
                if (!expect(p, TK_DOTDOT)) return false;
                int e1 = p->nexprs;
                int hi = parse_expr(p);
                if (hi < 0) return false;
                bool hi_const = expr_fold(p, hi, &hc);
                if (hi_const) { f->rmax = hc; p->nexprs = e1; }
                else f->rmax_expr = hi;
                if (lo_const && hi_const && hc < lc) {
                    fail(p, f->line, f->col,
                         "numeric range is empty: hi (%ld) is below lo (%ld)",
                         hc, lc);
                    return false;
                }
                f->has_range = true;
            }
            f->is_num = true;
            return true;
        }
        if (p->cur.kind == TK_IDENT) {             /* `: enumname` — a named domain */
            int ei = find_enum(p, p->cur);
            if (ei < 0) {
                fail(p, p->cur.line, p->cur.col,
                     "'%.*s' is not a declared enum; only `: int`, `: { … }`, "
                     "or a declared `enum` domain are supported (entity-domain "
                     "fluents land later)", p->cur.len, p->cur.start);
                return false;
            }
            f->is_mv = true;
            f->nvalues = p->enums[ei].nvalues;
            for (int v = 0; v < f->nvalues; v++) f->values[v] = p->enums[ei].values[v];
            advance(p);
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

/* provider := 'provider' ( pdecl | '(' pdecl* ')' ); pdecl := IDENT '(' sort,… ')'
 * A computed relation (§5.6), host-answered — like a boolean fluent decl but with
 * no value type. */
static void parse_provider(parser *p)
{
    advance(p);                                    /* 'provider' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a provider name, found %s", d);
            return;
        }
        if (p->nproviders >= MAX_FLUENTS) {
            fail(p, p->cur.line, p->cur.col, "too many providers (max %d)", MAX_FLUENTS);
            return;
        }
        ast_fluent *pr = &p->providers[p->nproviders];
        if (!parse_fdecl(p, pr)) return;
        if (pr->is_num || pr->is_mv) {
            fail(p, pr->line, pr->col,
                 "a provider is a relation, not a typed fluent — drop the `: …`");
            return;
        }
        p->nproviders++;
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
/* `act` (nullable) receives `set of SORT` params — actions only; rules pass
 * NULL, so a `set of` there is a located error. */
static bool parse_params(parser *p, var_bind *vars, int *nvars, ast_action *act)
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
        token nm = p->cur;
        advance(p);
        if (!expect(p, TK_COLON)) return false;
        if (p->cur.kind == TK_SET) {               /* `set of SORT` — a set param */
            if (!act) {
                fail(p, nm.line, nm.col,
                     "`set of` parameters are only allowed on actions");
                return false;
            }
            advance(p);                            /* 'set' */
            if (!expect(p, TK_OF)) return false;
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an element sort after `set of`, found %s", d);
                return false;
            }
            if (act->nsets >= MAX_ARGS) {
                fail(p, nm.line, nm.col, "too many set parameters (max %d)", MAX_ARGS);
                return false;
            }
            act->sets[act->nsets].name = intern_tok(p, nm);
            act->sets[act->nsets].sortname = intern_tok(p, p->cur);
            act->sets[act->nsets].line = nm.line;
            act->sets[act->nsets].col = nm.col;
            act->nsets++;
            advance(p);                            /* the element sort */
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (*nvars >= MAX_ARGS) {
            fail(p, nm.line, nm.col, "too many variables (max %d)", MAX_ARGS);
            return false;
        }
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return false;
        }
        var_bind *v = &vars[*nvars];
        v->name = intern_tok(p, nm);
        v->line = nm.line;
        v->col = nm.col;
        /* encode the sort name atom for resolution in the semantic pass */
        v->sort = -(int)intern_tok(p, p->cur) - 2;
        (*nvars)++;
        advance(p);
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    return expect(p, TK_RPAREN);
}

/* Bound vars of a `for each`: `T : sort (',' U : sort)*` — like parse_params
 * but unparenthesized (we are already past `each`). */
static bool parse_binder_vars(parser *p, ast_binder *bnd)
{
    for (;;) {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a bound variable, found %s", d);
            return false;
        }
        if (bnd->nvars >= MAX_ARGS) {
            fail(p, p->cur.line, p->cur.col, "too many bound variables (max %d)", MAX_ARGS);
            return false;
        }
        var_bind *v = &bnd->vars[bnd->nvars];
        v->name = intern_tok(p, p->cur);
        v->line = p->cur.line; v->col = p->cur.col; v->sort = -1;
        advance(p);
        if (!expect(p, TK_COLON)) return false;
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return false;
        }
        v->sort = -(int)intern_tok(p, p->cur) - 2;   /* encoded for the semantic pass */
        bnd->nvars++;
        advance(p);
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    return true;
}

/* binder := 'for' 'each' bvars [ 'where' conj ] [ 'limit' INT ] ':'
 *            ( bind_eff | '{' bind_eff (',' bind_eff)* '}' )
 * bind_eff := atom [ 'when' conj ]      -- `when` only inside a `{ … }` block
 * The parsed binder is stashed in the file-wide pool; the enclosing action
 * records its index. */
static bool parse_binder(parser *p, ast_action *a)
{
    token ft = p->cur;
    advance(p);                                    /* 'for' */
    if (!expect(p, TK_EACH)) return false;
    if (p->nbinders >= MAX_BINDERS) {
        fail(p, ft.line, ft.col, "too many `for each` binders (max %d)", MAX_BINDERS);
        return false;
    }
    if (a->nbind >= MAX_ACT_BINDERS) {
        fail(p, ft.line, ft.col, "too many binders in one action (max %d)", MAX_ACT_BINDERS);
        return false;
    }
    int slot = p->nbinders;
    ast_binder *bnd = &p->binders[slot];
    memset(bnd, 0, sizeof *bnd);
    bnd->line = ft.line; bnd->col = ft.col; bnd->limit = -1;

    if (!parse_binder_vars(p, bnd)) return false;
    if (p->cur.kind == TK_WHERE) {
        advance(p);
        int nw = parse_conj(p, bnd->where, MAX_WHEN);
        if (nw < 0) return false;
        bnd->nwhere = nw;
    }
    if (p->cur.kind == TK_LIMIT) {                  /* reserved for a later slice */
        fail(p, p->cur.line, p->cur.col,
             "`limit` is not supported yet (bounded quantification is a later "
             "slice); drop it or split the effect");
        return false;
    }
    if (!expect(p, TK_COLON)) return false;

    if (p->cur.kind == TK_LBRACE) {                 /* block: many items, per-item `when` */
        advance(p);
        for (;;) {
            if (bnd->nitems >= MAX_ITEMS) {
                fail(p, p->cur.line, p->cur.col, "too many effect items (max %d)", MAX_ITEMS);
                return false;
            }
            binder_item *it = &bnd->items[bnd->nitems];
            memset(it, 0, sizeof *it);
            if (!parse_atom(p, &it->eff)) return false;
            if (p->cur.kind == TK_WHEN) {
                advance(p);
                int nw = parse_conj(p, it->when, MAX_WHEN);
                if (nw < 0) return false;
                it->nwhen = nw;
            }
            bnd->nitems++;
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_RBRACE)) return false;
    } else {                                        /* single form: one effect, no `when` */
        binder_item *it = &bnd->items[0];
        memset(it, 0, sizeof *it);
        if (!parse_atom(p, &it->eff)) return false;
        bnd->nitems = 1;
    }
    p->nbinders++;
    a->bind_ix[a->nbind++] = slot;
    return true;
}

/* A `causes` effect body: `&`-separated items, each a plain effect atom or a
 * `for each` binder. Shared by actions and `rule … causes` ramifications. */
static bool parse_effects(parser *p, ast_action *a)
{
    for (;;) {
        if (p->cur.kind == TK_FOR) {
            if (!parse_binder(p, a)) return false;
        } else {
            if (a->neff >= MAX_BODY) {
                fail(p, p->cur.line, p->cur.col,
                     "too many effects (max %d atoms)", MAX_BODY);
                return false;
            }
            if (!parse_atom(p, &a->effects[a->neff])) return false;
            a->neff++;
        }
        if (p->cur.kind == TK_AMP) { advance(p); continue; }
        break;
    }
    return true;
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

    if (!parse_params(p, r->vars, &r->nvars, NULL)) return;   /* rules: no set params */
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
        if (!parse_effects(p, a)) return;
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

    /* optional `@band` annotation (§6.2): assigns this rule to a priority band */
    if (p->cur.kind == TK_AT) {
        advance(p);                                /* '@' */
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a band name after '@', found %s", d);
            return;
        }
        copy_ident(r->band, MAX_NAME, p->cur);
        r->band_line = p->cur.line;
        r->band_col = p->cur.col;
        advance(p);
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

    if (!parse_params(p, a->vars, &a->nvars, a)) return;
    if (!expect(p, TK_COLON)) return;

    if (p->cur.kind == TK_REQUIRES) {
        advance(p);
        int nr = parse_conj(p, a->requires, MAX_BODY);
        if (nr < 0) return;
        a->nreq = nr;
    }
    if (!expect(p, TK_CAUSES)) return;
    if (!parse_effects(p, a)) return;
    p->nactions++;
}

/* sup := IDENT '>' IDENT (label > label) */
/* bands := 'bands' IDENT ':' IDENT ('<' IDENT)*    -- a priority ladder (§6.2).
 * Names a totally-ordered list of band names, low to high. Semantic validation
 * (unique names, no band shared across ladders) happens in the semantic pass. */
static void parse_bands(parser *p)
{
    advance(p);                                    /* 'bands' */
    if (p->nladders >= MAX_LADDERS) {
        fail(p, p->cur.line, p->cur.col, "too many priority ladders (max %d)", MAX_LADDERS);
        return;
    }
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a ladder name, found %s", d);
        return;
    }
    ast_ladder *l = &p->ladders[p->nladders];
    l->nbands = 0;
    copy_ident(l->name, MAX_NAME, p->cur);
    l->line = p->cur.line;
    l->col = p->cur.col;
    advance(p);
    if (!expect(p, TK_COLON)) return;

    for (;;) {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a band name, found %s", d);
            return;
        }
        if (l->nbands >= MAX_BANDS) {
            fail(p, p->cur.line, p->cur.col,
                 "too many bands in ladder '%s' (max %d)", l->name, MAX_BANDS);
            return;
        }
        copy_ident(l->band[l->nbands++], MAX_NAME, p->cur);
        advance(p);
        if (p->cur.kind != TK_LT) break;
        advance(p);                                /* '<' */
    }
    if (l->nbands < 2)
        warn(p, l->line, l->col,
             "ladder '%s' has %d band(s); a ladder with fewer than two bands "
             "orders nothing", l->name, l->nbands);
    p->nladders++;
}

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

/* All O(1) via the maps built in resolve_entities (was linear — the source of
 * the O(n^2) grounding wall, since decode_binding hits these per instance). */
static int find_entity(parser *p, uint32_t atom)
{
    return atom < p->ent_of_cap ? p->ent_of[atom] : -1;
}

/* domain of a sort: entities declared for it, in declaration order */
static int domain_size(parser *p, int sort)
{
    return (sort >= 0 && sort < p->nsorts) ? p->domain_n[sort] : 0;
}
static uint32_t domain_at(parser *p, int sort, int k)
{
    if (sort < 0 || sort >= p->nsorts || k < 0 || k >= p->domain_n[sort])
        return INTERN_NONE;
    return p->domain_ents[sort][k];
}
static int entity_pos(parser *p, int sort, uint32_t atom)
{
    int i = find_entity(p, atom);
    return (i >= 0 && p->ents[i].sort == sort) ? p->ent_pos[i] : -1;
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
/* atom -> int map with geometric growth (amortized O(1); a grow-to-key+1 per
 * call would reintroduce O(n^2)). New slots init to -1. */
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

static void resolve_entities(parser *p)
{
    for (int i = 0; i < p->nents; i++) {
        int s = decode_sort(p, p->ents[i].sort, p->ents[i].line, p->ents[i].col,
                            "an entity declaration");
        p->ents[i].sort = s;                       /* may be -1 on error */
    }

    /* atom -> first-occurrence index (also an O(n) duplicate check) */
    for (int i = 0; i < p->nents; i++) {
        uint32_t at = p->ents[i].atom;
        int prev = at < p->ent_of_cap ? p->ent_of[at] : -1;
        if (prev >= 0)
            serr(p, p->ents[i].line, p->ents[i].col,
                 "duplicate entity '%s'", intern_name(p->syms, at));
        else
            atom_map_set(&p->ent_of, &p->ent_of_cap, at, i);
    }

    /* per-sort entity lists + each entity's position within its sort */
    for (int s = 0; s < p->nsorts; s++) p->domain_n[s] = 0;
    for (int i = 0; i < p->nents; i++)
        if (p->ents[i].sort >= 0) p->domain_n[p->ents[i].sort]++;
    for (int s = 0; s < p->nsorts; s++)
        p->domain_ents[s] = malloc((size_t)(p->domain_n[s] ? p->domain_n[s] : 1)
                                   * sizeof *p->domain_ents[s]);
    p->ent_pos = malloc((size_t)(p->nents > 0 ? p->nents : 1) * sizeof *p->ent_pos);
    int fill[MAX_SORTS];
    for (int s = 0; s < p->nsorts; s++) fill[s] = 0;
    for (int i = 0; i < p->nents; i++) {
        int s = p->ents[i].sort;
        if (s < 0) { p->ent_pos[i] = -1; continue; }
        p->ent_pos[i] = fill[s];
        p->domain_ents[s][fill[s]++] = p->ents[i].atom;
    }
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

    /* providers register a relation predicate with its arg sorts (no value). */
    for (int i = 0; i < p->nproviders; i++) {
        ast_fluent *pr = &p->providers[i];
        pred_info *pi = find_pred(p, pr->pred);
        if (pi && (pi->is_fluent || pi->is_provider)) {
            serr(p, pr->line, pr->col, "'%s' is already declared",
                 intern_name(p->syms, pr->pred));
            continue;
        }
        pi = intern_pred(p, pr->pred, pr->nargs);
        if (!pi) { serr(p, pr->line, pr->col, "too many predicates"); return; }
        pi->is_provider = true;
        pi->arity = pr->nargs;
        for (int k = 0; k < pr->nargs; k++)
            pi->argsort[k] = decode_sort(p, -(int)pr->argsort[k] - 2,
                                         pr->line, pr->col, "a provider declaration");
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
    case EX_ROLL:                    /* a seeded draw — nothing to resolve */
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

/* Validate dynamic clamp bounds (`int in 0 .. hp_max(X)`, §5.8): the bound
 * expression's fluent reads must resolve to declared numeric fluents, keyed by
 * the declaring fluent's own sort(s) — the key sort name is the implicit key,
 * so it is offered as an in-scope variable. A `roll()` in a bound would make
 * the clamp non-deterministic across reads, so it is rejected. */
static void check_fluent_bounds(parser *p)
{
    for (int i = 0; i < p->nfluents; i++) {
        ast_fluent *f = &p->fluents[i];
        if (f->rmin_expr < 0 && f->rmax_expr < 0) continue;
        var_bind kv[MAX_ARGS];
        for (int k = 0; k < f->nargs; k++) {
            kv[k].name = f->argsort[k];
            kv[k].sort = find_sort(p, f->argsort[k]);
            kv[k].line = f->line;
            kv[k].col = f->col;
        }
        int roots[2] = { f->rmin_expr, f->rmax_expr };
        for (int r = 0; r < 2; r++) {
            if (roots[r] < 0) continue;
            check_expr(p, roots[r], kv, f->nargs);
            if (expr_reads_roll(p, roots[r]))
                serr(p, p->exprs[roots[r]].line, p->exprs[roots[r]].col,
                     "a clamp bound may not use `roll()` — the range must be a "
                     "stable value, not a fresh draw");
        }
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
    if (at->is_expr_guard) {                        /* `expr <op> expr` guard */
        if (in_effect) {
            serr(p, at->line, at->col,
                 "a comparison guard can't appear in a `causes` clause");
            return;
        }
        check_expr(p, at->lhs_root, vars, nvars);
        check_expr(p, at->rhs_root, vars, nvars);
        return;
    }
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

/* Does expression tree e contain a `roll()` (an EX_ROLL draw)? */
static bool expr_reads_roll(parser *p, int e)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_LOAD: return false;
    case EX_ROLL: return true;
    case EX_NEG:  return expr_reads_roll(p, n->lhs);
    default:      return expr_reads_roll(p, n->lhs) || expr_reads_roll(p, n->rhs);
    }
}

/* Does expression tree e read variable `name` (in an EX_LOAD fluent read)? */
static bool expr_uses_var(parser *p, int e, uint32_t name)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_ROLL: return false;
    case EX_LOAD:
        for (int k = 0; k < n->nargs; k++) if (n->args[k].name == name) return true;
        return false;
    case EX_NEG: return expr_uses_var(p, n->lhs, name);
    default:     return expr_uses_var(p, n->lhs, name) || expr_uses_var(p, n->rhs, name);
    }
}

/* Every rule variable must occur in the body — the safety / range-restriction
 * discipline (§5.2 item 1). Typed vars bound the domain (item 2), so an unused
 * var is a probable authoring slip, not an unsafe grounding: warn, don't fail. */
static void check_safety(parser *p, ast_rule *r)
{
    for (int i = 0; i < r->nvars; i++) {
        bool used = false;
        for (int b = 0; b < r->nbody && !used; b++) {
            if (r->body[b].is_expr_guard) {          /* variables live in the exprs */
                if (expr_uses_var(p, r->body[b].lhs_root, r->vars[i].name) ||
                    expr_uses_var(p, r->body[b].rhs_root, r->vars[i].name)) used = true;
                continue;
            }
            for (int k = 0; k < r->body[b].nargs; k++)
                if (r->body[b].args[k].name == r->vars[i].name) { used = true; break; }
        }
        if (!used)
            warn(p, r->vars[i].line, r->vars[i].col,
                 "variable '%s' of rule '%s' does not occur in the body — "
                 "it grounds over the whole '%s' sort",
                 intern_name(p->syms, r->vars[i].name), r->label,
                 r->vars[i].sort >= 0 ? p->sorts[r->vars[i].sort].name : "?");
    }
}

/* Priority-ladder well-formedness (§6.2): ladder names unique, band names
 * unique within a ladder, and no band shared across ladders (comparability
 * must be unambiguous). Every `@band` annotation must name a declared band —
 * an unbanded default would let a rule's defeat behaviour change because a
 * ladder was declared elsewhere (the non-local surprise §6.1 forbids). */
static void check_bands(parser *p)
{
    for (int i = 0; i < p->nladders; i++) {
        ast_ladder *li = &p->ladders[i];
        for (int j = i + 1; j < p->nladders; j++)
            if (strcmp(li->name, p->ladders[j].name) == 0)
                serr(p, p->ladders[j].line, p->ladders[j].col,
                     "duplicate priority ladder '%s'", li->name);
        for (int a = 0; a < li->nbands; a++) {
            for (int b = a + 1; b < li->nbands; b++)
                if (strcmp(li->band[a], li->band[b]) == 0)
                    serr(p, li->line, li->col,
                         "band '%s' listed twice in ladder '%s'", li->band[a], li->name);
            for (int j = i + 1; j < p->nladders; j++)
                for (int b = 0; b < p->ladders[j].nbands; b++)
                    if (strcmp(li->band[a], p->ladders[j].band[b]) == 0)
                        serr(p, p->ladders[j].line, p->ladders[j].col,
                             "band '%s' appears in both ladders '%s' and '%s'; "
                             "a band belongs to exactly one ladder",
                             li->band[a], li->name, p->ladders[j].name);
        }
    }
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->band[0] == '\0') continue;
        bool found = false;
        for (int li = 0; li < p->nladders && !found; li++)
            for (int b = 0; b < p->ladders[li].nbands; b++)
                if (strcmp(p->ladders[li].band[b], r->band) == 0) { found = true; break; }
        if (!found)
            serr(p, r->band_line, r->band_col,
                 "unknown band '%s' — no `bands` ladder declares it", r->band);
    }
}

/* A `set of SORT` action param (§13) is a transient, host-answered membership
 * relation: register it as a provider `name(SORT)` so `T in name` reads it
 * through the ordinary provider path (§5.6). Deduped by name across actions;
 * a same-name clash at a different arity/sort is an error. Runs before the
 * predicate registry so the relation is marked provider-answered. */
static void register_set_providers(parser *p)
{
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        for (int s = 0; s < a->nsets; s++) {
            uint32_t name = a->sets[s].name, esort = a->sets[s].sortname;
            bool dup = false;
            for (int j = 0; j < p->nproviders; j++)
                if (p->providers[j].pred == name) {
                    dup = true;
                    if (p->providers[j].nargs != 1 || p->providers[j].argsort[0] != esort)
                        serr(p, a->sets[s].line, a->sets[s].col,
                             "set parameter '%s' clashes with another declaration "
                             "of the same name", intern_name(p->syms, name));
                    break;
                }
            if (dup) continue;
            if (p->nproviders >= MAX_FLUENTS) {
                serr(p, a->sets[s].line, a->sets[s].col,
                     "too many providers (max %d)", MAX_FLUENTS);
                return;
            }
            ast_fluent *pr = &p->providers[p->nproviders++];
            memset(pr, 0, sizeof *pr);
            pr->pred = name;
            pr->nargs = 1;
            pr->argsort[0] = esort;
            pr->line = a->sets[s].line;
            pr->col = a->sets[s].col;
        }
    }
}

static void semantic_pass(parser *p)
{
    resolve_entities(p);
    register_set_providers(p);
    build_pred_registry(p);
    check_fluent_bounds(p);

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
        /* `for each` binders: resolve bound vars, then check the where/when
         * guards and effects against the combined (action ++ binder) scope. */
        for (int bi = 0; bi < a->nbind; bi++) {
            ast_binder *bnd = &p->binders[a->bind_ix[bi]];
            resolve_vars(p, bnd->vars, bnd->nvars, "a `for each` binder");
            for (int k = 0; k < bnd->nvars; k++)
                if (var_index(a->vars, a->nvars, bnd->vars[k].name) >= 0)
                    serr(p, bnd->vars[k].line, bnd->vars[k].col,
                         "bound variable '%s' shadows an action parameter",
                         intern_name(p->syms, bnd->vars[k].name));
            var_bind cv[2 * MAX_ARGS];
            int nc = a->nvars;
            for (int k = 0; k < a->nvars; k++)  cv[k] = a->vars[k];
            for (int k = 0; k < bnd->nvars; k++) cv[nc + k] = bnd->vars[k];
            nc += bnd->nvars;
            for (int b = 0; b < bnd->nwhere; b++)
                check_atom(p, &bnd->where[b], cv, nc, true, false, false, "a `where` guard");
            for (int it = 0; it < bnd->nitems; it++) {
                binder_item *item = &bnd->items[it];
                check_atom(p, &item->eff, cv, nc, false, true, false, "a `for each` effect");
                if (item->eff.value != INTERN_NONE && item->eff.neg)
                    serr(p, item->eff.line, item->eff.col,
                         "a negative multi-valued effect ('~(%s = %s)') is not "
                         "supported yet — assign the intended value instead",
                         intern_name(p->syms, item->eff.pred),
                         intern_name(p->syms, item->eff.value));
                for (int b = 0; b < item->nwhen; b++)
                    check_atom(p, &item->when[b], cv, nc, true, false, false, "a `when` guard");
            }
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

    check_bands(p);
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
    case EX_ROLL:  return false;              /* a fresh draw — never a constant */
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
    if (n->kind == EX_ROLL) {
        /* site keyed by (this node, the ground binding, tag) — the node is the
         * rule namespace (§5.10), the binding gives each instance its own draw. */
        uint64_t site = 0x9E3779B97F4A7C15ull ^ ((uint64_t)e * 0x100000001B3ull)
                        ^ ((uint64_t)(uint32_t)n->lhs + 1);
        for (int k = 0; k < nvars; k++)
            site = site * 0x100000001B3ull ^ binding[k];
        int idx = world_add_roll_site(p->w, (int)n->konst, site);
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_ROLL; code[(*pos)++].arg = (long)idx; }
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

static void inst_name(parser *p, char *buf, size_t n, const char *label,
                      var_bind *vars, int nvars, const uint32_t *binding);

static dl_lit ground_lit(parser *p, ast_atom *at, var_bind *vars, int nvars,
                         const uint32_t *binding)
{
    if (at->is_expr_guard) {                       /* `expr <op> expr` — e.g. the d20 */
        expr_ins lcode[MAX_CODE], rcode[MAX_CODE];
        int nl = 0, nr = 0;
        emit_expr(p, at->lhs_root, vars, nvars, binding, lcode, &nl);
        emit_expr(p, at->rhs_root, vars, nvars, binding, rcode, &nr);
        char label[24], nm[MAX_GROUND];
        snprintf(label, sizeof label, "eg%d", at->lhs_root);   /* per guard occurrence */
        inst_name(p, nm, sizeof nm, label, vars, nvars, binding);
        uint32_t g = intern_id(p->syms, nm);
        world_add_expr_guard(p->w, g, lcode, nl, rcode, nr, at->cmp);
        return at->neg ? dl_neg(g) : dl_pos(g);
    }
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
        pred_info *pi = find_pred(p, at->pred);
        if (pi && pi->is_provider)                 /* a computed relation (§5.6) */
            world_declare_provider_atom(p->w, g, at->pred, args, at->nargs);
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
            if (f->is_num) {                       /* value-store slot, not an atom */
                uint32_t atom = ground_pred(p, f->pred, binding, f->nargs);
                world_declare_num(p->w, atom, f->rmin, f->rmax, f->has_range);
                if (f->rmin_expr >= 0 || f->rmax_expr >= 0) {
                    /* dynamic clamp: compile each bound per entity, the key sort
                     * name resolving to this instance's binding (§5.8) */
                    var_bind kv[MAX_ARGS];
                    for (int k = 0; k < f->nargs; k++) {
                        kv[k].name = f->argsort[k];
                        kv[k].sort = vb[k].sort;
                    }
                    expr_ins lo[MAX_CODE], hi[MAX_CODE];
                    int nlo = 0, nhi = 0;
                    if (f->rmin_expr >= 0)
                        emit_expr(p, f->rmin_expr, kv, f->nargs, binding, lo, &nlo);
                    if (f->rmax_expr >= 0)
                        emit_expr(p, f->rmax_expr, kv, f->nargs, binding, hi, &nhi);
                    world_set_num_clamp(p->w, atom,
                                        f->rmin_expr >= 0 ? lo : NULL, nlo,
                                        f->rmax_expr >= 0 ? hi : NULL, nhi);
                }
            }
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

        /* set-quantified effect binders (§13). The bound var(s) extend this
         * instance's binding; one step rule is emitted per (inner binding ×
         * item), all sharing this action's trigger `act`. The `where` guard and
         * an item's `when` guard lower to step conditions (like a `requires`),
         * so the per-target subset is resolved at tick time. */
        for (int bi = 0; bi < a->nbind; bi++) {
            ast_binder *bnd = &p->binders[a->bind_ix[bi]];
            var_bind cv[2 * MAX_ARGS];
            uint32_t  cb[2 * MAX_ARGS];
            for (int k = 0; k < a->nvars; k++) { cv[k] = a->vars[k]; cb[k] = binding[k]; }
            for (int k = 0; k < bnd->nvars; k++) cv[a->nvars + k] = bnd->vars[k];
            int ncv = a->nvars + bnd->nvars;

            bool bof = false;
            long inner = instance_count(p, bnd->vars, bnd->nvars, &bof);
            if (bof) {
                warn(p, bnd->line, bnd->col,
                     "a `for each` in '%s' grounds to more than %d instances",
                     a->name, MAX_INSTANCES);
                continue;
            }
            for (long j = 0; j < inner; j++) {
                uint32_t ib[MAX_ARGS];
                decode_binding(p, bnd->vars, bnd->nvars, j, ib);
                for (int k = 0; k < bnd->nvars; k++) cb[a->nvars + k] = ib[k];

                for (int it = 0; it < bnd->nitems; it++) {
                    binder_item *item = &bnd->items[it];
                    /* conds = action requires + binder where + item when */
                    step_cond bc[MAX_BODY];
                    int nbc = 0;
                    for (int b = 0; b < a->nreq && nbc < MAX_BODY; b++) {
                        bc[nbc].lit = ground_lit(p, &a->requires[b], cv, ncv, cb);
                        bc[nbc++].primed = a->requires[b].primed;
                    }
                    for (int b = 0; b < bnd->nwhere && nbc < MAX_BODY; b++) {
                        bc[nbc].lit = ground_lit(p, &bnd->where[b], cv, ncv, cb);
                        bc[nbc++].primed = false;
                    }
                    for (int b = 0; b < item->nwhen && nbc < MAX_BODY; b++) {
                        bc[nbc].lit = ground_lit(p, &item->when[b], cv, ncv, cb);
                        bc[nbc++].primed = false;
                    }

                    ast_atom *e = &item->eff;
                    dl_lit eff2[MAX_BODY];
                    int ne2 = 0;
                    if (!e->is_num_effect) {
                        if (e->value == INTERN_NONE) {
                            eff2[ne2++] = ground_lit(p, e, cv, ncv, cb);
                        } else {                       /* MV: chosen value + sibling negations */
                            uint32_t mvarg[MAX_ARGS];
                            for (int k = 0; k < e->nargs; k++)
                                mvarg[k] = resolve_arg(cv, ncv, cb, e->args[k]);
                            pred_info *pi = find_pred(p, e->pred);
                            eff2[ne2++] = dl_pos(ground_mv_atom(p, e->pred, mvarg,
                                                                e->nargs, e->value));
                            for (int v = 0; v < pi->nvalues && ne2 < MAX_BODY; v++)
                                if (pi->values[v] != e->value)
                                    eff2[ne2++] = dl_neg(ground_mv_atom(p, e->pred, mvarg,
                                                                        e->nargs, pi->values[v]));
                        }
                    }
                    char bname[MAX_GROUND];
                    inst_name(p, bname, sizeof bname, a->name, cv, ncv, cb);
                    int h2 = world_add_step_rule(p->w, bname, act, bc, nbc, eff2, ne2);
                    char pbuf[MAX_NAME + 24];
                    world_set_step_prov(p->w, h2, prov_str(p, bnd->line, pbuf, sizeof pbuf));
                    if (e->is_num_effect) {
                        uint32_t narg[MAX_ARGS];
                        for (int k = 0; k < e->nargs; k++)
                            narg[k] = resolve_arg(cv, ncv, cb, e->args[k]);
                        uint32_t num = ground_pred(p, e->pred, narg, e->nargs);
                        expr_ins code[MAX_CODE];
                        int nc = 0;
                        emit_expr(p, e->expr_root, cv, ncv, cb, code, &nc);
                        world_add_num_effect(p->w, h2, num, e->numop, code, nc);
                    }
                }
            }
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
/* Make every ground instance of `ra` superior to the aligned instance of `rb`
 * (`ra > rb`). Instances are aligned over the union of the two rules' variables
 * shared by name; a variable appearing in both must agree on sort. Shared by the
 * explicit `>` (ground_sup) and by band-generated edges (ground_bands). `line`/
 * `col` locate the shared-sort error at the edge's declaration site. */
static void emit_sup_edges(parser *p, ast_rule *ra, ast_rule *rb, int line, int col)
{
    if (!ra->insts || !rb->insts) return;          /* a rule failed to ground */

    /* union variable list, shared by name (sorts must agree) */
    var_bind uni[2 * MAX_ARGS];
    int nuni = 0;
    for (int i = 0; i < ra->nvars; i++) uni[nuni++] = ra->vars[i];
    for (int i = 0; i < rb->nvars; i++) {
        int j = var_index(uni, nuni, rb->vars[i].name);
        if (j < 0) uni[nuni++] = rb->vars[i];
        else if (uni[j].sort != rb->vars[i].sort) {
            serr(p, line, col,
                 "'%s > %s' shares variable '%s' at different sorts",
                 ra->label, rb->label, intern_name(p->syms, rb->vars[i].name));
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
    emit_sup_edges(p, ra, rb, s->bline, s->bcol);
}

/* Resolve a band name to its ladder and rank (0 = lowest). Returns false if no
 * declared ladder contains it. */
static bool find_band(parser *p, const char *name, int *ladder, int *rank)
{
    for (int li = 0; li < p->nladders; li++)
        for (int b = 0; b < p->ladders[li].nbands; b++)
            if (strcmp(p->ladders[li].band[b], name) == 0) {
                if (ladder) *ladder = li;
                if (rank) *rank = b;
                return true;
            }
    return false;
}

/* Two boolean judgment-rule heads conflict iff they assert complementary
 * literals of the same predicate (`p` vs `~p`). MV heads are already rejected
 * upstream (§5.7), and numeric-effect bands are out of scope (§5.8), so bands
 * apply to the boolean read-side only. */
static bool heads_conflict(const ast_rule *a, const ast_rule *b)
{
    if (a->head.pred != b->head.pred) return false;
    if (a->head.value != INTERN_NONE || b->head.value != INTERN_NONE) return false;
    if (a->head.is_guard || b->head.is_guard) return false;
    return a->head.neg != b->head.neg;
}

/* Desugar bands into pairwise `>` (§6.2): for each pair of banded judgment
 * rules on the SAME ladder whose heads conflict, append a synthetic superiority
 * edge (higher band > lower). Emitting into p->sups — rather than adding world
 * edges directly — is what makes bands *pure sugar*: grounding, the lane-family
 * taint analysis (which reads p->sups), and why-traces then treat a band edge
 * exactly like a hand-written `>`, and the engine never learns bands exist.
 * Same band = incomparable (no edge); different ladders or banded-vs-unbanded =
 * incomparable (as before bands existed). Runs before grounding. */
static void desugar_bands(parser *p)
{
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *ri = &p->rules[i];
        if (ri->band[0] == '\0') continue;
        int li, ranki;
        if (!find_band(p, ri->band, &li, &ranki)) continue;   /* errored in check_bands */
        for (int j = i + 1; j < p->nrules; j++) {
            ast_rule *rj = &p->rules[j];
            if (rj->band[0] == '\0') continue;
            int lj, rankj;
            if (!find_band(p, rj->band, &lj, &rankj)) continue;
            if (li != lj || ranki == rankj) continue;          /* incomparable */
            if (!heads_conflict(ri, rj)) continue;
            ast_rule *hi = ranki > rankj ? ri : rj;
            ast_rule *lo = ranki > rankj ? rj : ri;
            if (p->nsups >= MAX_SUPS) {
                serr(p, hi->band_line, hi->band_col,
                     "priority bands generated more than %d superiority edges — "
                     "raise MAX_SUPS or split the ladder", MAX_SUPS);
                return;
            }
            ast_sup *s = &p->sups[p->nsups++];
            snprintf(s->a, MAX_NAME, "%s", hi->label);
            snprintf(s->b, MAX_NAME, "%s", lo->label);
            s->aline = s->bline = hi->band_line;
            s->acol  = s->bcol  = hi->band_col;
        }
    }
}

/* Any predicate used in a condition that is neither a declared fluent nor a
 * rule head can never be true — the Osiris typo bug (§6.1). */
static void check_orphans(parser *p)
{
    for (int i = 0; i < p->nrefs; i++) {
        uint32_t a = p->refs[i].pred;
        pred_info *pi = find_pred(p, a);
        if (is_fluent_pred(p, a) || is_head_pred(p, a) || (pi && pi->is_provider))
            continue;
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
        case TK_BANDS:
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
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect || a->is_expr_guard)
        return false;                              /* MV / numeric / expr guard: out */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num || pi->is_provider)
        return false;                              /* providers are host-answered, not laned */
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
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect || a->is_expr_guard)
        return false;
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num || pi->is_provider || pi->arity != a->nargs)
        return false;                              /* providers are host-answered, not laned */
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
 * The step world is a homogeneous single-sort boolean one: per-entity fluents
 * are arity-1 over one sort S, actions/ramifications single-var over S. Globals
 * (arity-0 fluents) are allowed as broadcast READS in requires — a per-unit rule
 * gated by a shared flag — represented as a CUR local whose fact is the same in
 * every lane; being read-only here they need no primed/inertia (a global's next
 * value is its current one). Still bails to N=1 on: judgment rules, numeric/MV,
 * a global as an EFFECT (existential/aggregation — per-lane verdicts would
 * diverge from one global value), `unless`, or multi-sort. Built and validated
 * against the N=1 step family (world_step_lanes_check), the prototype-before-
 * adopt path the judgment lanes took (and now routed — see world_step). */

/* Index of a per-entity fluent pred in fpred[] (validated present when called). */
static int step_fidx(parser *p, const int *fpred, int nf, uint32_t pred)
{
    for (int i = 0; i < nf; i++)
        if (p->preds[fpred[i]].pred == pred) return i;
    return -1;
}

/* A boolean fluent read/write for a step rule: the action's own variable over S
 * (any polarity), or — for a READ (is_effect=false) — an arity-0 global, which
 * broadcasts to every lane. Globals as effects are deferred (return false). */
static bool step_atom_ok(parser *p, const ast_atom *a, int S, uint32_t var,
                         bool is_effect)
{
    if (a->is_guard || a->is_num_effect || a->value != INTERN_NONE)
        return false;                              /* numeric guard / MV: out */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || !pi->is_fluent || pi->is_mv || pi->is_num)
        return false;
    if (pi->arity == 0)
        return !is_effect;                         /* global: read-only broadcast */
    if (pi->arity != 1 || pi->argsort[0] != S)
        return false;
    return a->nargs == 1 && a->args[0].name == var;
}

/* S1: a numeric effect laneable iff it writes a numeric fluent arity-1 over S on
 * the action's own var with a constant-folding RHS (*konst gets the value). */
static bool num_eff_ok(parser *p, const ast_atom *e, int S, uint32_t var, long *konst)
{
    pred_info *pi = find_pred(p, e->pred);
    if (!pi || !pi->is_fluent || !pi->is_num) return false;
    if (pi->arity != 1 || pi->argsort[0] != S) return false;
    if (e->nargs != 1 || e->args[0].name != var) return false;
    return expr_fold(p, e->expr_root, konst);
}

#define MAX_LANE_NUMEFF 512

static void emit_step_lanes(parser *p)
{
    /* Judgment rules do not block the transition: a judgment never changes a
     * fluent (I1), so the next-state fluents are judgment-independent, and a step
     * rule that *reads* a judgment head is rejected by step_atom_ok below (not a
     * fluent) — bailing to N=1. So read-side judgments (queried by the host, not
     * gating any transition) can coexist with a laned step; they stay on jfam for
     * world_query. Incorporating judgment-gated step rules as derived lane locals
     * is the next widening. */

    /* the lane sort S: every per-entity fluent must be arity-1 over one shared
     * sort. Boolean fluents lane directly; numeric fluents (§5.8) become columns
     * committed column-parallel. Arity-0 booleans are read-only globals; a numeric
     * global, MV, or multi-sort bails the whole family. */
    int S = -1, fpred[MAX_PREDS], nf = 0, numpred[MAX_PREDS], nnp = 0;
    for (int i = 0; i < p->npreds; i++) {
        pred_info *pi = &p->preds[i];
        if (!pi->is_fluent)
            continue;
        if (pi->is_mv)
            return;
        if (pi->arity == 0) {
            if (pi->is_num) return;                 /* numeric global: not laned yet */
            continue;                               /* boolean global: on demand */
        }
        if (pi->arity != 1)
            return;
        if (S < 0) S = pi->argsort[0];
        else if (pi->argsort[0] != S) return;      /* multi-sort: bail */
        if (pi->is_num) numpred[nnp++] = i;
        else fpred[nf++] = i;
    }
    if ((nf == 0 && nnp == 0) || S < 0)
        return;
    int nent = domain_size(p, S);
    if (nent == 0)
        return;

    /* validate every action/ramification; collect the distinct action triggers
     * and the distinct global fluents read anywhere (broadcast read locals). */
    uint32_t apred[MAX_ACTIONS];
    int na = 0;
    uint32_t glob[MAX_PREDS];
    int ng = 0;
    bool act_has_num[MAX_ACTIONS], act_is_binder[MAX_ACTIONS];
    for (int i = 0; i < p->nactions; i++) { act_has_num[i] = false; act_is_binder[i] = false; }
    int neff_act[MAX_LANE_NUMEFF], neff_schema[MAX_LANE_NUMEFF], neff_op[MAX_LANE_NUMEFF];
    long neff_konst[MAX_LANE_NUMEFF];
    int nne = 0;
    /* binder items to lane (one numeric const effect per item): its action, item
     * index, target-numeric schema, op, constant. */
    int bitem_act[MAX_LANE_NUMEFF], bitem_it[MAX_LANE_NUMEFF];
    int bitem_schema[MAX_LANE_NUMEFF], bitem_op[MAX_LANE_NUMEFF];
    long bitem_konst[MAX_LANE_NUMEFF];
    int nbitem = 0;
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        if (a->nbind > 0) {
            /* a `for each` binder cast (e.g. Fireball): the binder's target var is
             * the lane axis and the cast is a broadcast trigger. First cut: one
             * caster var, one binder over S, no caster-side requires/effects,
             * boolean where/when guards over the target (arity-1 over S), and
             * constant-RHS numeric effects on the target. */
            if (a->neff != 0 || a->nreq != 0 || a->nvars != 1 || a->nbind != 1)
                return;
            ast_binder *bnd = &p->binders[a->bind_ix[0]];
            if (bnd->nvars != 1 || bnd->vars[0].sort != S) return;
            uint32_t tv = bnd->vars[0].name;             /* the target (lane) var */
            for (int b = 0; b < bnd->nwhere; b++)
                if (!step_atom_ok(p, &bnd->where[b], S, tv, false) ||
                    find_pred(p, bnd->where[b].pred)->arity != 1) return;
            for (int it = 0; it < bnd->nitems; it++) {
                binder_item *item = &bnd->items[it];
                long k;
                if (!item->eff.is_num_effect || !num_eff_ok(p, &item->eff, S, tv, &k)) return;
                int sc = -1;
                for (int j = 0; j < nnp; j++)
                    if (p->preds[numpred[j]].pred == item->eff.pred) { sc = j; break; }
                if (sc < 0) return;
                for (int b = 0; b < item->nwhen; b++)
                    if (!step_atom_ok(p, &item->when[b], S, tv, false) ||
                        find_pred(p, item->when[b].pred)->arity != 1) return;
                if (nbitem >= MAX_LANE_NUMEFF) return;
                bitem_act[nbitem] = i; bitem_it[nbitem] = it; bitem_schema[nbitem] = sc;
                bitem_op[nbitem] = (int)item->eff.numop; bitem_konst[nbitem] = k;
                nbitem++;
            }
            act_is_binder[i] = true;
            continue;                                    /* not a per-lane action */
        }
        if (a->nvars != 1 || a->vars[0].sort != S)
            return;
        uint32_t var = a->vars[0].name;
        for (int b = 0; b < a->nreq; b++) {
            if (!step_atom_ok(p, &a->requires[b], S, var, false)) return;
            if (find_pred(p, a->requires[b].pred)->arity == 0) {   /* a global read */
                uint32_t g = a->requires[b].pred;
                int found = -1;
                for (int j = 0; j < ng; j++) if (glob[j] == g) { found = j; break; }
                if (found < 0) { if (ng >= MAX_PREDS) return; glob[ng++] = g; }
            }
        }
        for (int b = 0; b < a->neff; b++) {
            ast_atom *e = &a->effects[b];
            if (e->is_num_effect) {                /* a numeric effect: lane it (S1) */
                long k;
                if (!num_eff_ok(p, e, S, var, &k)) return;   /* not laneable -> N=1 */
                if (nne >= MAX_LANE_NUMEFF) return;
                int sc = -1;
                for (int j = 0; j < nnp; j++)
                    if (p->preds[numpred[j]].pred == e->pred) { sc = j; break; }
                if (sc < 0) return;
                neff_act[nne] = i; neff_schema[nne] = sc;
                neff_op[nne] = (int)e->numop; neff_konst[nne] = k;
                nne++;
                act_has_num[i] = true;
            } else if (!step_atom_ok(p, e, S, var, true)) {
                return;
            }
        }
        if (!a->is_ramif) {
            uint32_t tr = intern_id(p->syms, a->name);
            int found = -1;
            for (int j = 0; j < na; j++) if (apred[j] == tr) { found = j; break; }
            if (found < 0) { if (na >= MAX_ACTIONS) return; apred[na++] = tr; }
        }
    }

    /* family locals: per per-entity fluent a current + a primed local; per read
     * global one CUR local (broadcast, read-only — no primed/inertia); per action
     * trigger one action local; per action with numeric effects one fired-marker
     * readout (a synthetic head `body -> marker`, read by the numeric commit).
     * cur/pri interleaved so index math stays local. */
    int nmark = 0, nbcast = 0;
    for (int i = 0; i < p->nactions; i++) {
        if (act_has_num[i]) nmark++;
        if (act_is_binder[i]) nbcast++;
    }
    int nloc = 2 * nf + ng + na + nmark + nbcast + nbitem;
    dlcol *f = dlcol_new(nloc, nent);
    int cur_local[MAX_PREDS], pri_local[MAX_PREDS], glob_local[MAX_PREDS];
    int inertia_pos[MAX_PREDS], inertia_neg[MAX_PREDS], act_local[MAX_ACTIONS];
    int marker_local[MAX_ACTIONS], bcast_local[MAX_ACTIONS], bmarker[MAX_LANE_NUMEFF];
    for (int i = 0; i < p->nactions; i++) { marker_local[i] = -1; bcast_local[i] = -1; }
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
    for (int j = 0; j < ng; j++) {
        glob_local[j] = n; kind[n] = WORLD_STEP_CUR;   /* broadcast read-only input */
        dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, glob[j]));
        n++;
    }
    for (int j = 0; j < na; j++) {
        act_local[j] = n; kind[n] = WORLD_STEP_ACTION;
        dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, apred[j]));
        n++;
    }
    /* fired markers — PRIMED-kind readouts with no fluent backing (fl_of -> -1),
     * so the boolean commit skips them; the numeric commit reads them per lane. */
    for (int i = 0; i < p->nactions; i++) if (act_has_num[i]) {
        marker_local[i] = n; kind[n] = WORLD_STEP_PRIMED;
        char mname[MAX_NAME + 8];
        snprintf(mname, sizeof mname, "fired:%s", p->actions[i].name);
        dlcol_set_atom_name(f, (uint32_t)n, mname);
        n++;
    }
    /* one broadcast-cast local per binder action, then one fired marker per item */
    for (int i = 0; i < p->nactions; i++) if (act_is_binder[i]) {
        bcast_local[i] = n; kind[n] = WORLD_STEP_BCAST;
        char cn[MAX_NAME + 8];
        snprintf(cn, sizeof cn, "cast:%s", p->actions[i].name);
        dlcol_set_atom_name(f, (uint32_t)n, cn);
        n++;
    }
    for (int k = 0; k < nbitem; k++) {
        bmarker[k] = n; kind[n] = WORLD_STEP_PRIMED;
        char mn[MAX_NAME + 16];
        snprintf(mn, sizeof mn, "fired:%s#%d", p->actions[bitem_act[k]].name, bitem_it[k]);
        dlcol_set_atom_name(f, (uint32_t)n, mn);
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
        if (act_is_binder[i]) continue;            /* binder marker rules built below */
        int nbody = a->nreq + (a->is_ramif ? 0 : 1);
        dl_lit body[MAX_BODY + 1];
        int bi = 0;
        for (int b = 0; b < a->nreq; b++) {
            uint32_t rp = a->requires[b].pred;
            int loc;
            if (find_pred(p, rp)->arity == 0) {        /* a global: broadcast read */
                int gj = -1;
                for (int j = 0; j < ng; j++) if (glob[j] == rp) { gj = j; break; }
                loc = glob_local[gj];                  /* read-only: global' == global */
            } else {
                int fi = step_fidx(p, fpred, nf, rp);
                loc = a->requires[b].primed ? pri_local[fi] : cur_local[fi];
            }
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
            if (a->effects[b].is_num_effect) continue;   /* numeric: the marker below */
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
        /* one fired marker per action with numeric effects: `body -> marker`.
         * Numeric effects don't defeat, so this defeasible head is +∂ exactly when
         * the body holds — matching the N=1 srule_fired test, per lane. */
        if (act_has_num[i]) {
            dl_lit mh = { (uint32_t)marker_local[i], false };
            char mname[MAX_NAME + 8];
            snprintf(mname, sizeof mname, "fired:%s", a->name);
            int mid = dlcol_add_rule(f, mname, DL_DEFEASIBLE, mh, body, nbody);
            dlcol_set_prov(f, mid, pbuf);
        }
    }

    /* binder fired markers: `cast & where(T) & when(T) => marker` — the broadcast
     * cast, ANDed with the per-lane boolean guards, decides which target lanes take
     * the effect. Numeric effects don't defeat, so a defeasible head suffices. */
    for (int k = 0; k < nbitem; k++) {
        int i = bitem_act[k], it = bitem_it[k];
        ast_action *a = &p->actions[i];
        ast_binder *bnd = &p->binders[a->bind_ix[0]];
        binder_item *item = &bnd->items[it];
        dl_lit body[MAX_BODY + 1];
        int bi = 0;
        body[bi++] = (dl_lit){ (uint32_t)bcast_local[i], false };
        for (int b = 0; b < bnd->nwhere && bi < MAX_BODY; b++) {
            int fi = step_fidx(p, fpred, nf, bnd->where[b].pred);
            int loc = bnd->where[b].primed ? pri_local[fi] : cur_local[fi];
            body[bi++] = (dl_lit){ (uint32_t)loc, bnd->where[b].neg };
        }
        for (int b = 0; b < item->nwhen && bi < MAX_BODY; b++) {
            int fi = step_fidx(p, fpred, nf, item->when[b].pred);
            int loc = item->when[b].primed ? pri_local[fi] : cur_local[fi];
            body[bi++] = (dl_lit){ (uint32_t)loc, item->when[b].neg };
        }
        char mn[MAX_NAME + 16];
        snprintf(mn, sizeof mn, "fired:%s#%d", a->name, it);
        char pbuf[MAX_NAME + 24];
        prov_str(p, bnd->line, pbuf, sizeof pbuf);
        int mid = dlcol_add_rule(f, mn, DL_DEFEASIBLE,
                                 (dl_lit){ (uint32_t)bmarker[k], false }, body, bi);
        dlcol_set_prov(f, mid, pbuf);
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
    for (int j = 0; j < ng; j++)
        for (int e = 0; e < nent; e++)
            ground[(size_t)glob_local[j] * nent + e] = glob[j];  /* broadcast (arity 0) */
    for (int j = 0; j < na; j++)
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            ground[(size_t)act_local[j] * nent + e] = ground_pred(p, apred[j], &ent, 1);
        }
    for (int i = 0; i < p->nactions; i++) if (act_has_num[i]) {   /* marker: a non-fluent atom */
        char mname[MAX_NAME + 16];
        snprintf(mname, sizeof mname, "fired:%s#", p->actions[i].name);
        uint32_t ma = intern_id(p->syms, mname);
        for (int e = 0; e < nent; e++) ground[(size_t)marker_local[i] * nent + e] = ma;
    }
    for (int i = 0; i < p->nactions; i++) if (act_is_binder[i]) {   /* bcast local: non-fluent */
        char cn[MAX_NAME + 16];
        snprintf(cn, sizeof cn, "cast:%s#", p->actions[i].name);
        uint32_t ca = intern_id(p->syms, cn);
        for (int e = 0; e < nent; e++) ground[(size_t)bcast_local[i] * nent + e] = ca;
    }
    for (int k = 0; k < nbitem; k++) {                             /* binder marker: non-fluent */
        char mn[MAX_NAME + 24];
        snprintf(mn, sizeof mn, "fired:%s#%d#", p->actions[bitem_act[k]].name, bitem_it[k]);
        uint32_t ma = intern_id(p->syms, mn);
        for (int e = 0; e < nent; e++) ground[(size_t)bmarker[k] * nent + e] = ma;
    }

    world_add_step_lane_family(p->w, f, nloc, nent, ground, kind);

    /* numeric lane extension: per-schema ground columns + all effect specs (slice-1
     * per-lane effects and binder items), each pointing at its fired-marker local. */
    if (nnp > 0) {
        uint32_t *numcell = malloc((size_t)nnp * nent * sizeof *numcell);
        for (int s = 0; s < nnp; s++) {
            uint32_t P = p->preds[numpred[s]].pred;
            for (int e = 0; e < nent; e++) {
                uint32_t ent = domain_at(p, S, e);
                numcell[(size_t)s * nent + e] = ground_pred(p, P, &ent, 1);
            }
        }
        int sc_schema[2 * MAX_LANE_NUMEFF], sc_op[2 * MAX_LANE_NUMEFF], nspec = 0;
        long sc_konst[2 * MAX_LANE_NUMEFF];
        uint32_t effmark[2 * MAX_LANE_NUMEFF];
        for (int k = 0; k < nne; k++) {
            sc_schema[nspec] = neff_schema[k]; sc_op[nspec] = neff_op[k];
            sc_konst[nspec] = neff_konst[k];
            effmark[nspec] = (uint32_t)marker_local[neff_act[k]]; nspec++;
        }
        for (int k = 0; k < nbitem; k++) {
            sc_schema[nspec] = bitem_schema[k]; sc_op[nspec] = bitem_op[k];
            sc_konst[nspec] = bitem_konst[k];
            effmark[nspec] = (uint32_t)bmarker[k]; nspec++;
        }
        world_step_lane_set_numeric(p->w, nnp, numcell, nspec,
                                    sc_schema, sc_op, sc_konst, effmark);
        free(numcell);
    }

    /* register broadcast cast atoms: every ground `action(caster)` -> its BCAST
     * local, so the discrete cast fans out over the target lanes. */
    if (nbcast > 0) {
        int total = 0;
        for (int i = 0; i < p->nactions; i++)
            if (act_is_binder[i]) total += domain_size(p, p->actions[i].vars[0].sort);
        uint32_t *catom = malloc((size_t)(total ? total : 1) * sizeof *catom);
        int *clocal = malloc((size_t)(total ? total : 1) * sizeof *clocal);
        int ncast = 0;
        for (int i = 0; i < p->nactions; i++) if (act_is_binder[i]) {
            int Sc = p->actions[i].vars[0].sort, kc = domain_size(p, Sc);
            uint32_t nameatom = intern_id(p->syms, p->actions[i].name);
            for (int c = 0; c < kc; c++) {
                uint32_t ent = domain_at(p, Sc, c);
                catom[ncast] = ground_pred(p, nameatom, &ent, 1);
                clocal[ncast] = bcast_local[i];
                ncast++;
            }
        }
        world_step_lane_set_bcast(p->w, ncast, catom, clocal);
        free(catom); free(clocal);
    }
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
    p->binders = calloc(MAX_BINDERS, sizeof *p->binders);
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
        case TK_SCENE:
        case TK_MODULE:
        case TK_EXTEND: parse_module_header(p); break;
        case TK_SORT:   parse_sort(p);   break;
        case TK_ENUM:   parse_enum(p);   break;
        case TK_ENTITY: parse_entity(p); break;
        case TK_STATE:  parse_state(p);  break;
        case TK_PROVIDER: parse_provider(p); break;
        case TK_INIT:   parse_init(p);   break;
        case TK_RULE:   parse_rule(p);   break;
        case TK_ACTION: parse_action(p); break;
        case TK_BANDS:  parse_bands(p);  break;
        case TK_IDENT:  parse_sup(p);    break;
        default: {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a declaration "
                 "(scene/sort/enum/entity/state/init/rule/action/bands) "
                 "or a superiority statement, found %s", d);
            break;
        }
        }
        if (p->err_flag) synchronize(p);
        p->ndecls++;
    }

    world *result = NULL;
    if (p->nerrors == 0) {
        /* pass 2: semantic analysis, then build-time grounding */
        semantic_pass(p);
        if (p->nerrors == 0) {
            desugar_bands(p);                     /* band ladders → pairwise `>` (§6.2) */
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
    free(p->binders);
    free(p->exprs);
    free(p->ents);
    free(p->ent_of);
    free(p->ent_pos);
    for (int s = 0; s < p->nsorts; s++) free(p->domain_ents[s]);
    free(p);
    return result;
}
