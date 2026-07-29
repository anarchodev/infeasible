#include "lang/story.h"
#include "lang/lexer.h"
#include "state/factindex.h"

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
#define MAX_MEMPOOL    1024    /* membership-list values across a file (#95) */
#define MAX_LAYERS     8       /* guarded definitions per value (#82/#94) */
#define MAX_FLUENTS    256     /* fluent *predicate* schemas */
#define MAX_PREDS      512     /* predicate registry (fluents + heads) */
#define MAX_RULES      256
#define MAX_ACTIONS    128
#define MAX_SUPS       256
#define MAX_INITS      (1 << 24)   /* runaway ceiling only — the list grows to fit */
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
#define INT_SORT       (-3)        /* provider-arg sort sentinel for `int` (§5.6) */

/* ---- AST ------------------------------------------------------------ */

/* `is_int`: a numeric literal argument (`near(X, Y, 2)`, §5.6). `name` is the
 * interned decimal string ("2"), so grounding and the ground-term rendering are
 * unchanged; `ival` is the value. Legal only in an `int` provider-arg position. */
typedef struct { uint32_t name; int line, col; bool is_int; long ival; } ast_arg;

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
    bool        is_valuedef;   /* head only: `v(args) = expr` defines a declared
                                * `value` (#82); lhs_root holds the definition expr */
    bool        is_member;     /* `X in { v, … }` finite-domain membership (#95):
                                * args[0] = the element var, mem_ix/mem_n index the
                                * mempool, `neg` = `not in`. A static grounding
                                * filter — never emitted, never in the fixpoint. */
    int         mem_ix, mem_n;
    uint32_t    as_value;      /* `-= e as fire` (#83): the damage-type enum value
                                * this contribution accumulates under; 0 = untyped */
    int       line, col;
} ast_atom;

/* Effect-RHS expression tree (§5.8), interned into a parser-owned node pool.
 * A leaf is a constant (`4`) or a numeric-fluent read (`hp`, `hp(X)`); interior
 * nodes are the closed arithmetic set. Grounding walks the tree per instance,
 * folding constant subtrees and emitting VM bytecode for the rest. */
typedef enum {
    EX_CONST, EX_LOAD, EX_ROLL, EX_CALL, EX_ADD, EX_SUB, EX_MUL, EX_DIV, EX_NEG,
    EX_MIN, EX_MAX,
    EX_TEST,  /* `test([~]p(args))` (#86): a literal's solved verdict as 0/1.
               * `pred`/`args` = the literal; `konst` = 1 for a `~` literal. */
    EX_PRIOR  /* `prior` (#82/#94): inside a layered definition's expression,
               * the value that would have held without this definition —
               * compiles to EXPR_P over the chain's running value. */
} ex_kind;
/* EX_CALL (§5.6): a value-returning function-provider call `f(e1, …, ek)`.
 * `pred` = the function name; `nargs` = k; `cargs[0..k)` = child expr indices. */
/* EX_ROLL (§5.10): a seeded die. `konst` = sides, `lhs` = an author disambiguator
 * tag (0 default). The roll site is keyed by (this node, the binding, tag). */

typedef struct {
    ex_kind  kind;
    long     konst;           /* EX_CONST */
    uint32_t pred;            /* EX_LOAD: numeric fluent; EX_CALL: function name */
    int      nargs;           /* EX_LOAD: arg names; EX_CALL: call args */
    ast_arg  args[MAX_ARGS];  /* EX_LOAD */
    int      cargs[MAX_ARGS]; /* EX_CALL: child expr node indices */
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
    int          vclass;          /* valuedef layer class (#94): 0 override,
                                   * 1 prior+e, 2 max(prior,e), 3 min(prior,e),
                                   * 4 general prior use */
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
    /* `name : DOMAIN` params (§5.6/§13): a single opaque host value, likewise
     * kept out of the entity var list. In a provider read the param resolves to
     * a stable placeholder atom (its name) that the host maps to the value from
     * its provider context — the engine never sees the value. Held as var_binds
     * (with the domain sort) so they extend the guard-checking scope. */
    var_bind  dparams[MAX_ARGS];
    int       ndparams;
    ast_atom  requires[MAX_BODY];
    int       nreq;
    ast_atom  effects[MAX_BODY];
    int       neff;
    int       stratum;            /* §5.8 strata (#87), assigned by stratify_steps */
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
    uint32_t val_sort;            /* `: cell` — valued in an entity sort's entities
                                   * (§5.6 functional fluent); 0 if not. Values are
                                   * populated from the sort after resolve_entities. */
    bool     is_num;              /* declared `: int` (§5.8) — or store-backed cell */
    bool     is_cell;             /* `: <domain>` — store-backed opaque handle (§5.6):
                                   * one uint32/entity in the value store, host-set/read,
                                   * copied with `:=`; no arithmetic, no int guards */
    bool     has_range;           /* declared `in lo..hi` — the clamp range */
    int      merge_mode;          /* `merge min|max` (#85): 0 register, 1 min, 2 max */
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
    int      val_sort;            /* value sort index for `: cell` fluents, else -1;
                                   * lets `at(X) = c` bind `c` as a join variable */
    bool     is_num;              /* a numeric fluent (§5.8) */
    bool     is_cell;             /* a store-backed opaque-domain fluent (§5.6) */
    bool     is_provider;         /* a computed relation, host-answered (§5.6) */
    bool     is_value;            /* an engine-derived value (#82): defined by a
                                   * rule, inlined at read sites, never stored */
} pred_info;

/* A value-returning function provider (§5.6): `function f(t1,…) : ret`. Unlike a
 * boolean provider (a relation), a function returns a value used in effect
 * expressions (EX_CALL) — e.g. `neighbor(cell, dir) : cell` for movement. Arg and
 * return types are declared sorts/domains, or `int`; they are recorded for
 * arity/return-type checks but the value is opaque to the engine (host-computed). */
typedef struct {
    uint32_t name;
    int      nargs;
    uint32_t argsort[MAX_ARGS];   /* arg type name atom; INTERN_NONE means `int` */
    uint32_t ret;                 /* return type name atom; INTERN_NONE means `int` */
    int      line, col;
} ast_function;

typedef struct {
    lexer        lx;
    token        cur;
    intern      *syms;
    const char  *srcname;         /* source file name, for provenance (§6.3) */
    world       *w;
    story_diags *diags;
    int          nerrors;
    bool         err_flag;        /* an error hit in the current declaration */
    bool         ground_matched;  /* ground eligible rules via the join matcher
                                   * (#28) rather than the eager odometer */
    bool         sparse;          /* #92 (tick-time only): skip the boolean state
                                   * cross-product; declare fluents on touch */
    factindex   *fidx;            /* base-fluent extension index, built from init
                                   * facts when ground_matched (the matcher scans it) */
    int          ndecls;          /* declarations parsed so far (header must be first) */

    /* Flat top-level `scene NAME` header (§4.1/§6.4). A single scene is one
     * partition — semantically invisible (no atom is qualified), because there
     * is no second scope to import from yet; the name is recorded for future
     * provenance/imports (§5.5, M4). */
    char         scene_name[MAX_NAME];
    int          scene_line, scene_col;
    bool         has_scene;

    /* `is_domain`: an opaque value domain (§5.6/§13), declared with `domain`.
     * A domain is a sort the engine never enumerates — it has no entities and
     * cannot be grounded over; its values are host-minted handles that appear
     * only as provider/action-param arg types. Sharing the sort table lets the
     * arity/sort checks treat a domain arg like any other. */
    /* `is_enum`: a sort synthesized from an `enum` (#96) — a finite, declared,
     * ground domain whose "entities" are the enum's values, so enum-typed
     * arguments, rule variables, and membership ride the existing odometer.
     * Values are not entities: `entity x : <enum>` is rejected. */
    struct { char name[MAX_NAME]; int line, col; bool is_domain; bool is_enum; } sorts[MAX_SORTS];
    int nsorts;
    struct ent_rec { uint32_t atom; int sort; int line, col; } *ents;  /* heap, grown */
    int nents, capents;
    int nuserents;                /* ents[0..nuserents) authored; rest = enum values (#96) */
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
    ast_function functions[MAX_FLUENTS];  /* value-returning fn providers (§5.6) */
    int nfunctions;
    ast_fluent  valuedecls[MAX_FLUENTS];  /* engine-derived values (#82): `value v(…) : int` */
    int nvaluedecls;
    int dtype_sort;               /* #83: the ONE enum-sort damage types come from
                                   * (-1 until an `as` typed contribution is seen) */
    bool has_pguards;             /* #87: any primed numeric guard — strata exist,
                                   * step lanes bail, world steps N=1 */
    int value_def[MAX_FLUENTS];           /* per value: the BASE definition (the one
                                           * unconditional rule), -1 = none yet */
    int vdefs[MAX_FLUENTS][MAX_LAYERS + 1];   /* all defs, declaration order (#82/#94) */
    int nvdefs[MAX_FLUENTS];
    int value_layers[MAX_FLUENTS][MAX_LAYERS];/* guarded defs, CHAIN order (bottom->top) */
    int value_nlayers[MAX_FLUENTS];
    int *vmark_of; uint32_t vmark_cap;    /* marker atom -> grounded flag (dedup) */
    bool in_valuedef_expr;                /* `prior` legality context */
    int vdepth;                           /* value-inline recursion depth (cycle backstop) */
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
    ast_atom   *inits;                  /* grown geometrically (§ loud failures) */
    int ninits, capinits;

    ex_node    *exprs;            /* heap; MAX_EXPRS effect-expression nodes */
    int nexprs;

    uint32_t    mempool[MAX_MEMPOOL];   /* membership-list values (#95), atoms */
    int nmempool;

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

/* Does interned atom `a` spell `word`? (parser has p->syms.) */
static bool ident_atom_is(parser *p, uint32_t a, const char *word)
{
    return strcmp(intern_name(p->syms, a), word) == 0;
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

/* domain := 'domain' ( IDENT | '(' (','? IDENT)* ')' )  -- opaque value domain
 * (§5.6/§13). Same shape as `sort`, but flags the entry so the engine never
 * enumerates it: its values are host-minted handles that appear only as
 * provider/action-param arg types. */
static void parse_domain(parser *p)
{
    advance(p);                                    /* 'domain' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a domain name, found %s", d);
            return;
        }
        if (p->nsorts >= MAX_SORTS) {
            fail(p, p->cur.line, p->cur.col, "too many sorts/domains (max %d)", MAX_SORTS);
            return;
        }
        copy_ident(p->sorts[p->nsorts].name, MAX_NAME, p->cur);
        p->sorts[p->nsorts].line = p->cur.line;
        p->sorts[p->nsorts].col = p->cur.col;
        p->sorts[p->nsorts].is_domain = true;
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

/* A declared engine-derived value (#82), matched by name; -1 if none. Values
 * must be declared before the rules that read or define them (the same
 * decl-before-use contract `function` calls already rely on in parse_factor). */
static int find_value(parser *p, uint32_t name)
{
    for (int i = 0; i < p->nvaluedecls; i++)
        if (p->valuedecls[i].pred == name) return i;
    return -1;
}

/* ---- effect-expression parser (§5.8) --------------------------------
 *
 *   expr   := term (('+'|'-') term)*
 *   term   := factor (('*'|'/') factor)*      -- '/' floors (rounds toward -inf)
 *   factor := '-' factor | INT
 *           | ('min'|'max'|'divup') '(' expr ',' expr ')'  -- divup = ceiling div
 *           | 'test' '(' ['~'] IDENT [ '(' arg* ')' ] ')'  -- verdict as 0/1 (#86)
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
static int find_function(parser *p, uint32_t name);

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
        bool isdivup = ident_is(id, "divup");
        advance(p);
        if (ident_is(id, "prior") && p->cur.kind != TK_LPAREN) {
            /* `prior` (#82/#94): the value a layered definition would have had
             * without this layer; legality (definitions only) checked later */
            return alloc_expr(p, EX_PRIOR, id.line, id.col);
        }
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
        if (ident_is(id, "test") && p->cur.kind == TK_LPAREN) {
            /* `test([~]p(args))` (#86): a boolean literal's verdict as 0/1 */
            advance(p);
            int n = alloc_expr(p, EX_TEST, id.line, id.col);
            if (n < 0) return -1;
            if (p->cur.kind == TK_TILDE) { p->exprs[n].konst = 1; advance(p); }
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an atom inside test(…), found %s", d);
                return -1;
            }
            p->exprs[n].pred = intern_tok(p, p->cur);
            advance(p);
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
                        fail(p, p->cur.line, p->cur.col,
                             "too many arguments (max %d)", MAX_ARGS);
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
            if (!expect(p, TK_RPAREN)) return -1;
            return n;
        }
        if ((ismin || ismax || isdivup) && p->cur.kind == TK_LPAREN) {
            /* min/max(a, b), divup(a, b) */
            advance(p);
            int a = parse_expr(p);
            if (a < 0) return -1;
            if (!expect(p, TK_COMMA)) return -1;
            int b = parse_expr(p);
            if (b < 0) return -1;
            if (!expect(p, TK_RPAREN)) return -1;
            if (isdivup) {
                /* ceiling division ("rounded up", the 5e per-feature exception
                 * to the global round-down rule): desugar to -((-a) / b) — the
                 * floor/ceil identity, so the dual semantics can never drift
                 * from EXPR_DIV and fold/VM agree by construction. */
                long dv;
                if (expr_fold(p, b, &dv) && dv == 0) {
                    fail(p, id.line, id.col, "division by a constant zero");
                    return -1;
                }
                int na = alloc_expr(p, EX_NEG, id.line, id.col);
                if (na < 0) return -1;
                p->exprs[na].lhs = a;
                int nd = alloc_expr(p, EX_DIV, id.line, id.col);
                if (nd < 0) return -1;
                p->exprs[nd].lhs = na;
                p->exprs[nd].rhs = b;
                int nn = alloc_expr(p, EX_NEG, id.line, id.col);
                if (nn < 0) return -1;
                p->exprs[nn].lhs = nd;
                return nn;
            }
            int n = alloc_expr(p, ismin ? EX_MIN : EX_MAX, id.line, id.col);
            if (n < 0) return -1;
            p->exprs[n].lhs = a;
            p->exprs[n].rhs = b;
            return n;
        }
        if (find_function(p, intern_tok(p, id)) >= 0 && p->cur.kind == TK_LPAREN) {
            /* a value-returning function-provider call `f(e1, …, ek)` (§5.6):
             * arguments are expressions (a cell read `at(X)`, a direction int),
             * not bare arg names — distinguished from a fluent read by the callee
             * being a declared `function`. */
            advance(p);                                        /* '(' */
            int n = alloc_expr(p, EX_CALL, id.line, id.col);
            if (n < 0) return -1;
            p->exprs[n].pred = intern_tok(p, id);
            for (;;) {
                int a = parse_expr(p);
                if (a < 0) return -1;
                if (p->exprs[n].nargs >= MAX_ARGS) {
                    fail(p, id.line, id.col, "too many arguments (max %d)", MAX_ARGS);
                    return -1;
                }
                p->exprs[n].cargs[p->exprs[n].nargs++] = a;
                if (p->cur.kind == TK_COMMA) { advance(p); continue; }
                break;
            }
            if (!expect(p, TK_RPAREN)) return -1;
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
    while (p->cur.kind == TK_STAR || p->cur.kind == TK_SLASH) {
        bool isdiv = p->cur.kind == TK_SLASH;
        token o = p->cur; advance(p);
        int r = parse_factor(p);
        if (r < 0) return -1;
        long dv;
        if (isdiv && expr_fold(p, r, &dv) && dv == 0) {
            fail(p, o.line, o.col, "division by a constant zero");
            return -1;
        }
        int n = alloc_expr(p, isdiv ? EX_DIV : EX_MUL, o.line, o.col);
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
     * conjunct starts with something a boolean atom can't: a `roll`/`min`/`max`/
     * `divup` function call, an int, `(`, `-`, or a declared `value` (#82).
     * Covers the d20: `roll(20) + atk(A) >= ac(T)`, `max(roll(20,1),
     * roll(20,2)) + atk >= ac`, and `atk_roll(A,T) + atk(A) >= ac(T)`. */
    if (ident_is(p->cur, "roll") || ident_is(p->cur, "min") || ident_is(p->cur, "max") ||
        ident_is(p->cur, "divup") ||
        (p->cur.kind == TK_IDENT && find_value(p, intern_tok(p, p->cur)) >= 0) ||
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
            ex_node *lv = &p->exprs[lhs];
            if ((p->cur.kind == TK_ASSIGN || p->cur.kind == TK_PLUSEQ ||
                 p->cur.kind == TK_MINUSEQ) && lv->kind == EX_LOAD &&
                find_value(p, lv->pred) >= 0) {
                fail(p, lead.line, lead.col,
                     "'%s' is a derived value — it cannot be written or caused; "
                     "change the state its definition reads, or redefine it "
                     "(`rule <label>: => %s(…) = expr`)",
                     intern_name(p->syms, lv->pred), intern_name(p->syms, lv->pred));
                return false;
            }
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a comparison (<=, <, >=, >, =) in a roll/expression guard, found %s", d);
            return false;
        }
        advance(p);
        int rhs = parse_expr(p);
        if (rhs < 0) return false;
        ex_node *ln = &p->exprs[lhs];
        if (op == WORLD_CMP_EQ && !out->neg && ln->kind == EX_LOAD &&
            find_value(p, ln->pred) >= 0) {
            /* bare `v(args) = expr` (#82): a value definition in head position,
             * an equality guard in a body — the semantic pass disambiguates */
            out->pred = ln->pred;
            out->nargs = ln->nargs;
            for (int k = 0; k < ln->nargs; k++) out->args[k] = ln->args[k];
            out->is_valuedef = true;
            out->lhs_root = rhs;
            out->line = lead.line; out->col = lead.col;
            return true;
        }
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
        if (p->cur.kind == TK_LBRACE) {            /* finite-domain membership (#95) */
            advance(p);
            out->args[0].name = out->pred;         /* the element var */
            out->args[0].line = id.line;
            out->args[0].col = id.col;
            out->nargs = 1;
            out->pred = INTERN_NONE;               /* not a predicate — a filter */
            out->is_member = true;
            out->neg ^= notin;
            out->mem_ix = p->nmempool;
            for (;;) {
                if (p->cur.kind != TK_IDENT) {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected a value name in a membership list, found %s", d);
                    return false;
                }
                if (p->nmempool >= MAX_MEMPOOL) {
                    fail(p, p->cur.line, p->cur.col,
                         "too many membership-list values in this file (max %d)",
                         MAX_MEMPOOL);
                    return false;
                }
                p->mempool[p->nmempool++] = intern_tok(p, p->cur);
                out->mem_n++;
                advance(p);
                if (p->cur.kind == TK_COMMA) { advance(p); continue; }
                break;
            }
            return expect(p, TK_RBRACE);
        }
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a `set of` parameter name after `in` (or '{' for a "
                 "membership list), found %s", d);
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
            if (p->cur.kind != TK_IDENT && p->cur.kind != TK_INT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected an argument name or integer, found %s", d);
                return false;
            }
            if (out->nargs >= MAX_ARGS) {
                fail(p, p->cur.line, p->cur.col,
                     "too many arguments (max %d)", MAX_ARGS);
                return false;
            }
            ast_arg *ag = &out->args[out->nargs];
            ag->name = intern_tok(p, p->cur);      /* IDENT name or the digit string */
            ag->line = p->cur.line;
            ag->col = p->cur.col;
            ag->is_int = (p->cur.kind == TK_INT);  /* a numeric literal arg (§5.6) */
            ag->ival = ag->is_int ? p->cur.ival : 0;
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
        if (p->cur.kind == TK_AS) {            /* typed contribution (#83) */
            advance(p);
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected a damage-type value after `as`, found %s", d);
                return false;
            }
            out->as_value = intern_tok(p, p->cur);
            advance(p);
        }
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
static int find_sort(parser *p, uint32_t name_atom);   /* defined below */

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
            /* `merge min|max` (#85): the ASSIGN-class algebra. Contextual
             * keyword — a fluent literally named `merge` following an `: int`
             * in a grouped `state ( … )` will misparse here; the located error
             * makes the fix obvious. */
            if (ident_is(p->cur, "merge")) {
                advance(p);
                if (ident_is(p->cur, "min"))      f->merge_mode = 1;
                else if (ident_is(p->cur, "max")) f->merge_mode = 2;
                else {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected `min` or `max` after `merge` — the algebra "
                         "set is closed (#85; host merges spend I4), found %s", d);
                    return false;
                }
                advance(p);
            }
            f->is_num = true;
            return true;
        }
        if (p->cur.kind == TK_IDENT) {             /* `: enumname` or `: sortname` */
            int ei = find_enum(p, p->cur);
            if (ei >= 0) {                          /* a named enum value domain */
                f->is_mv = true;
                f->nvalues = p->enums[ei].nvalues;
                for (int v = 0; v < f->nvalues; v++) f->values[v] = p->enums[ei].values[v];
                advance(p);
                return true;
            }
            int si = find_sort(p, intern_tok(p, p->cur));
            if (si >= 0 && !p->sorts[si].is_domain) {
                /* `: cell` — a functional fluent valued in an entity sort (§5.6):
                 * exactly one entity per tick, logic-backed over §5.7 (values =
                 * the sort's entities, filled after resolve_entities). */
                f->is_mv = true;
                f->val_sort = intern_tok(p, p->cur);
                advance(p);
                return true;
            }
            if (si >= 0 && p->sorts[si].is_domain) {
                /* `: cell` where cell is a `domain` (§5.6) — a STORE-BACKED
                 * functional fluent: one opaque uint32 handle per entity in the
                 * value store (never |cells| atoms). Host-set and host-read;
                 * copied with `:=`; equality is read through a provider. Reuses
                 * the numeric store, tagged is_cell to forbid arithmetic. */
                f->is_num = true;
                f->is_cell = true;
                f->val_sort = intern_tok(p, p->cur);
                advance(p);
                return true;
            }
            fail(p, p->cur.line, p->cur.col,
                 "'%.*s' is not a declared enum, sort, or domain; a fluent value "
                 "type is `: int`, `: { … }`, an `enum`, a `sort`, or a `domain`",
                 p->cur.len, p->cur.start);
            return false;
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

/* value := 'value' ( vdecl | '(' vdecl* ')' ) ; vdecl := fdecl with `: int`.
 * An engine-derived value (#82): declared here (name + type, so definitions can
 * spread across rules and a typo'd read is a located error, not a silent new
 * predicate), defined by a `rule … => v(args) = expr` head, and inlined at
 * every read site. Never stored, never a fluent — no inertia, no effects. */
static void parse_value(parser *p)
{
    advance(p);                                    /* 'value' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a value name, found %s", d);
            return;
        }
        if (p->nvaluedecls >= MAX_FLUENTS) {
            fail(p, p->cur.line, p->cur.col, "too many values (max %d)", MAX_FLUENTS);
            return;
        }
        ast_fluent *v = &p->valuedecls[p->nvaluedecls];
        if (!parse_fdecl(p, v)) return;
        if (!v->is_num || v->is_mv || v->is_cell) {
            fail(p, v->line, v->col,
                 "a value needs a return type, and only `: int` is supported "
                 "in this slice (#82) — enum-valued values are a later slice");
            return;
        }
        if (v->has_range) {
            fail(p, v->line, v->col,
                 "a value has no clamp range — a range clamps stored state "
                 "(§5.8); a derived value is whatever its definition computes");
            return;
        }
        if (v->merge_mode) {
            fail(p, v->line, v->col,
                 "a value has no merge algebra — competing definitions combine "
                 "by defeat and `prior` (#82/#94), not by merging effects");
            return;
        }
        p->value_def[p->nvaluedecls] = -1;
        p->nvaluedecls++;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

static int find_function(parser *p, uint32_t name)
{
    for (int i = 0; i < p->nfunctions; i++)
        if (p->functions[i].name == name) return i;
    return -1;
}

/* One type token in a function signature: a declared sort/domain name, or `int`.
 * Returns the type name atom, INTERN_NONE for `int`; sets *ok=false on error. */
static uint32_t parse_type_token(parser *p, bool *ok)
{
    *ok = true;
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col,
             "expected a type name (a sort, domain, or `int`), found %s", d);
        *ok = false;
        return INTERN_NONE;
    }
    if (ident_is(p->cur, "int")) { advance(p); return INTERN_NONE; }
    uint32_t t = intern_tok(p, p->cur);
    advance(p);
    return t;
}

/* function := 'function' IDENT '(' type (',' type)* ')' ':' type
 * A value-returning host function (§5.6): args/return are declared sorts/domains
 * (or `int`). Registered so a call `f(…)` in an effect expression grounds to an
 * EX_CALL; the returned value is opaque to the engine (host-computed, I4). */
static void parse_function(parser *p)
{
    advance(p);                                    /* 'function' */
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a function name, found %s", d);
        return;
    }
    if (p->nfunctions >= MAX_FLUENTS) {
        fail(p, p->cur.line, p->cur.col, "too many functions (max %d)", MAX_FLUENTS);
        return;
    }
    ast_function *fn = &p->functions[p->nfunctions];
    memset(fn, 0, sizeof *fn);
    fn->name = intern_tok(p, p->cur);
    fn->line = p->cur.line;
    fn->col = p->cur.col;
    advance(p);
    if (!expect(p, TK_LPAREN)) return;
    for (;;) {
        if (fn->nargs >= MAX_ARGS) {
            fail(p, fn->line, fn->col, "too many function arguments (max %d)", MAX_ARGS);
            return;
        }
        bool ok;
        uint32_t t = parse_type_token(p, &ok);
        if (!ok) return;
        fn->argsort[fn->nargs++] = t;
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    if (!expect(p, TK_RPAREN)) return;
    if (!expect(p, TK_COLON)) return;
    bool ok;
    fn->ret = parse_type_token(p, &ok);
    if (!ok) return;
    p->nfunctions++;
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
        if (p->ninits == p->capinits) {            /* grow to fit — no fixed cap */
            if (p->capinits >= MAX_INITS) {        /* runaway ceiling: scream loudly */
                fail(p, p->cur.line, p->cur.col,
                     "more than %d init facts — this is almost certainly a runaway, "
                     "not real content", MAX_INITS);
                return;
            }
            p->capinits *= 2;
            p->inits = realloc(p->inits, (size_t)p->capinits * sizeof *p->inits);
        }
        ast_atom a;
        if (!parse_atom(p, &a)) return;
        if (a.neg) {
            fail(p, a.line, a.col,
                 "init lists facts that start true; a negated init is redundant "
                 "(everything unlisted is closed-world false)");
            return;
        }
        if (a.is_member || a.is_expr_guard) {
            fail(p, a.line, a.col,
                 "init lists ground facts — a membership/comparison test is not a fact");
            return;
        }
        p->inits[p->ninits++] = a;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* params := '(' vbind (',' vbind)* ')'; vbind := IDENT ':' IDENT */
static bool is_declared_domain_tok(parser *p, token t)
{
    for (int i = 0; i < p->nsorts; i++)
        if (p->sorts[i].is_domain && ident_is(t, p->sorts[i].name)) return true;
    return false;
}

/* `act` (nullable) receives `set of SORT` and `: DOMAIN` params — actions only;
 * rules pass NULL, so either there is a located error. A domain must be declared
 * before the action that uses it (so the type is classifiable at parse time). */
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
        if (p->cur.kind == TK_IDENT && is_declared_domain_tok(p, p->cur)) {
            if (!act) {
                fail(p, nm.line, nm.col,
                     "domain parameters are only allowed on actions");
                return false;
            }
            if (act->ndparams >= MAX_ARGS) {
                fail(p, nm.line, nm.col, "too many domain parameters (max %d)", MAX_ARGS);
                return false;
            }
            var_bind *dv = &act->dparams[act->ndparams++];
            dv->name = intern_tok(p, nm);
            dv->line = nm.line;
            dv->col = nm.col;
            dv->sort = -(int)intern_tok(p, p->cur) - 2;   /* encoded, resolved in semantic */
            advance(p);                            /* the domain type */
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

    /* An arrow directly after ':' is an empty body — legal only for a value
     * definition (`rule L(…): => v(…) = expr`, #82); the semantic pass rejects
     * an empty-bodied boolean rule. */
    int nb = 0;
    if (p->cur.kind != TK_ARROW && p->cur.kind != TK_FATARROW &&
        p->cur.kind != TK_SQARROW) {
        nb = parse_conj(p, r->body, MAX_BODY);
        if (nb < 0) return;
    }
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
    pi->val_sort = -1;
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

/* #96: an enum is a finite, declared, ground domain — the same property that
 * makes a sort groundable. Synthesize a sort per enum whose "entities" are the
 * enum's values, so `resistant(actor, damage_type)`, `D : damage_type` rule
 * variables, and `D in { … }` all ride the existing odometer, arg checks, and
 * lane machinery with no new grounding shape. The distinction preserved:
 * values are NOT entities — `entity x : <enum>` is rejected below, and a value
 * name colliding with an entity (or another enum's value) is a duplicate
 * error, because an atom must resolve to exactly one domain. */
static void synthesize_enum_sorts(parser *p)
{
    p->nuserents = p->nents;
    for (int i = 0; i < p->nenums; i++) {
        enum_dom *e = &p->enums[i];
        if (find_sort(p, intern_id(p->syms, e->name)) >= 0) {
            serr(p, e->line, e->col,
                 "'%s' is declared as both a sort and an enum", e->name);
            continue;
        }
        if (p->nsorts >= MAX_SORTS) {
            serr(p, e->line, e->col, "too many sorts (max %d)", MAX_SORTS);
            return;
        }
        int s = p->nsorts++;
        snprintf(p->sorts[s].name, MAX_NAME, "%s", e->name);
        p->sorts[s].line = e->line;
        p->sorts[s].col = e->col;
        p->sorts[s].is_domain = false;
        p->sorts[s].is_enum = true;
        for (int k = 0; k < e->nvalues; k++) {
            if (p->nents >= MAX_ENTS) {
                serr(p, e->line, e->col, "too many entities (max %d)", MAX_ENTS);
                return;
            }
            if (p->nents == p->capents) {
                p->capents = p->capents ? p->capents * 2 : 64;
                p->ents = realloc(p->ents, (size_t)p->capents * sizeof *p->ents);
            }
            p->ents[p->nents].atom = e->values[k];
            p->ents[p->nents].sort = s;            /* already resolved */
            p->ents[p->nents].line = e->line;
            p->ents[p->nents].col = e->col;
            p->nents++;
        }
    }
}

static void resolve_entities(parser *p)
{
    for (int i = 0; i < p->nents; i++) {
        int s = decode_sort(p, p->ents[i].sort, p->ents[i].line, p->ents[i].col,
                            "an entity declaration");
        p->ents[i].sort = s;                       /* may be -1 on error */
        if (i < p->nuserents && s >= 0 && p->sorts[s].is_enum)
            serr(p, p->ents[i].line, p->ents[i].col,
                 "cannot declare an entity of the enum '%s' — enum values are "
                 "not entities (#96); add the name to the enum instead",
                 p->sorts[s].name);
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
        for (int k = 0; k < f->nargs; k++) {
            pi->argsort[k] = decode_sort(p, -(int)f->argsort[k] - 2,
                                         f->line, f->col, "a fluent declaration");
            if (pi->argsort[k] >= 0 && p->sorts[pi->argsort[k]].is_domain)
                serr(p, f->line, f->col,
                     "fluent '%s' is keyed by the domain '%s'; domain-keyed "
                     "fluents (functional fluents / terrain over cells) are not "
                     "supported yet (#19). A domain is a provider/param arg type "
                     "only", intern_name(p->syms, f->pred), p->sorts[pi->argsort[k]].name);
        }
        pi->is_num = f->is_num;
        pi->is_cell = f->is_cell;
        pi->is_mv = f->is_mv;
        pi->val_sort = f->val_sort ? find_sort(p, f->val_sort) : -1;
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
        for (int k = 0; k < pr->nargs; k++) {
            if (ident_atom_is(p, pr->argsort[k], "int"))   /* a numeric arg (§5.6) */
                pi->argsort[k] = INT_SORT;
            else
                pi->argsort[k] = decode_sort(p, -(int)pr->argsort[k] - 2,
                                             pr->line, pr->col, "a provider declaration");
        }
    }

    /* engine-derived values (#82) register the pred with its arg sorts. Not a
     * fluent (never stored, no inertia, no effects) and not a head (its
     * definitions are the attackable literals, never the value itself). */
    for (int i = 0; i < p->nvaluedecls; i++) {
        ast_fluent *v = &p->valuedecls[i];
        pred_info *pi = find_pred(p, v->pred);
        if (pi && (pi->is_fluent || pi->is_provider || pi->is_value)) {
            serr(p, v->line, v->col, "'%s' is already declared",
                 intern_name(p->syms, v->pred));
            continue;
        }
        pi = intern_pred(p, v->pred, v->nargs);
        if (!pi) { serr(p, v->line, v->col, "too many predicates"); return; }
        pi->is_value = true;
        pi->arity = v->nargs;
        for (int k = 0; k < v->nargs; k++)
            pi->argsort[k] = decode_sort(p, -(int)v->argsort[k] - 2,
                                         v->line, v->col, "a value declaration");
    }

    /* rule heads register the conclusion predicates (arity from the head). */
    for (int i = 0; i < p->nrules; i++) {
        ast_atom *h = &p->rules[i].head;
        if (h->is_valuedef) continue;      /* a definition head is not a boolean conclusion */
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
    for (int i = 0; i < nvars; i++) {
        vars[i].sort = decode_sort(p, vars[i].sort, vars[i].line, vars[i].col, what);
        if (vars[i].sort >= 0 && p->sorts[vars[i].sort].is_domain)
            serr(p, vars[i].line, vars[i].col,
                 "cannot range a variable over the opaque domain '%s' (%s) — a "
                 "domain is never enumerated; use it only as a provider/param "
                 "argument type", p->sorts[vars[i].sort].name, what);
    }
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
    bool schema = pi && (pi->is_fluent || pi->is_provider);
    for (int k = 0; k < nargs; k++) {
        const ast_arg *arg = &args[k];
        int want = schema ? pi->argsort[k] : -1;
        if (arg->is_int) {                      /* a numeric literal — int slots only */
            if (schema && want != INT_SORT)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' is the integer %ld, but that position is "
                     "not declared `int`", k + 1, intern_name(p->syms, pred), arg->ival);
            continue;
        }
        int vi = var_index(vars, nvars, arg->name);
        int ei = find_entity(p, arg->name);
        if (vi < 0 && ei < 0) {
            serr(p, arg->line, arg->col,
                 "'%s' in %s is neither a bound variable nor a declared entity",
                 intern_name(p->syms, arg->name), ctx);
            continue;
        }
        if (schema) {                           /* sort-check against schema */
            int got = vi >= 0 ? vars[vi].sort : p->ents[ei].sort;
            if (want == INT_SORT)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' expects an integer but got '%s'",
                     k + 1, intern_name(p->syms, pred), intern_name(p->syms, arg->name));
            else if (want >= 0 && got >= 0 && want != got)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' expects sort '%s' but got '%s'",
                     k + 1, intern_name(p->syms, pred),
                     p->sorts[want].name, p->sorts[got].name);
        }
    }
}

static int expr_value_sort(parser *p, int e);      /* defined below check_atom */
static const char *value_sort_name(parser *p, int s);

/* A pred test(…) can reify (#86): a boolean fluent, a provider relation, or a
 * derived boolean head — anything whose literal has a verdict in the theory. */
static bool pi_is_boolean_testable(const pred_info *pi)
{
    if (pi->is_num || pi->is_mv || pi->is_cell || pi->is_value) return false;
    return pi->is_fluent || pi->is_provider || pi->is_head;
}

/* Does expression tree e contain a test(…), looking THROUGH derived-value
 * reads into their definitions (#82 inlines them, so a test inside a value
 * would otherwise smuggle itself into contexts where it is rejected)? */
static bool expr_has_test(parser *p, int e, int depth)
{
    if (e < 0 || depth > 2 * MAX_ARGS) return false;   /* cycles error elsewhere */
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_TEST: return true;
    case EX_CONST: case EX_ROLL: return false;
    case EX_LOAD: {
        int vi = find_value(p, n->pred);
        if (vi < 0) return false;
        for (int d = 0; d < p->nvdefs[vi]; d++) {
            ast_rule *dr = &p->rules[p->vdefs[vi][d]];
            if (dr->nbody > 0 || dr->has_guard)    /* layered: markers tested */
                return true;
            if (expr_has_test(p, dr->head.lhs_root, depth + 1)) return true;
        }
        return false;
    }
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_has_test(p, n->cargs[k], depth)) return true;
        return false;
    case EX_NEG: return expr_has_test(p, n->lhs, depth);
    default:     return expr_has_test(p, n->lhs, depth) ||
                        expr_has_test(p, n->rhs, depth);
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
        if (pi && pi->is_value) {              /* a derived-value read (#82) */
            int vi = find_value(p, n->pred);
            if (vi >= 0 && p->nvdefs[vi] == 0) {
                serr(p, n->line, n->col,
                     "value '%s' has no definition — write "
                     "`rule <label>: => %s(…) = expr`",
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
                            "a value read");
            return;
        }
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
    case EX_CALL: {
        int fi = find_function(p, n->pred);
        if (fi < 0) {                          /* unreachable: parse gated on this */
            serr(p, n->line, n->col, "'%s' is not a declared function",
                 intern_name(p->syms, n->pred));
            return;
        }
        ast_function *fn = &p->functions[fi];
        if (fn->nargs != n->nargs) {
            serr(p, n->line, n->col, "function '%s' takes %d argument%s but %d given",
                 intern_name(p->syms, n->pred), fn->nargs,
                 fn->nargs == 1 ? "" : "s", n->nargs);
            return;
        }
        for (int k = 0; k < n->nargs; k++) {   /* args are ordinary RHS expressions */
            check_expr(p, n->cargs[k], vars, nvars);
            /* type-check each argument against the declared parameter. An `int`
             * parameter (argsort == INTERN_NONE) wants an integer expression; a
             * sort/domain parameter wants a value of that type (a cell read of
             * that domain, or a nested call returning it). An unresolved declared
             * type is skipped — check_functions already flagged it. */
            int want = -1;
            if (fn->argsort[k] != INTERN_NONE) {
                want = find_sort(p, fn->argsort[k]);
                if (want < 0) continue;
            }
            int got = expr_value_sort(p, n->cargs[k]);
            if (got != want)
                serr(p, n->line, n->col,
                     "function '%s' argument %d expects %s but got %s",
                     intern_name(p->syms, n->pred), k + 1,
                     value_sort_name(p, want), value_sort_name(p, got));
        }
        return;
    }
    case EX_PRIOR:
        if (!p->in_valuedef_expr)
            serr(p, n->line, n->col,
                 "`prior` is only meaningful inside a value definition's "
                 "expression — it names the value the layers below would have "
                 "produced (#82)");
        return;
    case EX_TEST: {                            /* `test([~]p(args))` (#86) */
        note_ref(p, n->pred, n->line, n->col);
        pred_info *ti = find_pred(p, n->pred);
        if (ti && (pi_is_boolean_testable(ti))) {
            if (ti->arity != n->nargs) {
                serr(p, n->line, n->col, "'%s' takes %d argument%s but %d given",
                     intern_name(p->syms, n->pred), ti->arity,
                     ti->arity == 1 ? "" : "s", n->nargs);
                return;
            }
            check_pred_args(p, n->pred, ti, n->args, n->nargs, vars, nvars,
                            "a test(…)");
            return;
        }
        if (ti && (ti->is_num || ti->is_mv || ti->is_cell || ti->is_value))
            serr(p, n->line, n->col,
                 "test(…) takes a boolean literal — '%s' is %s; %s",
                 intern_name(p->syms, n->pred),
                 ti->is_num ? "numeric" : ti->is_value ? "a derived value"
                            : "not boolean",
                 ti->is_num || ti->is_value
                     ? "compare it (`… >= n`) instead of testing it" : "");
        /* an unregistered pred: leave it to the orphan analysis (note_ref) —
         * a typo warns; at commit an absent atom simply tests 0 */
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
            if (expr_has_test(p, roots[r], 0))
                serr(p, p->exprs[roots[r]].line, p->exprs[roots[r]].col,
                     "a clamp bound may not use test(…) yet — effects only "
                     "until the §5.8 stratifier (#87)");
        }
    }
}

/* Is expr `e` a valid RHS for a store-backed cell `:=` (§5.6)? Either a bare read
 * of another cell fluent (a move/copy — `at(X) := at(Y)`), or a call to a value-
 * returning function whose declared return type is the target cell's value domain
 * (`val_sort`) — `at(X) := neighbor(at(X), dir)`. Both hand the store an opaque
 * handle; no arithmetic on it. */
static bool expr_is_cell_rhs(parser *p, int e, int val_sort)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    if (n->kind == EX_LOAD) {
        pred_info *pi = find_pred(p, n->pred);
        return pi && pi->is_cell;
    }
    if (n->kind == EX_CALL) {
        int fi = find_function(p, n->pred);
        return fi >= 0 && find_sort(p, p->functions[fi].ret) == val_sort;
    }
    return false;
}

/* The value type of an effect expression, for function-argument type-checking:
 * a sort/domain index (a cell/domain-valued expr), or -1 for a plain integer. A
 * cell-fluent read carries its value domain; a function call carries its return
 * type; a numeric read, constant, roll, or any arithmetic node is an int. */
static int expr_value_sort(parser *p, int e)
{
    if (e < 0) return -1;
    ex_node *n = &p->exprs[e];
    if (n->kind == EX_CALL) {
        int fi = find_function(p, n->pred);
        return fi >= 0 ? find_sort(p, p->functions[fi].ret) : -1;
    }
    if (n->kind == EX_LOAD) {
        pred_info *pi = find_pred(p, n->pred);
        return (pi && pi->is_cell) ? pi->val_sort : -1;
    }
    return -1;                                  /* const / roll / arithmetic -> int */
}

/* Render a value type for a diagnostic: -1 is `int`, else the sort/domain name. */
static const char *value_sort_name(parser *p, int s)
{
    return s < 0 ? "int" : p->sorts[s].name;
}

/* A derived-value read in a rule position (#82). Rewrites the two comparison
 * forms into an ordinary expr guard over an EX_LOAD of the value — ONE
 * canonical shape downstream, so grounding, lanes, and the matcher never see a
 * value-specific atom — and rejects everything else: a value is derived, so it
 * is never written, never primed (that's #87 stratification), and never a
 * boolean atom (its definitions are the attackable literals, not the value). */
static void check_value_read(parser *p, ast_atom *at, bool in_effect)
{
    const char *nm = intern_name(p->syms, at->pred);
    if (at->primed) {
        serr(p, at->line, at->col,
             "a primed value read (`%s'`) needs the §5.8 stratification (#87) — "
             "not supported yet; test the current value instead", nm);
        return;
    }
    if (in_effect || at->is_num_effect) {
        serr(p, at->line, at->col,
             "'%s' is a derived value — it cannot be written or caused; change "
             "the state its definition reads, or write a definition rule "
             "(`rule <label>: => %s(…) = expr`)", nm, nm);
        return;
    }
    if (at->is_guard || at->is_valuedef) {          /* `v cmp n` / body `v = expr` */
        int l = alloc_expr(p, EX_LOAD, at->line, at->col);
        if (l < 0) return;
        p->exprs[l].pred = at->pred;
        p->exprs[l].nargs = at->nargs;
        for (int k = 0; k < at->nargs; k++) p->exprs[l].args[k] = at->args[k];
        int r;
        if (at->is_valuedef) {                      /* body position: equality guard */
            r = at->lhs_root;
            at->cmp = WORLD_CMP_EQ;
            at->is_valuedef = false;
        } else {
            r = alloc_expr(p, EX_CONST, at->line, at->col);
            if (r < 0) return;
            p->exprs[r].konst = at->threshold;
            at->is_guard = false;
        }
        at->is_expr_guard = true;
        at->lhs_root = l;
        at->rhs_root = r;
        return;
    }
    serr(p, at->line, at->col,
         "'%s' is a value — read it in a comparison (`%s(…) >= n`) or inside an "
         "expression, never as a boolean atom; to dispute it, defeat its "
         "definitions (#82)", nm, nm);
}

/* ---- #86 guard half: test(…) admissibility in expression guards --------
 *
 * A test-bearing guard evaluates in pass B of the two-phase solve, against
 * the settled pass-A result — so every tested atom must be fully decided by
 * pass A. Base fluents and providers are load-time inputs (always settled);
 * a DERIVED judgment is admissible iff its backward cone contains no
 * test-bearing guard (else its own verdict waits on pass B — circular).
 * Single-level nesting is exactly what the two-phase engine makes sound. */

static int pred_idx(parser *p, uint32_t pred);     /* defined below */

static bool rule_has_test_guard(parser *p, const ast_rule *r)
{
    for (int b = 0; b < r->nbody; b++)
        if (r->body[b].is_expr_guard &&
            (expr_has_test(p, r->body[b].lhs_root, 0) ||
             expr_has_test(p, r->body[b].rhs_root, 0))) return true;
    for (int g = 0; g < r->nguard; g++)
        if (r->guard[g].is_expr_guard &&
            (expr_has_test(p, r->guard[g].lhs_root, 0) ||
             expr_has_test(p, r->guard[g].rhs_root, 0))) return true;
    return false;
}

static void check_tested_cone_pred(parser *p, uint32_t pred, int line, int col)
{
    pred_info *pi = find_pred(p, pred);
    if (!pi || pi->is_fluent || pi->is_provider || !pi->is_head)
        return;             /* load-time input, or unknown (orphan warns) */
    bool seen[MAX_PREDS] = { false };
    uint32_t queue[MAX_PREDS];
    int qh = 0, qt = 0, ti = pred_idx(p, pred);
    if (ti >= 0) seen[ti] = true;
    queue[qt++] = pred;
    while (qh < qt) {
        uint32_t q = queue[qh++];
        for (int i = 0; i < p->nrules; i++) {
            ast_rule *r = &p->rules[i];
            if (r->head.is_valuedef || r->head.pred != q) continue;
            if (rule_has_test_guard(p, r)) {
                serr(p, line, col,
                     "test(%s…) in a guard: '%s' is itself derived through a "
                     "test(…) guard (rule '%s'), so it does not settle below "
                     "this guard — nested test guards are a later stratum "
                     "(§5.8); derive one side from state instead",
                     intern_name(p->syms, pred),
                     intern_name(p->syms, pred), r->label);
                return;
            }
            for (int pass = 0; pass < 2; pass++) {
                ast_atom *ats = pass ? r->guard : r->body;
                int nat = pass ? r->nguard : r->nbody;
                for (int b = 0; b < nat; b++) {
                    if (ats[b].is_expr_guard || ats[b].is_member) continue;
                    int bi = pred_idx(p, ats[b].pred);
                    if (bi >= 0 && p->preds[bi].is_head && !seen[bi] &&
                        qt < MAX_PREDS) {
                        seen[bi] = true;
                        queue[qt++] = ats[b].pred;
                    }
                }
            }
        }
    }
}

static void check_guard_test_nodes(parser *p, int e, int depth)
{
    if (e < 0 || depth > 2 * MAX_ARGS) return;   /* cycles error in their own pass */
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_TEST:  check_tested_cone_pred(p, n->pred, n->line, n->col); return;
    case EX_CONST: case EX_ROLL: return;
    case EX_LOAD: {
        /* a LAYERED value read in a guard tests its markers (#82/#94): every
         * definition body must settle in pass A — its preds' cones test-free
         * (test-guards inside definition bodies are rejected upstream) */
        int vj = find_value(p, n->pred);
        if (vj < 0) return;
        for (int d = 0; d < p->nvdefs[vj]; d++) {
            ast_rule *dr = &p->rules[p->vdefs[vj][d]];
            for (int pass2 = 0; pass2 < 2; pass2++) {
                ast_atom *ats = pass2 ? dr->guard : dr->body;
                int nat = pass2 ? dr->nguard : dr->nbody;
                for (int b = 0; b < nat; b++)
                    if (!ats[b].is_expr_guard && !ats[b].is_member)
                        check_tested_cone_pred(p, ats[b].pred,
                                               ats[b].line, ats[b].col);
            }
            check_guard_test_nodes(p, dr->head.lhs_root, depth + 1);
        }
        return;
    }
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            check_guard_test_nodes(p, n->cargs[k], depth);
        return;
    case EX_NEG:   check_guard_test_nodes(p, n->lhs, depth); return;
    default:
        check_guard_test_nodes(p, n->lhs, depth);
        check_guard_test_nodes(p, n->rhs, depth);
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
    if (at->is_expr_guard) {                        /* `expr <op> expr` guard */
        if (in_effect) {
            serr(p, at->line, at->col,
                 "a comparison guard can't appear in a `causes` clause");
            return;
        }
        /* test(…) in a guard is admissible when every tested atom settles in
         * pass A of the two-phase solve (#86 guard half — advantage's home);
         * a tested judgment derived through another test-guard is rejected */
        check_guard_test_nodes(p, at->lhs_root, 0);
        check_guard_test_nodes(p, at->rhs_root, 0);
        check_expr(p, at->lhs_root, vars, nvars);
        check_expr(p, at->rhs_root, vars, nvars);
        return;
    }
    if (at->is_member) {                            /* `X in { … }` (#95) */
        if (in_effect) {
            serr(p, at->line, at->col,
                 "a membership test can't appear in a `causes` clause");
            return;
        }
        int vi = var_index(vars, nvars, at->args[0].name);
        if (vi < 0) {
            serr(p, at->args[0].line, at->args[0].col,
                 "'%s' in a membership test must be a bound variable",
                 intern_name(p->syms, at->args[0].name));
            return;
        }
        int vs = vars[vi].sort;
        for (int k = 0; k < at->mem_n; k++) {
            uint32_t val = p->mempool[at->mem_ix + k];
            int ei = find_entity(p, val);
            int got = ei >= 0 ? p->ents[ei].sort : -1;
            if (ei < 0 || (vs >= 0 && got >= 0 && got != vs))
                serr(p, at->line, at->col,
                     "'%s' is not a value of '%s' (the sort of '%s') — this "
                     "membership test could never match it",
                     intern_name(p->syms, val),
                     vs >= 0 ? p->sorts[vs].name : "?",
                     intern_name(p->syms, at->args[0].name));
        }
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
        /* A primed NUMERIC guard (`hp(X)' <= 0` — the dying trigger) is the
         * §5.8 stratification case, now supported (#87): the compiler assigns
         * strata (stratify_steps below) and the engine solves once per
         * stratum, minting the guard's fact when the fluent's next value
         * settles. Primed JUDGMENTS remain a later slice. */
        if (at->is_guard) {
            pred_info *pg = find_pred(p, at->pred);
            if (!pg || !pg->is_fluent || !pg->is_num) {
                serr(p, at->line, at->col,
                     "'%s' is compared primed but is not a declared numeric "
                     "fluent (`%s : int`)",
                     intern_name(p->syms, at->pred), intern_name(p->syms, at->pred));
                return;
            }
            check_pred_args(p, at->pred, pg, at->args, at->nargs, vars, nvars, ctx);
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
    if (pi && pi->is_value) {                       /* a derived value (#82) */
        check_value_read(p, at, in_effect);
        if (!at->is_expr_guard) return;             /* misuse already reported */
        check_expr(p, at->lhs_root, vars, nvars);
        check_expr(p, at->rhs_root, vars, nvars);
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
        if (pi->is_cell) {
            /* a store-backed cell fluent takes only `:=` from another cell fluent
             * (a move/copy) — no arithmetic on an opaque handle (§5.6) */
            if (at->numop != WORLD_OP_ASSIGN)
                serr(p, at->line, at->col,
                     "'%s' is a store-backed cell fluent — it takes `:=` (a copy), "
                     "not `+=`/`-=`; there is no arithmetic on an opaque cell handle",
                     intern_name(p->syms, at->pred));
            else if (!expr_is_cell_rhs(p, at->expr_root, pi->val_sort))
                serr(p, at->line, at->col,
                     "'%s :=' must copy another cell fluent (e.g. `at(Y)`) or call a "
                     "function returning that cell type (e.g. `neighbor(at(X), dir)`), "
                     "not an arithmetic or literal expression",
                     intern_name(p->syms, at->pred));
        }
        check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars, ctx);
        check_expr(p, at->expr_root, vars, nvars);
        if (at->as_value != INTERN_NONE) {          /* typed contribution (#83) */
            if (at->numop == WORLD_OP_ASSIGN) {
                serr(p, at->line, at->col,
                     "`as` types a contribution (`+=`/`-=`), not a `:=` "
                     "assignment — an assigned value has no damage type");
                return;
            }
            int ei = find_entity(p, at->as_value);
            int es = ei >= 0 ? p->ents[ei].sort : -1;
            if (es < 0 || !p->sorts[es].is_enum) {
                serr(p, at->line, at->col,
                     "'%s' after `as` is not a declared enum value — damage "
                     "types are a closed `enum` (#83)",
                     intern_name(p->syms, at->as_value));
                return;
            }
            if (p->dtype_sort >= 0 && p->dtype_sort != es) {
                serr(p, at->line, at->col,
                     "typed contributions must draw from ONE enum per world — "
                     "'%s' is from '%s' but damage types were already '%s'",
                     intern_name(p->syms, at->as_value), p->sorts[es].name,
                     p->sorts[p->dtype_sort].name);
                return;
            }
            p->dtype_sort = es;
            /* the response is per SUBJECT (resistant(subject, type)): the
             * target fluent's first argument must be an entity */
            if (pi->arity < 1 || pi->argsort[0] < 0 ||
                p->sorts[pi->argsort[0]].is_enum) {
                serr(p, at->line, at->col,
                     "a typed contribution needs a subject — '%s' must be "
                     "keyed by an entity first argument (e.g. `hp(actor)`) so "
                     "`resistant(<subject>, %s)` has someone to be about",
                     intern_name(p->syms, at->pred),
                     intern_name(p->syms, at->as_value));
                return;
            }
        }
        return;
    }
    if (pi && pi->is_cell) {   /* read side: cells are read through providers only */
        serr(p, at->line, at->col,
             "a store-backed cell fluent '%s' can't be read in a rule guard; "
             "read positions through a provider (e.g. same_cell(X,Y), adjacent, "
             "near) — §5.6", intern_name(p->syms, at->pred));
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
        if (!in_domain) {
            /* a `: cell` functional fluent admits a JOIN: `at(X) = c` where `c`
             * is a bound variable over the value sort (§5.6) — grounds to the
             * actor's cell per instance. */
            int vi = var_index(vars, nvars, at->value);
            if (pi->val_sort >= 0 && vi >= 0 && vars[vi].sort == pi->val_sort)
                in_domain = true;
        }
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
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_reads_roll(p, n->cargs[k])) return true;
        return false;
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
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_uses_var(p, n->cargs[k], name)) return true;
        return false;
    case EX_NEG: return expr_uses_var(p, n->lhs, name);
    default:     return expr_uses_var(p, n->lhs, name) || expr_uses_var(p, n->rhs, name);
    }
}

/* Does variable `v` occur in `at` at all (any arg position, or the value slot
 * of a `f(X) = v` value-join, or inside an expr guard)? */
static bool atom_uses_var(parser *p, const ast_atom *at, uint32_t v)
{
    if (at->is_expr_guard)
        return expr_uses_var(p, at->lhs_root, v) || expr_uses_var(p, at->rhs_root, v);
    for (int k = 0; k < at->nargs; k++)
        if (at->args[k].name == v) return true;
    return at->value == v;                        /* value-join var (§5.6) */
}

/* Is arg `a` already bound — an entity constant / int literal, or a var the
 * fixpoint has marked bound? (Constants are always bound; a var is bound once
 * some generator produces it.) */
static bool arg_is_bound(ast_rule *r, ast_arg a, const bool *vbound)
{
    if (a.is_int) return true;
    int vi = var_index(r->vars, r->nvars, a.name);
    return vi < 0 || vbound[vi];                  /* vi < 0 => an entity constant */
}

/* Range restriction with sideways information passing (§5.2 item 1). A variable
 * is *bindable* if some positive generator produces it under a feasible
 * evaluation order — computed as a fixpoint into `vbound[nvars]`:
 *   - a positive fluent/derived atom, and a numeric guard/expr (`hp(X) <= 0` —
 *     the value store has a row per entity), generate their arg vars outright;
 *   - a PROVIDER generates its args only once at least one of its own args is
 *     already bound (a constant anchor like `wiz`, or a var another generator
 *     produced): the "who is in range of X" mode (§5.2 disc. 3). A provider with
 *     nothing bound cannot enumerate and binds nothing;
 *   - negation binds nothing (a filter).
 * The join planner (#28) consumes the same bound-set as its binding order. */
static void compute_bound_vars(parser *p, ast_rule *r, bool *vbound)
{
    for (int i = 0; i < r->nvars; i++) vbound[i] = false;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = 0; b < r->nbody; b++) {
            ast_atom *at = &r->body[b];
            if (at->neg) continue;                /* filter */

            if (at->is_expr_guard) {              /* reads the value store */
                for (int i = 0; i < r->nvars; i++)
                    if (!vbound[i] && atom_uses_var(p, at, r->vars[i].name))
                        { vbound[i] = true; changed = true; }
                continue;
            }

            pred_info *pi = find_pred(p, at->pred);
            bool provider = pi && pi->is_provider && !at->is_guard;
            if (provider) {                       /* needs an anchor to enumerate */
                bool anchored = false;
                for (int k = 0; k < at->nargs; k++)
                    if (arg_is_bound(r, at->args[k], vbound)) { anchored = true; break; }
                if (!anchored) continue;
            }
            /* fluent / derived / numeric-guard, or an anchored provider: binds args */
            for (int k = 0; k < at->nargs; k++) {
                int vi = var_index(r->vars, r->nvars, at->args[k].name);
                if (vi >= 0 && !vbound[vi]) { vbound[vi] = true; changed = true; }
            }
            if (at->value != INTERN_NONE) {
                int vi = var_index(r->vars, r->nvars, at->value);
                if (vi >= 0 && !vbound[vi]) { vbound[vi] = true; changed = true; }
            }
        }
    }
}

/* Every rule variable must be range-restricted (bindable by compute_bound_vars).
 * An unbindable var grounds over its whole declared sort — the nᵏ blow-up
 * bench_ground measures — and the tick-time matcher (#28) has no extension to
 * enumerate it from. Typed sorts (item 2) still make the grounding finite, so
 * today this is a WARNING, not a failure; once #28's router exists a
 * matcher-routed rule upgrades it to an error. The diagnostic distinguishes
 * "never occurs" from "only in filters / an unanchored provider" so the fix
 * (add a positive anchor) is obvious. */
static void check_safety(parser *p, ast_rule *r)
{
    bool vbound[MAX_ARGS];
    compute_bound_vars(p, r, vbound);

    for (int i = 0; i < r->nvars; i++) {
        if (vbound[i]) continue;                  /* range-restricted */

        uint32_t v = r->vars[i].name;
        bool occurs = false;
        for (int b = 0; b < r->nbody && !occurs; b++)
            occurs = atom_uses_var(p, &r->body[b], v);
        const char *sort = r->vars[i].sort >= 0 ? p->sorts[r->vars[i].sort].name : "?";

        if (occurs)
            warn(p, r->vars[i].line, r->vars[i].col,
                 "variable '%s' of rule '%s' has no positive generator — it "
                 "appears only in negation or an unanchored provider, so it "
                 "grounds over the whole '%s' sort and a tick-time matcher "
                 "cannot enumerate it (§5.2 item 1)",
                 intern_name(p->syms, v), r->label, sort);
        else
            warn(p, r->vars[i].line, r->vars[i].col,
                 "variable '%s' of rule '%s' does not occur in the body — "
                 "it grounds over the whole '%s' sort",
                 intern_name(p->syms, v), r->label, sort);
    }
}

static long instance_count(parser *p, var_bind *vars, int nvars, bool *overflow);

/* Tiny union-find over a rule's variable indices (nvars <= MAX_ARGS). */
static int uf_find(int *uf, int x) { while (uf[x] != x) x = uf[x] = uf[uf[x]]; return x; }
static void uf_union(int *uf, int a, int b) { uf[uf_find(uf, a)] = uf_find(uf, b); }

/* Cardinality guard (§5.2 item 5 / §5.6): a rule whose variables split into two
 * or more groups that no positive body atom joins ranges over their *cross
 * product* — a multiplicative blow-up with no sparse relation linking the groups.
 * When that product is large it is a compile-time WARNING carrying the estimated
 * count, so an author anchors it (a provider like `near(X,Y)`, or a joining
 * fluent) rather than paying nᵏ silently. This is the gap check_safety leaves:
 * there every var is *unbound*; here every var IS bound (the rule is safe) but
 * the groups are independent. A single joined group (one component) is a genuine
 * relation the tick-time matcher / provider prunes, so it never warns. */
static void check_cardinality(parser *p, ast_rule *r)
{
    if (r->nvars < 2) return;                      /* no cross product to blow up */

    bool of = false;
    long total = instance_count(p, r->vars, r->nvars, &of);
    if (of || total <= CARD_WARN) return;          /* overflow is reported at grounding */

    /* only flag a *safe* rule — an unbound var is check_safety's to report. */
    bool vbound[MAX_ARGS];
    compute_bound_vars(p, r, vbound);
    for (int i = 0; i < r->nvars; i++) if (!vbound[i]) return;

    /* union the variables any positive (non-negated) body atom relates: vars
     * sharing such an atom are one group; independent groups multiply. */
    int uf[MAX_ARGS];
    for (int i = 0; i < r->nvars; i++) uf[i] = i;
    for (int b = 0; b < r->nbody; b++) {
        ast_atom *at = &r->body[b];
        if (at->neg) continue;                     /* a filter, not a join */
        int first = -1;
        for (int i = 0; i < r->nvars; i++) {
            if (!atom_uses_var(p, at, r->vars[i].name)) continue;
            if (first < 0) first = i; else uf_union(uf, first, i);
        }
    }
    int comps = 0;
    for (int i = 0; i < r->nvars; i++) if (uf_find(uf, i) == i) comps++;
    if (comps < 2) return;                          /* one joined group: anchored */

    warn(p, r->line, r->col,
         "rule '%s' grounds to %ld instances over an un-anchored cross product "
         "(%d independent variable groups) — join the variables with a sparse "
         "anchor (a provider like `near(X, Y)`) or split the sorts "
         "(§5.2 cardinality warning)", r->label, total, comps);
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

/* A functional fluent `at(X) : cell` (§5.6) is valued in a sort's entities. The
 * entities are known only after resolve_entities, so fill the fluent's MV value
 * domain from them here. Logic-backed over §5.7 (one value-atom per cell); a
 * large domain exceeds MAX_DOMAIN and needs the store-backed representation
 * (not yet, #19) — a located error, never a silent overflow. */
static void populate_sort_valued_fluents(parser *p)
{
    for (int i = 0; i < p->nfluents; i++) {
        ast_fluent *f = &p->fluents[i];
        if (!f->val_sort || f->is_cell) continue;   /* is_cell: store-backed, not MV */
        int si = find_sort(p, f->val_sort);
        if (si < 0) continue;                      /* checked at parse */
        int n = p->domain_n[si];
        if (n == 0) {
            serr(p, f->line, f->col,
                 "functional fluent '%s' is valued in sort '%s', which has no "
                 "entities", intern_name(p->syms, f->pred), p->sorts[si].name);
            continue;
        }
        if (n > MAX_DOMAIN) {
            serr(p, f->line, f->col,
                 "functional fluent '%s' ranges over %d entities of sort '%s' "
                 "(max %d, logic-backed); the store-backed representation for "
                 "large cell domains is not implemented yet (#19)",
                 intern_name(p->syms, f->pred), n, p->sorts[si].name, MAX_DOMAIN);
            continue;
        }
        f->nvalues = n;
        for (int v = 0; v < n; v++) f->values[v] = p->domain_ents[si][v];
    }
}

/* Validate value-returning function declarations (§5.6): every arg/return type is
 * a declared sort/domain (or `int`); the name doesn't clash with a fluent,
 * provider, or another function. Types are checked once here, not per call. */
static void check_functions(parser *p)
{
    for (int i = 0; i < p->nfunctions; i++) {
        ast_function *fn = &p->functions[i];
        for (int j = 0; j < i; j++)
            if (p->functions[j].name == fn->name)
                serr(p, fn->line, fn->col, "function '%s' is declared more than once",
                     intern_name(p->syms, fn->name));
        pred_info *pi = find_pred(p, fn->name);
        if (pi && (pi->is_fluent || pi->is_provider))
            serr(p, fn->line, fn->col,
                 "function '%s' clashes with a fluent or provider of the same name",
                 intern_name(p->syms, fn->name));
        for (int k = 0; k < fn->nargs; k++)
            if (fn->argsort[k] != INTERN_NONE && find_sort(p, fn->argsort[k]) < 0)
                serr(p, fn->line, fn->col,
                     "function '%s' argument %d has unknown type '%s' (expected a "
                     "sort, domain, or `int`)", intern_name(p->syms, fn->name), k + 1,
                     intern_name(p->syms, fn->argsort[k]));
        if (fn->ret != INTERN_NONE && find_sort(p, fn->ret) < 0)
            serr(p, fn->line, fn->col,
                 "function '%s' has unknown return type '%s' (expected a sort, "
                 "domain, or `int`)", intern_name(p->syms, fn->name),
                 intern_name(p->syms, fn->ret));
    }
}

/* Register a value definition's rule index (#82) — a pre-pass, so a read
 * checked anywhere in the rules loop can already see whether definitions
 * exist regardless of declaration order. All defs collect here; the base is
 * chosen and the chain ordered in order_value_layers below. */
static void register_valuedef(parser *p, int ri)
{
    ast_rule *r = &p->rules[ri];
    int vi = find_value(p, r->head.pred);          /* parse gated on this */
    if (vi < 0) return;
    if (p->nvdefs[vi] >= MAX_LAYERS + 1) {
        serr(p, r->line, r->col,
             "'%s' has too many definitions (max %d — one base + %d layers)",
             intern_name(p->syms, r->head.pred), MAX_LAYERS + 1, MAX_LAYERS);
        return;
    }
    p->vdefs[vi][p->nvdefs[vi]++] = ri;
}

/* Does expr subtree e mention `prior` anywhere? */
static bool expr_has_prior(parser *p, int e)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_PRIOR: return true;
    case EX_CONST: case EX_ROLL: case EX_LOAD: return false;
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_has_prior(p, n->cargs[k])) return true;
        return false;
    case EX_NEG: return expr_has_prior(p, n->lhs);
    default:     return expr_has_prior(p, n->lhs) || expr_has_prior(p, n->rhs);
    }
}

/* Classify a definition's combination class (#94): the commutation check is
 * BY CLASS — `prior + e` layers commute with each other (addition), and so do
 * `max(prior, e)` / `min(prior, e)` layers among themselves; an override (no
 * `prior`) or a general prior use (e.g. halve/double — the §5.8 witness for
 * why floored scalings do NOT commute) must be totally ordered by `>`. */
static int valuedef_class(parser *p, int e)
{
    if (!expr_has_prior(p, e)) return 0;           /* override */
    ex_node *n = &p->exprs[e];
    int a = n->lhs, b = n->rhs;
    bool la = a >= 0 && p->exprs[a].kind == EX_PRIOR;
    bool lb = b >= 0 && p->exprs[b].kind == EX_PRIOR;
    if ((la && !lb && b >= 0 && !expr_has_prior(p, b)) ||
        (lb && !la && a >= 0 && !expr_has_prior(p, a))) {
        if (n->kind == EX_ADD) return 1;
        if (n->kind == EX_MAX) return 2;
        if (n->kind == EX_MIN) return 3;
    }
    return 4;                                      /* general: order it explicitly */
}

/* Validate a value definition `rule L(vars): [body] => v(args) = expr`
 * (#82/#94). A definition is never a dl rule — its BODY grounds to a marker
 * judgment (`body => label(binding)`) on first read, and its expression is
 * inlined into the chain program at every read site; the whole rules pipeline
 * skips the definition itself. */
static void check_valuedef(parser *p, int ri)
{
    ast_rule *r = &p->rules[ri];
    ast_atom *h = &r->head;
    const char *nm = intern_name(p->syms, h->pred);
    resolve_vars(p, r->vars, r->nvars, "a value definition");
    if (h->neg) {
        serr(p, h->line, h->col,
             "a value definition cannot be negated — to dispute '%s', defeat "
             "its definitions (#82)", nm);
        return;
    }
    if (r->kind != DL_DEFEASIBLE) {
        serr(p, h->line, h->col,
             "a value definition is defeasible — write '=>' (defeat between "
             "definitions is what will make '%s' layerable, #82)", nm);
        return;
    }
    /* guarded definitions (#82 layering): the body is an ordinary conjunct,
     * checked like a rule body; it must not itself contain a test-bearing
     * guard (the marker has to settle in pass A of the two-phase solve) */
    for (int b = 0; b < r->nbody; b++)
        check_atom(p, &r->body[b], r->vars, r->nvars, true, false, false,
                   "a value-definition body");
    for (int g = 0; g < r->nguard; g++)
        check_atom(p, &r->guard[g], r->vars, r->nvars, true, false, false,
                   "a value-definition `unless` guard");
    if (rule_has_test_guard(p, r)) {
        serr(p, r->line, r->col,
             "a test(…) guard in a value definition's body — the definition's "
             "marker must settle in the first solve pass (§5.8); derive the "
             "condition from state instead");
        return;
    }
    int vi = find_value(p, h->pred);
    ast_fluent *v = &p->valuedecls[vi];
    pred_info *pi = find_pred(p, h->pred);
    if (h->nargs != v->nargs || r->nvars != v->nargs) {
        serr(p, h->line, h->col,
             "the definition of '%s' must bind exactly its %d declared "
             "argument%s (each head argument a distinct rule parameter)",
             nm, v->nargs, v->nargs == 1 ? "" : "s");
        return;
    }
    bool used[MAX_ARGS] = { false };
    for (int k = 0; k < h->nargs; k++) {
        int f = var_index(r->vars, r->nvars, h->args[k].name);
        if (f < 0 || used[f]) {
            serr(p, h->args[k].line, h->args[k].col,
                 "value-definition argument %d of '%s' must be a distinct rule "
                 "parameter, got '%s'", k + 1, nm,
                 intern_name(p->syms, h->args[k].name));
            return;
        }
        used[f] = true;
        if (pi && pi->argsort[k] >= 0 && r->vars[f].sort >= 0 &&
            pi->argsort[k] != r->vars[f].sort)
            serr(p, h->args[k].line, h->args[k].col,
                 "'%s' argument %d is declared '%s' but parameter '%s' ranges "
                 "over '%s'", nm, k + 1, p->sorts[pi->argsort[k]].name,
                 intern_name(p->syms, h->args[k].name),
                 p->sorts[r->vars[f].sort].name);
    }
    if (expr_has_test(p, h->lhs_root, 0)) {
        serr(p, h->line, h->col,
             "test(…) in a value definition could flow into a guard through a "
             "value read — that needs the §5.8 stratifier (#87); use test(…) "
             "directly in the effect instead");
        return;
    }
    p->in_valuedef_expr = true;                    /* `prior` legal here */
    check_expr(p, h->lhs_root, r->vars, r->nvars);
    p->in_valuedef_expr = false;
    r->vclass = valuedef_class(p, h->lhs_root);
}

/* #82: a value definition must not read itself, directly or through other
 * values — the read is an inline expansion, so a cycle is infinite regress. */
static bool value_cycle_dfs(parser *p, int vi, bool *onstack, bool *done);

static bool expr_value_cycle(parser *p, int e, bool *onstack, bool *done)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_ROLL: return false;
    case EX_LOAD: {
        int vj = find_value(p, n->pred);
        return vj >= 0 && value_cycle_dfs(p, vj, onstack, done);
    }
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_value_cycle(p, n->cargs[k], onstack, done)) return true;
        return false;
    case EX_NEG: return expr_value_cycle(p, n->lhs, onstack, done);
    default:     return expr_value_cycle(p, n->lhs, onstack, done) ||
                        expr_value_cycle(p, n->rhs, onstack, done);
    }
}

static bool value_cycle_dfs(parser *p, int vi, bool *onstack, bool *done)
{
    if (done[vi]) return false;
    if (onstack[vi]) return true;
    onstack[vi] = true;
    bool cyc = false;
    for (int d = 0; d < p->nvdefs[vi] && !cyc; d++)   /* every definition's expr */
        cyc = expr_value_cycle(p, p->rules[p->vdefs[vi][d]].head.lhs_root,
                               onstack, done);
    onstack[vi] = false;
    done[vi] = true;
    return cyc;
}

/* ---- #82/#94: choose the base, order the chain, check well-formedness ----
 *
 * Per value: exactly ONE unconditional definition (the base — #94's "a base
 * must exist", made static; it keeps every value total, so reads stay legal
 * in guards and effects exactly as in the single-definition slice). Guarded
 * definitions form the chain above it, ordered by the EXISTING superiority
 * relation over rule labels: `a > b` places a ABOVE b. The order need only be
 * partial: unordered pairs are legal iff both are layers of the SAME class
 * (add/add, max/max, min/min — commutative by construction, #94); an override
 * or a general prior use must be comparable to every other guarded def, since
 * its result depends on position. Ties fall back to declaration order, which
 * by then is semantically irrelevant. */
static void order_value_layers(parser *p)
{
    for (int vi = 0; vi < p->nvaluedecls; vi++) {
        int nds = p->nvdefs[vi];
        if (nds == 0) continue;
        const char *vn = intern_name(p->syms, p->valuedecls[vi].pred);

        /* the base: exactly one unconditional, prior-free definition */
        int base = -1;
        int gl[MAX_LAYERS], ngl = 0;               /* guarded defs, decl order */
        for (int d = 0; d < nds; d++) {
            int ri = p->vdefs[vi][d];
            ast_rule *r = &p->rules[ri];
            if (r->nbody == 0 && !r->has_guard) {
                if (base >= 0) {
                    serr(p, r->line, r->col,
                         "'%s' has two unconditional definitions ('%s' and "
                         "'%s') — exactly one is the base; give the other a "
                         "body (#94)", vn, p->rules[base].label, r->label);
                    return;
                }
                base = ri;
                if (expr_has_prior(p, r->head.lhs_root)) {
                    serr(p, r->line, r->col,
                         "`prior` in the base definition of '%s' has nothing "
                         "beneath it (#94)", vn);
                    return;
                }
            } else {
                gl[ngl++] = ri;                    /* bounded by register cap */
            }
        }
        if (base < 0) {
            serr(p, p->valuedecls[vi].line, p->valuedecls[vi].col,
                 "'%s' needs an unconditional base definition — every guarded "
                 "definition layers on or overrides the value the base "
                 "provides (#94: a base must exist)", vn);
            return;
        }
        p->value_def[vi] = base;

        /* superiority among the guarded defs: above[i][j] = i beats j */
        bool above[MAX_LAYERS][MAX_LAYERS] = { { false } };
        for (int e = 0; e < p->nsups; e++) {
            int wi = -1, li = -1;
            for (int i = 0; i < ngl; i++) {
                if (strcmp(p->sups[e].a, p->rules[gl[i]].label) == 0) wi = i;
                if (strcmp(p->sups[e].b, p->rules[gl[i]].label) == 0) li = i;
            }
            if (wi >= 0 && li >= 0) above[wi][li] = true;
        }
        for (int k = 0; k < ngl; k++)              /* transitive closure */
            for (int i = 0; i < ngl; i++)
                for (int j = 0; j < ngl; j++)
                    if (above[i][k] && above[k][j]) above[i][j] = true;

        /* well-formedness: comparability where order matters (#94) */
        for (int i = 0; i < ngl; i++)
            for (int j = i + 1; j < ngl; j++) {
                if (above[i][j] || above[j][i]) continue;
                int ci = p->rules[gl[i]].vclass, cj = p->rules[gl[j]].vclass;
                bool needs_order = ci == 0 || cj == 0 || ci == 4 || cj == 4 ||
                                   ci != cj;
                if (needs_order)
                    serr(p, p->rules[gl[j]].line, p->rules[gl[j]].col,
                         "definitions '%s' and '%s' of '%s' can both apply but "
                         "are not ordered, and their combination does not "
                         "commute — add a superiority edge ('%s > %s' or the "
                         "reverse) (#94)",
                         p->rules[gl[i]].label, p->rules[gl[j]].label, vn,
                         p->rules[gl[i]].label, p->rules[gl[j]].label);
            }
        if (above[0][0]) { /* unreachable; keeps -Wunused honest */ }

        /* chain order, bottom -> top: repeatedly take the declaration-earliest
         * def that beats no unplaced def (its losers are all placed) */
        bool placed[MAX_LAYERS] = { false };
        int nplaced = 0;
        while (nplaced < ngl) {
            int pick = -1;
            for (int i = 0; i < ngl && pick < 0; i++) {
                if (placed[i]) continue;
                bool ok = true;
                for (int j = 0; j < ngl && ok; j++)
                    if (!placed[j] && above[i][j]) ok = false;
                if (ok) pick = i;
            }
            if (pick < 0) {
                serr(p, p->rules[gl[0]].line, p->rules[gl[0]].col,
                     "superiority among the definitions of '%s' is cyclic", vn);
                return;
            }
            placed[pick] = true;
            p->value_layers[vi][nplaced++] = gl[pick];
        }
        p->value_nlayers[vi] = ngl;
    }
}

/* ---- §5.8 stratification (#87) -------------------------------------
 *
 * A primed numeric guard reads a fluent's NEXT value, so everything that can
 * write that fluent must settle in a lower stratum than any rule reading the
 * guard. The analysis is PRED-level (coarser than ground = conservative):
 *
 *   stratum(rule R) >= fstrat(N) + 1   for each primed numeric guard over N
 *                                       in R's body, where fstrat(N) = max
 *                                       stratum over writers of N;
 *   stratum(rule R) >= stratum(R')     for each primed BOOLEAN/MV read b' in
 *                                       R's body and each R' with an effect on
 *                                       b — the boolean fixpoint evaluates b'
 *                                       within a solve, so equality suffices,
 *                                       but the primed-numeric dependencies of
 *                                       b's writers must propagate through.
 *
 * Iterated to fixpoint. Acyclic programs stabilise with strata bounded by the
 * number of primed-guarded fluents; a program still moving past that bound
 * has a primed-numeric cycle — which genuinely oscillates ("heal if hp' < 5,
 * curse if hp' >= 5") — and is rejected with a located error naming the
 * fluent. Zero primed guards leaves every rule at stratum 0: the degenerate
 * case, today's single-solve tick. */

static bool action_writes_num(parser *p, const ast_action *a, uint32_t pred)
{
    for (int b = 0; b < a->neff; b++)
        if (a->effects[b].is_num_effect && a->effects[b].pred == pred) return true;
    for (int bi = 0; bi < a->nbind; bi++) {
        const ast_binder *bnd = &p->binders[a->bind_ix[bi]];
        for (int it = 0; it < bnd->nitems; it++)
            if (bnd->items[it].eff.is_num_effect && bnd->items[it].eff.pred == pred)
                return true;
    }
    return false;
}

static bool action_writes_bool(parser *p, const ast_action *a, uint32_t pred)
{
    for (int b = 0; b < a->neff; b++)
        if (!a->effects[b].is_num_effect && a->effects[b].pred == pred) return true;
    for (int bi = 0; bi < a->nbind; bi++) {
        const ast_binder *bnd = &p->binders[a->bind_ix[bi]];
        for (int it = 0; it < bnd->nitems; it++)
            if (!bnd->items[it].eff.is_num_effect && bnd->items[it].eff.pred == pred)
                return true;
    }
    return false;
}

static void stratify_steps(parser *p)
{
    /* count the distinct primed-guarded numeric preds; none -> nothing to do */
    uint32_t pg[MAX_PREDS];
    int npgf = 0;
    for (int i = 0; i < p->nactions; i++)
        for (int b = 0; b < p->actions[i].nreq; b++) {
            ast_atom *at = &p->actions[i].requires[b];
            if (!(at->primed && at->is_guard)) continue;
            bool seen = false;
            for (int k = 0; k < npgf && !seen; k++) seen = pg[k] == at->pred;
            if (!seen && npgf < MAX_PREDS) pg[npgf++] = at->pred;
        }
    if (npgf == 0) return;
    p->has_pguards = true;

    int rounds = npgf + 2;                 /* acyclic depth is bounded by npgf */
    bool changed = true;
    while (changed && rounds-- > 0) {
        changed = false;
        for (int i = 0; i < p->nactions; i++) {
            ast_action *a = &p->actions[i];
            int ns = a->stratum;
            for (int b = 0; b < a->nreq; b++) {
                ast_atom *at = &a->requires[b];
                if (!at->primed) continue;
                if (at->is_guard) {                /* reader sits ABOVE writers */
                    for (int j = 0; j < p->nactions; j++)
                        if (action_writes_num(p, &p->actions[j], at->pred) &&
                            p->actions[j].stratum + 1 > ns)
                            ns = p->actions[j].stratum + 1;
                } else {                           /* primed bool/MV: same solve */
                    for (int j = 0; j < p->nactions; j++)
                        if (action_writes_bool(p, &p->actions[j], at->pred) &&
                            p->actions[j].stratum > ns)
                            ns = p->actions[j].stratum;
                }
            }
            if (ns > a->stratum) { a->stratum = ns; changed = true; }
        }
    }
    if (changed) {
        /* still moving past the acyclic bound: a primed-numeric cycle. Name
         * the first primed guard whose fluent's writers reached the bound. */
        for (int i = 0; i < p->nactions; i++)
            for (int b = 0; b < p->actions[i].nreq; b++) {
                ast_atom *at = &p->actions[i].requires[b];
                if (!(at->primed && at->is_guard)) continue;
                for (int j = 0; j < p->nactions; j++)
                    if (action_writes_num(p, &p->actions[j], at->pred) &&
                        p->actions[j].stratum > npgf) {
                        serr(p, at->line, at->col,
                             "the next value of '%s' depends on itself through "
                             "primed guards — `%s'` gates a rule that (directly "
                             "or through other primed reads) writes '%s'; "
                             "primed-numeric cycles oscillate and have no "
                             "answer to converge to (§5.8); break the loop or "
                             "test the current value on one side",
                             intern_name(p->syms, at->pred),
                             intern_name(p->syms, at->pred),
                             intern_name(p->syms, at->pred));
                        return;
                    }
            }
        /* fallback (shouldn't be reachable): a located error on the first guard */
        serr(p, p->actions[0].line, p->actions[0].col,
             "primed-guard cycle detected among step rules (§5.8)");
    }
}

static void semantic_pass(parser *p)
{
    synthesize_enum_sorts(p);          /* #96: before entities resolve */
    resolve_entities(p);
    populate_sort_valued_fluents(p);
    register_set_providers(p);
    build_pred_registry(p);
    check_fluent_bounds(p);
    check_functions(p);

    /* value definitions register first (#82), so any read checked below can
     * see whether its definition exists regardless of declaration order */
    for (int i = 0; i < p->nrules; i++)
        if (p->rules[i].head.is_valuedef) register_valuedef(p, i);

    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->head.is_valuedef) {
            check_valuedef(p, i);
            for (int j = i + 1; j < p->nrules; j++)
                if (strcmp(r->label, p->rules[j].label) == 0)
                    serr(p, p->rules[j].line, p->rules[j].col,
                         "duplicate rule label '%s'", r->label);
            continue;
        }
        resolve_vars(p, r->vars, r->nvars, "a rule");
        if (r->nbody == 0)
            serr(p, r->line, r->col,
                 "a rule needs a body — only a value definition "
                 "(`rule L: => v(…) = expr`) may omit it");
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
        if (r->head.is_guard || r->head.is_expr_guard)
            serr(p, r->head.line, r->head.col,
                 "a rule cannot conclude a numeric comparison — guards are "
                 "read-only inputs derived from the value store (§5.8)");
        for (int b = 0; b < r->nguard; b++)
            check_atom(p, &r->guard[b], r->vars, r->nvars, true, false, false, "an `unless` guard");
        check_safety(p, r);
        check_cardinality(p, r);
        for (int j = i + 1; j < p->nrules; j++)
            if (strcmp(r->label, p->rules[j].label) == 0)
                serr(p, p->rules[j].line, p->rules[j].col,
                     "duplicate rule label '%s'", r->label);
    }

    order_value_layers(p);             /* #82/#94: base + chain + commute checks */

    /* #82: cyclic value definitions are infinite inline regress — reject */
    {
        bool onstack[MAX_FLUENTS] = { false }, done[MAX_FLUENTS] = { false };
        for (int i = 0; i < p->nvaluedecls; i++)
            if (!done[i] && value_cycle_dfs(p, i, onstack, done))
                serr(p, p->valuedecls[i].line, p->valuedecls[i].col,
                     "the definition of '%s' reads itself (directly or through "
                     "other values) — value definitions cannot be cyclic",
                     intern_name(p->syms, p->valuedecls[i].pred));
    }

    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        resolve_vars(p, a->vars, a->nvars, a->is_ramif ? "a ramification" : "an action");
        /* Domain params (§5.6/§13) extend the guard-checking scope: `at` is a
         * known name of its domain sort, so `in_radius(T, at)` type-checks. They
         * are NOT in a->vars, so grounding leaves them as placeholder atoms. */
        var_bind ascope[2 * MAX_ARGS];
        int nas = a->nvars;
        for (int k = 0; k < a->nvars; k++) ascope[k] = a->vars[k];
        for (int k = 0; k < a->ndparams; k++) {
            a->dparams[k].sort = decode_sort(p, a->dparams[k].sort,
                                             a->dparams[k].line, a->dparams[k].col,
                                             "a domain parameter");
            if (a->dparams[k].sort >= 0 && !p->sorts[a->dparams[k].sort].is_domain)
                serr(p, a->dparams[k].line, a->dparams[k].col,
                     "parameter '%s' has sort '%s', which is not a `domain`",
                     intern_name(p->syms, a->dparams[k].name),
                     p->sorts[a->dparams[k].sort].name);
            if (nas < 2 * MAX_ARGS) ascope[nas++] = a->dparams[k];
        }
        const char *bctx = a->is_ramif ? "a ramification body" : "a `requires` clause";
        for (int b = 0; b < a->nreq; b++)
            check_atom(p, &a->requires[b], ascope, nas, true, false, a->is_ramif, bctx);
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
            var_bind cv[3 * MAX_ARGS];
            int nc = 0;
            for (int k = 0; k < nas; k++)        cv[nc++] = ascope[k];   /* action vars + domain params */
            for (int k = 0; k < bnd->nvars && nc < 3 * MAX_ARGS; k++) cv[nc++] = bnd->vars[k];
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
    stratify_steps(p);                 /* §5.8 strata + cycle rejection (#87) */
}

/* ---- grounding: emit ground rules into world_* ---------------------- */

/* Write the ground term "pred(e1,e2)" (bare "pred" at arity 0) into buf. */
/* Takes the intern table directly (not the parser) so the tick-time matcher
 * (#28) can reuse the EXACT ground-atom spelling post-compile — byte-identical
 * atoms and why-traces by construction. */
static int build_term(intern *syms, uint32_t pred, const uint32_t *args, int n,
                      char *buf, size_t cap)
{
    int off = snprintf(buf, cap, "%s", intern_name(syms, pred));
    if (n == 0) return off;
    off += snprintf(buf + off, cap - (size_t)off, "(");
    for (int i = 0; i < n && off < (int)cap; i++)
        off += snprintf(buf + off, cap - (size_t)off, "%s%s",
                        i ? "," : "", intern_name(syms, args[i]));
    if (off < (int)cap) off += snprintf(buf + off, cap - (size_t)off, ")");
    return off;
}

/* Build the interned ground atom "pred(e1,e2)" (bare "pred" at arity 0). */
/* syms-only core so the tick-time matcher (#28) spells ground atoms identically. */
static uint32_t ground_pred_s(intern *syms, uint32_t pred, const uint32_t *args, int n)
{
    if (n == 0) return pred;
    char buf[MAX_GROUND];
    build_term(syms, pred, args, n, buf, sizeof buf);
    return intern_id(syms, buf);
}

static uint32_t ground_pred(parser *p, uint32_t pred, const uint32_t *args, int n)
{
    return ground_pred_s(p->syms, pred, args, n);
}

/* Build the interned value-atom "pred(e1,e2)=v" — a multi-valued fluent's
 * value erases to this boolean atom (§5.7). */
static uint32_t ground_mv_atom_s(intern *syms, uint32_t pred, const uint32_t *args,
                                 int n, uint32_t value)
{
    char buf[MAX_GROUND];
    int off = build_term(syms, pred, args, n, buf, sizeof buf);
    if (off < (int)sizeof buf)
        snprintf(buf + off, sizeof buf - (size_t)off, "=%s",
                 intern_name(syms, value));
    return intern_id(syms, buf);
}

static uint32_t ground_mv_atom(parser *p, uint32_t pred, const uint32_t *args,
                               int n, uint32_t value)
{
    return ground_mv_atom_s(p->syms, pred, args, n, value);
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
static uint32_t ground_guard_atom_s(intern *syms, uint32_t pred, const uint32_t *args,
                                    int n, world_cmp op, long threshold)
{
    char buf[MAX_GROUND];
    int off = build_term(syms, pred, args, n, buf, sizeof buf);
    if (off < (int)sizeof buf)
        snprintf(buf + off, sizeof buf - (size_t)off, "%s%ld",
                 cmp_spelling(op), threshold);
    return intern_id(syms, buf);
}

static uint32_t ground_guard_atom(parser *p, uint32_t pred, const uint32_t *args,
                                  int n, world_cmp op, long threshold)
{
    return ground_guard_atom_s(p->syms, pred, args, n, op, threshold);
}

/* A PRIMED guard atom (§5.8 #87) interns with the next-state mark in the
 * name — "hp(grik)'<=0" — so it can never collide with the current-value
 * guard "hp(grik)<=0" over the same fluent. */
static uint32_t ground_pguard_atom(parser *p, uint32_t pred, const uint32_t *args,
                                   int n, world_cmp op, long threshold)
{
    char buf[MAX_GROUND];
    int off = build_term(p->syms, pred, args, n, buf, sizeof buf);
    if (off < (int)sizeof buf)
        snprintf(buf + off, sizeof buf - (size_t)off, "'%s%ld",
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
    case EX_CALL:  return false;              /* a host call — never a constant */
    case EX_TEST:  return false;              /* a solved verdict — never a constant */
    case EX_PRIOR: return false;              /* the chain's running value */
    case EX_NEG:
        if (!expr_fold(p, n->lhs, &a)) return false;
        *out = -a; return true;
    default:
        if (!expr_fold(p, n->lhs, &a) || !expr_fold(p, n->rhs, &b)) return false;
        switch (n->kind) {
        case EX_ADD: *out = a + b; break;
        case EX_SUB: *out = a - b; break;
        case EX_MUL: *out = a * b; break;
        case EX_DIV:                       /* floored, matching EXPR_DIV exactly */
            if (b == 0) return false;      /* stays dynamic: the VM defines x/0 = 0 */
            *out = a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
            break;
        case EX_MIN: *out = a < b ? a : b; break;
        case EX_MAX: *out = a > b ? a : b; break;
        default: return false;
        }
        return true;
    }
}

static void emit_value_inline(parser *p, uint32_t pred, const uint32_t *rargs,
                              expr_ins *code, int *pos);
static void touch_ground_fluent(parser *p, uint32_t atom, uint32_t pred,
                                const uint32_t *args, int nargs);

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
        pred_info *vp = find_pred(p, n->pred);
        if (vp && vp->is_value) {              /* inline the definition (#82) */
            emit_value_inline(p, n->pred, args, code, pos);
            return;
        }
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
    if (n->kind == EX_CALL) {
        /* push each argument, then EXPR_CALL — arg packs (pred<<8 | nargs); the
         * function pred is a plain interned name the host dispatches on (§5.6). */
        for (int k = 0; k < n->nargs; k++)
            emit_expr(p, n->cargs[k], vars, nvars, binding, code, pos);
        if (*pos < MAX_CODE) {
            code[*pos].op = EXPR_CALL;
            code[(*pos)++].arg = ((long)n->pred << 8) | (long)(n->nargs & 0xff);
        }
        return;
    }
    if (n->kind == EX_PRIOR) {                 /* the chain's running value (#82) */
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_P; code[(*pos)++].arg = 0; }
        return;
    }
    if (n->kind == EX_TEST) {
        /* ground the tested literal exactly as a body atom would be: declare
         * provider atoms, touch sparse fluents — so the solve loads its facts
         * and the commit-side verdict read (#86) finds a located literal */
        uint32_t targs[MAX_ARGS];
        for (int k = 0; k < n->nargs; k++)
            targs[k] = resolve_arg(vars, nvars, binding, n->args[k]);
        uint32_t g = ground_pred(p, n->pred, targs, n->nargs);
        pred_info *ti = find_pred(p, n->pred);
        if (ti && ti->is_provider)
            world_declare_provider_atom(p->w, g, n->pred, targs, n->nargs);
        touch_ground_fluent(p, g, n->pred, targs, n->nargs);
        if (*pos < MAX_CODE) {
            code[*pos].op = EXPR_TEST;
            code[(*pos)++].arg = ((long)g << 1) | (n->konst ? 1 : 0);
        }
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
               : n->kind == EX_MUL ? EXPR_MUL : n->kind == EX_DIV ? EXPR_DIV
               : n->kind == EX_MIN ? EXPR_MIN                     : EXPR_MAX;
    if (*pos < MAX_CODE) { code[*pos].op = op; code[(*pos)++].arg = 0; }
}

/* Inline a value read (#82): emit the definition's expression under the read's
 * resolved arguments. Because a value has ONE definition (one expr tree), every
 * reader of `v(a,b)` emits the same EX_ROLL nodes under the same binding — so
 * the §5.10 site key (node, binding, tag) is identical and all readers share
 * the draw: one die, testable twice. Distinct bindings (other targets) still
 * key apart and draw independently, which is what fireball relies on. */
static bool members_ok(parser *p, ast_atom *body, int n, var_bind *vars,
                       int nvars, const uint32_t *binding);
static dl_lit ground_lit(parser *p, ast_atom *at, var_bind *vars, int nvars,
                         const uint32_t *binding);
static const char *prov_str(parser *p, int line, char *buf, size_t n);

static void emit_valuedef_sub(parser *p, ast_rule *d, const uint32_t *rargs,
                              uint32_t *sub)
{
    (void)p;
    for (int k = 0; k < d->head.nargs; k++) {
        int f = var_index(d->vars, d->nvars, d->head.args[k].name);
        if (f >= 0) sub[f] = rargs[k];
    }
}

/* Ground a layered definition's MARKER for one binding (#82/#94): a defeasible
 * judgment `body => label(binding)` (plus the `unless` defeater), deduped by
 * the marker atom, so every read site of every rule shares ONE marker per
 * (definition, binding). The marker is what the chain's EXPR_TEST consults —
 * an ordinary literal, so `why?` names the layer that applied. A statically
 * failed #95 membership grounds nothing: the marker stays unconcluded. */
static uint32_t ensure_marker_grounded(parser *p, int ri, const uint32_t *rargs)
{
    ast_rule *d = &p->rules[ri];
    uint32_t lbl = intern_id(p->syms, d->label);
    uint32_t m = ground_pred(p, lbl, rargs, d->head.nargs);
    if (m < p->vmark_cap && p->vmark_of[m] >= 0) return m;
    atom_map_set(&p->vmark_of, &p->vmark_cap, m, 1);
    uint32_t sub[MAX_ARGS] = { 0 };
    emit_valuedef_sub(p, d, rargs, sub);
    char pbuf[MAX_NAME + 24];
    if (members_ok(p, d->body, d->nbody, d->vars, d->nvars, sub)) {
        dl_lit body[MAX_BODY];
        int nb = 0;
        for (int b = 0; b < d->nbody; b++)
            if (!d->body[b].is_member)
                body[nb++] = ground_lit(p, &d->body[b], d->vars, d->nvars, sub);
        int h = world_add_rule(p->w, intern_name(p->syms, m), DL_DEFEASIBLE,
                               dl_pos(m), body, nb);
        world_set_rule_prov(p->w, h, prov_str(p, d->line, pbuf, sizeof pbuf));
        if (d->has_guard &&
            members_ok(p, d->guard, d->nguard, d->vars, d->nvars, sub)) {
            dl_lit g2[MAX_BODY];
            int ng = 0;
            for (int b = 0; b < d->nguard; b++)
                if (!d->guard[b].is_member)
                    g2[ng++] = ground_lit(p, &d->guard[b], d->vars, d->nvars, sub);
            char gname[MAX_GROUND + 8];
            snprintf(gname, sizeof gname, "%s.unless", intern_name(p->syms, m));
            int gh = world_add_rule(p->w, gname, DL_DEFEATER, dl_neg(m), g2, ng);
            world_set_rule_prov(p->w, gh, prov_str(p, d->line, pbuf, sizeof pbuf));
        }
    }
    return m;
}

/* Inline a value read (#82/#94): the chain program. Base expression first,
 * then per layer (chain order, bottom to top) the branch-free blend
 *     v' = v + test(marker)·(f(v) − v)
 * — DESIGN §5.8's evaluate-all-and-mask shape. `prior` inside f reads the
 * running value through the prior stack, so nested value chains compose. An
 * override's f ignores prior and the same blend selects it outright. Roll
 * sites stay keyed by (definition node, binding), so every reader of one
 * value shares every die, exactly as in the single-definition slice. */
static void emit_value_inline(parser *p, uint32_t pred, const uint32_t *rargs,
                              expr_ins *code, int *pos)
{
    int vi = find_value(p, pred);
    int base = vi >= 0 ? p->value_def[vi] : -1;
    if (base < 0) return;                      /* reported in the check pass */
    if (++p->vdepth > MAX_ARGS * 2) { p->vdepth--; return; }   /* cycle backstop */
    ast_rule *bd = &p->rules[base];
    uint32_t sub[MAX_ARGS] = { 0 };
    emit_valuedef_sub(p, bd, rargs, sub);
    emit_expr(p, bd->head.lhs_root, bd->vars, bd->nvars, sub, code, pos);
    for (int L = 0; L < p->value_nlayers[vi]; L++) {
        int ri = p->value_layers[vi][L];
        ast_rule *ld = &p->rules[ri];
        uint32_t lsub[MAX_ARGS] = { 0 };
        emit_valuedef_sub(p, ld, rargs, lsub);
        long targ = (long)ensure_marker_grounded(p, ri, rargs) << 1;
#define VEMIT(o, a) do { if (*pos < MAX_CODE) {         code[*pos].op = (o); code[(*pos)++].arg = (a); } } while (0)
        VEMIT(EXPR_PPUSH, 0);                  /* v -> prior slot   */
        VEMIT(EXPR_P, 0);                      /* [v]               */
        VEMIT(EXPR_TEST, targ);                /* [v, t]            */
        emit_expr(p, ld->head.lhs_root, ld->vars, ld->nvars, lsub, code, pos);
        VEMIT(EXPR_MUL, 0);                    /* [v, t*f]          */
        VEMIT(EXPR_ADD, 0);                    /* [v + t*f]         */
        VEMIT(EXPR_TEST, targ);
        VEMIT(EXPR_P, 0);
        VEMIT(EXPR_MUL, 0);                    /* [.., t*v]         */
        VEMIT(EXPR_SUB, 0);                    /* [v + t*(f - v)]   */
        VEMIT(EXPR_PPOP, 0);
#undef VEMIT
    }
    p->vdepth--;
}

static void inst_name(parser *p, char *buf, size_t n, const char *label,
                      var_bind *vars, int nvars, const uint32_t *binding);

/* #92 sparse universe: declare a boolean state fluent the grounder just
 * referenced — the compile-time half of the touched set (the runtime half is
 * world_set's schema hook). Filters itself: only plain boolean state preds
 * (mv/num/cell keep the dense declaration), only in sparse mode, and only on
 * first touch (declare + decl provenance + index structure, exactly what the
 * dense odometer attached). */
static const char *prov_str(parser *p, int line, char *buf, size_t n);

static void touch_ground_fluent(parser *p, uint32_t atom, uint32_t pred,
                                const uint32_t *args, int nargs)
{
    if (!p->sparse || world_has_fluent(p->w, atom))
        return;
    pred_info *pi = find_pred(p, pred);
    if (!pi || !pi->is_fluent || pi->is_mv || pi->is_num)
        return;
    world_declare_fluent(p->w, atom);
    for (int i = 0; i < p->nfluents; i++)          /* decl span for inertia prov */
        if (p->fluents[i].pred == pred) {
            char pbuf[MAX_NAME + 24];
            world_set_fluent_prov(p->w, atom,
                prov_str(p, p->fluents[i].line, pbuf, sizeof pbuf));
            break;
        }
    world_set_fluent_struct(p->w, atom, pred, args, nargs);
}

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
        if (at->primed) {                          /* §5.8 #87: next-value guard,
                                                    * minted by the stratum loop */
            g = ground_pguard_atom(p, at->pred, args, at->nargs,
                                   at->cmp, at->threshold);
            world_add_primed_guard(p->w, g, term, at->cmp, at->threshold);
        } else {
            g = ground_guard_atom(p, at->pred, args, at->nargs, at->cmp, at->threshold);
            world_add_guard(p->w, g, term, at->cmp, at->threshold);
        }
    } else if (at->value != INTERN_NONE) {
        /* a join value (`at(X) = c`) resolves through the binding; a literal
         * value (`at(X) = k1`) is not a variable and passes through unchanged. */
        uint32_t val = at->value;
        int vvi = var_index(vars, nvars, val);
        if (vvi >= 0) val = binding[vvi];
        g = ground_mv_atom(p, at->pred, args, at->nargs, val);
    } else {
        g = ground_pred(p, at->pred, args, at->nargs);
        pred_info *pi = find_pred(p, at->pred);
        if (pi && pi->is_provider)                 /* a computed relation (§5.6) */
            world_declare_provider_atom(p->w, g, at->pred, args, at->nargs);
        touch_ground_fluent(p, g, at->pred, args, at->nargs);   /* #92 */
    }
    return at->neg ? dl_neg(g) : dl_pos(g);
}

/* Readable instance name for why-traces: "label[X=hero,Y=key]". The `_s` core
 * takes the intern table + bare var-name array so the tick-time matcher (#28)
 * spells instance names identically post-compile. */
static void inst_name_s(intern *syms, char *buf, size_t n, const char *label,
                        const uint32_t *varnames, int nvars, const uint32_t *binding)
{
    if (nvars == 0) { snprintf(buf, n, "%s", label); return; }
    int off = snprintf(buf, n, "%s[", label);
    for (int i = 0; i < nvars && off < (int)n; i++)
        off += snprintf(buf + off, n - (size_t)off, "%s%s=%s", i ? "," : "",
                        intern_name(syms, varnames[i]),
                        intern_name(syms, binding[i]));
    if (off < (int)n) snprintf(buf + off, n - (size_t)off, "]");
}

static void inst_name(parser *p, char *buf, size_t n, const char *label,
                      var_bind *vars, int nvars, const uint32_t *binding)
{
    uint32_t names[MAX_ARGS];
    for (int i = 0; i < nvars && i < MAX_ARGS; i++) names[i] = vars[i].name;
    inst_name_s(p->syms, buf, n, label, names, nvars, binding);
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
        if (p->sparse && !f->is_num && !f->is_mv)
            continue;   /* #92: plain boolean state preds are declared on touch
                         * (inits, ground rules/actions, world_set via the
                         * schema hook) — never as a cross-product. mv keeps the
                         * dense family (effects negate every sibling) and
                         * num/cell are value-store slots, both O(instances)
                         * kinds an author declares deliberately. */
        var_bind vb[MAX_ARGS];                     /* borrow the odometer path */
        for (int k = 0; k < f->nargs; k++) {
            vb[k].name = INTERN_NONE;
            vb[k].sort = decode_sort(p, -(int)f->argsort[k] - 2, f->line, f->col, "");
        }
        bool of = false;
        long total = instance_count(p, vb, f->nargs, &of);
        if (of) {
            /* Hard error, not silence: instance_count returns 0 on overflow, so
             * this previously declared NOTHING for an over-large state pred —
             * every fact/query on it silently no-oped ("loud failures, no
             * silent caps"). */
            serr(p, f->line, f->col,
                 "state '%s' grounds to more than %d instances — split the "
                 "sorts (§5.2 cardinality cap)",
                 intern_name(p->syms, f->pred), MAX_INSTANCES);
            return;
        }
        uint32_t binding[MAX_ARGS];
        char pbuf[MAX_NAME + 24];
        const char *decl = prov_str(p, f->line, pbuf, sizeof pbuf);
        for (long idx = 0; idx < total; idx++) {
            decode_binding(p, vb, f->nargs, idx, binding);
            if (f->is_num) {                       /* value-store slot, not an atom */
                uint32_t atom = ground_pred(p, f->pred, binding, f->nargs);
                world_declare_num(p->w, atom, f->rmin, f->rmax, f->has_range);
                if (f->merge_mode)                 /* `merge min|max` (#85) */
                    world_set_num_merge(p->w, atom, f->merge_mode == 1
                                        ? WORLD_MERGE_MIN : WORLD_MERGE_MAX);
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
                /* structured (pred, args) handoff so the world can rebuild the
                 * tick-time extension index from its live vals (#28). Base
                 * boolean fluents only — the matcher kernel matches nothing else. */
                world_set_fluent_struct(p->w, a, f->pred, binding, f->nargs);
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
        touch_ground_fluent(p, atom, a->pred, args, a->nargs);  /* #92: mv filtered inside */
        world_set(p->w, atom, true);               /* siblings stay closed-world false */
    }
}

/* #95: evaluate a membership conjunct under a concrete binding. Pure grounding
 * filter — statically decided per instance, zero runtime cost, no fixpoint
 * effect: sugar over hand-writing one rule per alternative. */
static bool member_ok(parser *p, const ast_atom *at, var_bind *vars, int nvars,
                      const uint32_t *binding)
{
    uint32_t v = resolve_arg(vars, nvars, binding, at->args[0]);
    bool in = false;
    for (int k = 0; k < at->mem_n && !in; k++)
        in = p->mempool[at->mem_ix + k] == v;
    return at->neg ? !in : in;
}

/* All membership conjuncts of `body[0..n)` hold under `binding`? */
static bool members_ok(parser *p, ast_atom *body, int n, var_bind *vars,
                       int nvars, const uint32_t *binding)
{
    for (int b = 0; b < n; b++)
        if (body[b].is_member && !member_ok(p, &body[b], vars, nvars, binding))
            return false;
    return true;
}

static void ground_rule(parser *p, ast_rule *r)
{
    if (r->head.is_valuedef) return;   /* #82: inlined at read sites, never a dl rule */
    bool of = false;
    long total = instance_count(p, r->vars, r->nvars, &of);
    if (of) {
        /* Hard error, not a warning: a rule the author wrote silently vanishing
         * from the theory is a correctness hole (missing conclusions, no
         * failure) — "loud failures, no silent caps". Until the M3 tick-time
         * matcher (#26/#28) can absorb an un-anchored cross product, an author
         * must add a sparser anchor rather than ship a dropped rule. When the
         * matcher/router lands this cap becomes a routing threshold, not a stop. */
        serr(p, r->line, r->col,
             "rule '%s' grounds to more than %d instances — add a sparser "
             "anchor or split the sorts (§5.2 cardinality cap)",
             r->label, MAX_INSTANCES);
        return;
    }
    if (total == 0) return;                        /* an empty sort: no ground rules */
    /* the large-cross-product warning is anchor-aware and lives in the semantic
     * pass now (check_cardinality) — grounding-path independent, and it only
     * fires for a genuinely un-anchored product rather than on raw size. */

    r->insts = malloc((size_t)total * sizeof *r->insts);
    r->ninst = (int)total;

    uint32_t binding[MAX_ARGS];
    char name[MAX_GROUND];
    for (long idx = 0; idx < total; idx++) {
        decode_binding(p, r->vars, r->nvars, idx, binding);
        /* #95: a failed membership conjunct statically kills this instance —
         * exactly the rule the author would not have hand-written */
        if (!members_ok(p, r->body, r->nbody, r->vars, r->nvars, binding)) {
            r->insts[idx].handle = -1;
            continue;
        }
        dl_lit head = ground_lit(p, &r->head, r->vars, r->nvars, binding);
        dl_lit body[MAX_BODY];
        int nb = 0;
        for (int b = 0; b < r->nbody; b++)
            if (!r->body[b].is_member)             /* held: drop the conjunct */
                body[nb++] = ground_lit(p, &r->body[b], r->vars, r->nvars, binding);
        inst_name(p, name, sizeof name, r->label, r->vars, r->nvars, binding);
        r->insts[idx].handle = world_add_rule(p->w, name, r->kind, head, body, nb);
        char pbuf[MAX_NAME + 24];
        world_set_rule_prov(p->w, r->insts[idx].handle,
                            prov_str(p, r->line, pbuf, sizeof pbuf));

        /* `unless G` sugars to a defeater blocking this instance's head:
         * G ~> ~head (DESIGN.md §6), reinstated whenever the guard fails. A
         * failed membership in G makes the defeater unsatisfiable — skip it; a
         * held one drops out (an all-membership guard is an unconditional
         * defeat, which is the correct reading of `unless D in { … }`). */
        if (r->has_guard &&
            members_ok(p, r->guard, r->nguard, r->vars, r->nvars, binding)) {
            dl_lit guard[MAX_BODY];
            int ng = 0;
            for (int b = 0; b < r->nguard; b++)
                if (!r->guard[b].is_member)
                    guard[ng++] = ground_lit(p, &r->guard[b], r->vars, r->nvars, binding);
            char gname[MAX_GROUND + 8];
            snprintf(gname, sizeof gname, "%s.unless", name);
            int gh = world_add_rule(p->w, gname, DL_DEFEATER, dl_complement(head),
                                    guard, ng);
            world_set_rule_prov(p->w, gh, prov_str(p, r->line, pbuf, sizeof pbuf));
        }
    }
}

/* ---- the join matcher (§5.2 item 4, #28): ground a rule from the fact-store
 * extension index rather than the sort cross product. Kernel body atoms:
 * positive base boolean fluents GENERATE (scanned from the extension); negated
 * ones FILTER (closed-world membership at the leaf); numeric comparison guards
 * (`hp(X) <= 0`) and host-answered providers (`sees(X,Y)`) are carried into the
 * emitted rule and the solver evaluates/consults them. So every var must be bound
 * by a positive generator, never merely by a guard or a (non-enumerable) provider
 * — a var bound only by `near(X,Y,2)` is the generator-provider case, deferred.
 * Expr/roll guards, mv value-joins, `unless`, and superiority still fall back to
 * eager (#44 later slices). Anything
 * outside the kernel falls back to eager ground_rule, so every story still
 * compiles both ways and the two theories differ only where the matcher runs.
 * The equivalence (identical query verdicts + why-traces) is pinned by
 * test_matcher: eager grounds every sort^k instance (most inert); the matcher
 * grounds only the body-satisfying ones — an omitted inert instance concludes
 * nothing, so no verdict moves. Later slices add derived-body stratification,
 * per-tick re-matching, and the remaining atom kinds. */

static bool rule_in_sup(parser *p, ast_rule *r)
{
    for (int i = 0; i < p->nsups; i++)
        if (!strcmp(p->sups[i].a, r->label) || !strcmp(p->sups[i].b, r->label))
            return true;
    return false;
}

static bool rule_matchable(parser *p, ast_rule *r)
{
    if (r->nvars < 1 || r->nbody < 1) return false;
    if (r->has_guard) return false;                /* `unless` defeater — later */
    if (r->head.is_num_effect) return false;
    if (rule_in_sup(p, r)) return false;           /* per-instance `>` edges — later */
    bool genbound[MAX_ARGS] = { 0 };               /* bound by a positive fluent GENERATOR */
    for (int b = 0; b < r->nbody; b++) {
        ast_atom *at = &r->body[b];
        if (at->is_expr_guard || at->primed) return false;  /* roll/expr, primed — later */
        if (at->is_member) return false;   /* #95: a static filter the tick-time
                                            * re-ground path can't yet apply */
        pred_info *pi = find_pred(p, at->pred);
        if (at->is_guard) {                        /* numeric comparison FILTER */
            if (!pi || !pi->is_num) return false;  /* guards read a numeric fluent */
            continue;                              /* binds nothing; solver evaluates it */
        }
        if (pi && pi->is_provider) {               /* host-answered relation FILTER */
            /* Not enumerable (the callback answers yes/no for BOUND args), so a
             * provider never generates a var — its args must be fluent-bound (the
             * genbound check enforces it). The emitted provider atom is consulted
             * from the callback by the solver, like a guard. A var bound ONLY by a
             * provider is the generator-provider case (deferred): it fails
             * genbound and the rule stays eager. */
            continue;
        }
        if (at->value != INTERN_NONE) return false;        /* mv value-join — later */
        if (!pi || !pi->is_fluent || pi->is_num || pi->is_mv)
            return false;                                  /* base boolean fluent only */
        /* Only a POSITIVE fluent atom is a generator (scanned from the extension).
         * A negated atom is a closed-world FILTER — it binds nothing (§5.2 range
         * restriction), so it never contributes here. */
        if (!at->neg)
            for (int k = 0; k < at->nargs; k++) {
                int vi = var_index(r->vars, r->nvars, at->args[k].name);
                if (vi >= 0) genbound[vi] = true;
            }
    }
    /* Every var must be generator-bound — stricter than compute_bound_vars, which
     * counts a numeric guard or an anchored provider as binding for
     * range-restriction safety. The matcher can only ENUMERATE a var from a
     * positive fluent's tuples, never from a guard or a (non-enumerable) provider. */
    for (int i = 0; i < r->nvars; i++) if (!genbound[i]) return false;
    return true;
}

static bool pred_in_refs(parser *p, uint32_t pred)
{
    for (int i = 0; i < p->nrefs; i++)
        if (p->refs[i].pred == pred) return true;
    return false;
}

/* Island judgment (#80): a matchable rule whose head predicate's ENTIRE proof
 * cone is its own match set — so the matched layer can be a set-materialized
 * view (world_view_*) instead of per-tick ground rules. Requires, on top of
 * rule_matchable: a proving kind (a defeater never concludes); a head pred
 * that is a pure judgment (not a fluent — closed-world facts would join its
 * cone — nor mv/numeric/cell/provider); no OTHER rule concluding or attacking
 * it (either polarity — this also subsumes the superiority clause: this rule
 * itself is already gated by rule_in_sup, and any sup-carrying sibling is an
 * "other head"); and read NOWHERE — p->refs already collects every read site:
 * rule bodies, `unless` guards, action/ramification `requires`, and binder
 * `where`/`when` (heads, effects and inits deliberately don't note). Called
 * after desugar_bands, so band-synthesized `>` edges are visible via nsups.
 * Guard/provider FILTERS are fine: the matcher evaluates them at the match
 * leaf (world_num_cmp_holds / world_provider_holds_at) — the same values the
 * solver's fact-load would read, at the same freshness (both run under one
 * matched_stale trigger). A guard-filtered head answers UNDECIDED instead of
 * eager's vacuous REFUTED — the documented provability-contract asymmetry the
 * matcher already has for unsatisfiable instances. */
static bool rule_island(parser *p, ast_rule *r)
{
    if (r->kind == DL_DEFEATER) return false;
    pred_info *hp = find_pred(p, r->head.pred);
    if (hp && (hp->is_fluent || hp->is_mv || hp->is_num || hp->is_cell ||
               hp->is_provider))
        return false;
    if (pred_in_refs(p, r->head.pred)) return false;
    for (int j = 0; j < p->nrules; j++)
        if (&p->rules[j] != r && p->rules[j].head.pred == r->head.pred)
            return false;
    return true;
}

typedef struct { int *h; int n, cap; } inst_list;
static void inst_push(inst_list *L, int handle)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 16;
        L->h = realloc(L->h, (size_t)L->cap * sizeof *L->h);
    }
    L->h[L->n++] = handle;
}

/* Emit one matched instance — the same head/body/name/provenance ground_rule
 * writes per odometer step, but for a binding the join proved satisfiable. */
static void emit_matched(parser *p, ast_rule *r, const uint32_t *bind, inst_list *L)
{
    dl_lit head = ground_lit(p, &r->head, r->vars, r->nvars, bind);
    dl_lit body[MAX_BODY];
    for (int b = 0; b < r->nbody; b++)
        body[b] = ground_lit(p, &r->body[b], r->vars, r->nvars, bind);
    char name[MAX_GROUND];
    inst_name(p, name, sizeof name, r->label, r->vars, r->nvars, bind);
    int h = world_add_rule(p->w, name, r->kind, head, body, r->nbody);
    char pbuf[MAX_NAME + 24];
    world_set_rule_prov(p->w, h, prov_str(p, r->line, pbuf, sizeof pbuf));
    inst_push(L, h);
}

/* Membership test for a body FILTER atom whose vars are all bound: is the ground
 * fluent currently true? Closed-world, so a negated body literal `~atom` holds
 * iff this returns false. (§5.2 discipline 1: range restriction guarantees a
 * positive generator bound the vars before any filter sees them.) */
static bool atom_present(factindex *ix, ast_atom *at, var_bind *vars, int nvars,
                         const uint32_t *bind)
{
    bool     filt[FACTINDEX_MAXARGS];
    uint32_t want[FACTINDEX_MAXARGS];
    for (int k = 0; k < at->nargs; k++) {
        int vi = var_index(vars, nvars, at->args[k].name);
        filt[k] = true;
        want[k] = vi >= 0 ? bind[vi] : at->args[k].name;
    }
    factindex_cursor c;
    factindex_scan(ix, at->pred, filt, want, &c);
    uint32_t tup[FACTINDEX_MAXARGS];
    return factindex_next(&c, tup);
}

/* A body atom that is a host-answered provider relation — a filter, not a
 * generator, and not in the boolean fact index (so the leaf negation-membership
 * test must skip it; the solver consults its callback instead). */
static bool atom_is_provider(parser *p, const ast_atom *at)
{
    if (at->is_guard) return false;
    pred_info *pi = find_pred(p, at->pred);
    return pi && pi->is_provider;
}

/* Semi-naïve nested-loop join: probe positive body atom `b`'s extension with the
 * positions already bound (constants and vars bound by earlier atoms), bind its
 * free vars from each matching tuple, recurse. Negated atoms bind nothing, so
 * they are deferred to the leaf and applied as closed-world filters once every
 * var is bound. At the leaf the emitted rule still carries the full body (incl.
 * the negated literals), so it is byte-identical to the eager instance. */
static void match_rec(parser *p, ast_rule *r, int b, uint32_t *bind, inst_list *L)
{
    if (b == r->nbody) {
        for (int f = 0; f < r->nbody; f++)
            if (r->body[f].neg && !r->body[f].is_guard &&
                !atom_is_provider(p, &r->body[f]) &&
                atom_present(p->fidx, &r->body[f], r->vars, r->nvars, bind))
                return;                                /* ~fluent fails: it is true */
        emit_matched(p, r, bind, L);
        return;
    }
    ast_atom *at = &r->body[b];
    if (at->neg || at->is_guard || atom_is_provider(p, at)) {  /* filter: defer */
        match_rec(p, r, b + 1, bind, L); return;   /* negation → leaf; guard/provider → solver */
    }
    int n = at->nargs;

    bool     filt[FACTINDEX_MAXARGS];
    uint32_t want[FACTINDEX_MAXARGS];
    int      freevar[FACTINDEX_MAXARGS];           /* var this position binds, or -1 */
    for (int k = 0; k < n; k++) {
        int vi = var_index(r->vars, r->nvars, at->args[k].name);
        if (vi < 0)                       { filt[k] = true;  want[k] = at->args[k].name; freevar[k] = -1; }
        else if (bind[vi] != INTERN_NONE) { filt[k] = true;  want[k] = bind[vi];         freevar[k] = -1; }
        else                              { filt[k] = false; want[k] = 0;                freevar[k] = vi; }
    }

    factindex_cursor c;
    factindex_scan(p->fidx, at->pred, filt, want, &c);
    uint32_t tup[FACTINDEX_MAXARGS];
    while (factindex_next(&c, tup)) {
        int set[FACTINDEX_MAXARGS], nset = 0;
        bool ok = true;
        for (int k = 0; k < n; k++) {
            int vi = freevar[k];
            if (vi < 0) continue;
            if (bind[vi] == INTERN_NONE) { bind[vi] = tup[k]; set[nset++] = vi; }
            else if (bind[vi] != tup[k]) { ok = false; break; }   /* repeated var must agree */
        }
        if (ok) match_rec(p, r, b + 1, bind, L);
        for (int s = 0; s < nset; s++) bind[set[s]] = INTERN_NONE; /* backtrack */
    }
}

static void ground_rule_matched(parser *p, ast_rule *r)
{
    inst_list L = { 0, 0, 0 };
    uint32_t bind[MAX_ARGS];
    for (int i = 0; i < r->nvars; i++) bind[i] = INTERN_NONE;
    match_rec(p, r, 0, bind, &L);

    if (L.n == 0) { r->insts = NULL; r->ninst = 0; free(L.h); return; }
    r->insts = malloc((size_t)L.n * sizeof *r->insts);
    for (int i = 0; i < L.n; i++) r->insts[i].handle = L.h[i];
    r->ninst = L.n;
    free(L.h);
}

/* Base-boolean-fluent extension index from the init facts — the matcher scans
 * this instead of enumerating sorts. Numeric/mv inits are not boolean
 * extensions and are skipped (the kernel never matches over them). */
static void build_fact_index(parser *p)
{
    p->fidx = factindex_new();
    for (int i = 0; i < p->ninits; i++) {
        ast_atom *a = &p->inits[i];
        if (a->is_guard || a->value != INTERN_NONE) continue;
        uint32_t args[MAX_ARGS];
        for (int k = 0; k < a->nargs; k++) args[k] = a->args[k].name;
        factindex_add(p->fidx, a->pred, args, a->nargs);
    }
}

/* ---- tick-time matcher (#28, the runtime half) ---------------------------
 * A compact retained plan of the matchable rules, re-grounded against the
 * world's LIVE fact index each tick. Owns deep copies (no parser-lifetime
 * dependency) and reuses the syms-only cores (ground_pred_s / ground_mv_atom_s /
 * inst_name_s), so re-materialized atoms and why-traces are byte-identical to the
 * eager path. Kernel (rule_matchable): base boolean fluents (positive generate,
 * negated filter) plus numeric comparison guards and provider relations (carried
 * into the rule, solver-evaluated/consulted); the emit path never needs the expr
 * branch.
 *
 * m_match_rec / m_ground_lit / m_var_index deliberately mirror the compile-time
 * match_rec / ground_lit / var_index over the retained plan instead of the parser
 * AST. LOCKSTEP INVARIANT: a change to the eager matchable-kernel semantics must
 * be mirrored here, or the equivalence pin (test_matcher / test_ticktime) rots.
 * The full unification behind a shared plan+callback is the adoption target — as
 * is sinking this executor below the lang tier (or lowering the plan into a
 * state-owned structure), which routing through world_step will require since
 * state must not depend on lang. For the prototype the host drives re-grounding,
 * so no state->lang edge exists yet. */
typedef struct {
    uint32_t  pred, value;     /* value != INTERN_NONE: an mv head "pred(a)=v" */
    bool      neg;
    bool      is_guard;        /* numeric comparison `pred(args) <cmp> threshold` */
    bool      is_provider;     /* host-answered relation (consulted by the solver) */
    world_cmp cmp;
    long      threshold;
    int       nargs;
    uint32_t  arg[MAX_ARGS];   /* a var NAME or a constant entity (disambiguated below) */
} m_atom;

typedef struct {
    char        *label, *prov; /* owned copies (outlive the parser) */
    dl_rule_kind kind;
    int          nvars;
    uint32_t     varname[MAX_ARGS];
    m_atom       head;
    m_atom       body[MAX_BODY];
    int          nbody;
    bool         island;       /* #80: matches go to a world view, not rules */
    int          view;         /* world_view_new handle, -1 when !island      */
} m_rule;

typedef struct {               /* #92: one boolean state pred's shape */
    uint32_t pred;
    int      nargs;
    int      argsort[MAX_ARGS];   /* parser sort indices, matching ent_sort */
} m_schema;

struct story_matcher {
    intern *syms;
    world  *w;
    m_rule *rules;
    int     nrules, caprules;
    long    probes;            /* fact-tuples visited in the last reground (#46) */
    /* #92 sparse fluent universe: the retained schema — boolean state preds +
     * entity->sort membership, deep-copied before the parser tables are torn
     * down (same discipline as the retained rule plan). Backs the
     * world_schema_fn hook: recognize/decompose ground fluent atoms so the
     * world can lazily declare on touch and answer the rest closed-world. */
    m_schema *schemas;
    int       nschemas;
    int      *ent_sort;        /* entity atom -> sort index, -1 = not an entity */
    uint32_t  ent_sort_cap;
};

static char *m_dup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    memcpy(d, s, n);
    return d;
}

static int m_var_index(const m_rule *r, uint32_t name)
{
    for (int i = 0; i < r->nvars; i++) if (r->varname[i] == name) return i;
    return -1;
}

/* ground_lit restricted to the matchable kernel: the plain and mv-head branches.
 * An arg is a var (resolve through `bind`) or a constant entity (pass through) —
 * exactly resolve_arg's rule, over the retained var-name array. */
static dl_lit m_ground_lit(story_matcher *m, const m_rule *r, const m_atom *a,
                           const uint32_t *bind)
{
    uint32_t args[MAX_ARGS];
    for (int k = 0; k < a->nargs; k++) {
        int vi = m_var_index(r, a->arg[k]);
        args[k] = vi >= 0 ? bind[vi] : a->arg[k];
    }
    uint32_t g;
    if (a->is_guard) {                             /* numeric landmark `pred(a)<op>n` */
        uint32_t term = ground_pred_s(m->syms, a->pred, args, a->nargs);
        g = ground_guard_atom_s(m->syms, a->pred, args, a->nargs, a->cmp, a->threshold);
        world_add_guard(m->w, g, term, a->cmp, a->threshold);
    } else if (a->is_provider) {                   /* host-answered relation (§5.6) */
        g = ground_pred_s(m->syms, a->pred, args, a->nargs);
        world_declare_provider_atom(m->w, g, a->pred, args, a->nargs);
    } else if (a->value != INTERN_NONE) {          /* mv head "pred(a)=v" (§5.7) */
        uint32_t val = a->value;
        int vvi = m_var_index(r, val);
        if (vvi >= 0) val = bind[vvi];
        g = ground_mv_atom_s(m->syms, a->pred, args, a->nargs, val);
    } else {
        g = ground_pred_s(m->syms, a->pred, args, a->nargs);
    }
    return a->neg ? dl_neg(g) : dl_pos(g);
}

/* Emit one matched instance as an ordinary matched rule (world_add_rule) —
 * the non-island path, and the materialize-on-why path for island instances
 * (#80): the world hands the stored bindings back and this re-creates exactly
 * the rule full emission would have, so the shared trace renderer output is
 * byte-identical. */
static void m_emit_rule(story_matcher *m, m_rule *r, const uint32_t *bind)
{
    dl_lit head = m_ground_lit(m, r, &r->head, bind);
    dl_lit body[MAX_BODY];
    for (int b = 0; b < r->nbody; b++)
        body[b] = m_ground_lit(m, r, &r->body[b], bind);
    char name[MAX_GROUND];
    inst_name_s(m->syms, name, sizeof name, r->label, r->varname, r->nvars, bind);
    int h = world_add_rule(m->w, name, r->kind, head, body, r->nbody);
    world_set_rule_prov(m->w, h, r->prov);
}

static void m_emit(story_matcher *m, m_rule *r, const uint32_t *bind)
{
    if (r->island) {                               /* #80: membership, not rules */
        /* Guard/provider FILTERS evaluate here, at the leaf, instead of being
         * carried into a rule body for the solver: same values, same
         * matched_stale freshness. A failing filter emits nothing — the head
         * stays unmatched (UNDECIDED), where eager's inapplicable ground rule
         * would refute it: the documented provability-contract asymmetry.
         * Nothing is registered — landmark atoms appear only if a why
         * materializes the instance (m_emit_rule runs m_ground_lit then). */
        for (int b = 0; b < r->nbody; b++) {
            const m_atom *a = &r->body[b];
            if (!a->is_guard && !a->is_provider)
                continue;
            uint32_t args[MAX_ARGS];
            for (int k = 0; k < a->nargs; k++) {
                int vi = m_var_index(r, a->arg[k]);
                args[k] = vi >= 0 ? bind[vi] : a->arg[k];
            }
            if (a->is_guard) {
                uint32_t term = ground_pred_s(m->syms, a->pred, args, a->nargs);
                if (!world_num_cmp_holds(m->w, term, a->cmp, a->threshold))
                    return;
            } else if (!world_provider_holds_at(m->w, a->pred, args, a->nargs)) {
                return;
            }
        }
        dl_lit head = m_ground_lit(m, r, &r->head, bind);
        world_view_add(m->w, r->view, head.atom, bind, r->nvars);
        return;
    }
    m_emit_rule(m, r, bind);
}

/* atom_present over the compact plan + a live index (mirrors atom_present). */
static bool m_atom_present(const factindex *ix, const m_rule *r, const m_atom *a,
                           const uint32_t *bind)
{
    bool     filt[FACTINDEX_MAXARGS];
    uint32_t want[FACTINDEX_MAXARGS];
    for (int k = 0; k < a->nargs; k++) {
        int vi = m_var_index(r, a->arg[k]);
        filt[k] = true;
        want[k] = vi >= 0 ? bind[vi] : a->arg[k];
    }
    factindex_cursor c;
    factindex_scan(ix, a->pred, filt, want, &c);
    uint32_t tup[FACTINDEX_MAXARGS];
    return factindex_next(&c, tup);
}

static bool m_is_generator(const m_atom *a)   /* a positive fluent — scannable */
{
    return !a->neg && !a->is_guard && !a->is_provider;
}

/* Selectivity-ordered join plan (#46): visit the generator atoms smallest live
 * extension first (factindex_count), each connected to an already-bound var so we
 * never form an accidental cartesian sub-join. The order is a deterministic
 * function of the current extension sizes (I4). Reordering is semantically
 * invisible — the join commutes, so the same instance set is produced — it only
 * shrinks the intermediate bindings walked. `gorder` gets the body indices of the
 * generators in visitation order; filters (negation, guards, providers) are not
 * ordered here — they apply at the leaf / to the solver. */
static void plan_generators(m_rule *r, const factindex *ix, int *gorder, int *pngen)
{
    bool used[MAX_BODY] = { 0 };
    bool vb[MAX_ARGS]   = { 0 };            /* vars bound by already-ordered generators */
    int  ngen = 0, ncand = 0;
    for (int b = 0; b < r->nbody; b++)
        if (m_is_generator(&r->body[b])) ncand++;

    for (int step = 0; step < ncand; step++) {
        int  best = -1; long bestcnt = 0; bool best_conn = false;
        for (int b = 0; b < r->nbody; b++) {
            m_atom *at = &r->body[b];
            if (used[b] || !m_is_generator(at)) continue;
            bool conn = (ngen == 0);        /* the first pick is unconstrained */
            for (int k = 0; k < at->nargs && !conn; k++) {
                int vi = m_var_index(r, at->arg[k]);
                if (vi >= 0 && vb[vi]) conn = true;
            }
            long cnt = factindex_count(ix, at->pred);
            /* prefer a connected atom; among equal connectivity, the smaller
             * extension; ties break on body order (deterministic). */
            bool better = best < 0 ||
                          (conn != best_conn ? conn : cnt < bestcnt);
            if (better) { best = b; bestcnt = cnt; best_conn = conn; }
        }
        gorder[ngen++] = best;
        used[best] = true;
        for (int k = 0; k < r->body[best].nargs; k++) {
            int vi = m_var_index(r, r->body[best].arg[k]);
            if (vi >= 0) vb[vi] = true;
        }
    }
    *pngen = ngen;
}

/* Semi-naïve nested-loop join over the compact plan and a live fact index,
 * visiting generators in the selectivity order `gorder` (see plan_generators).
 * At the leaf every var is bound; the negated fluent filters are applied, then
 * the instance is emitted with its full body. */
static void m_match_rec(story_matcher *m, m_rule *r, const int *gorder, int ngen,
                        int gi, uint32_t *bind, const factindex *ix)
{
    if (gi == ngen) {
        for (int f = 0; f < r->nbody; f++)
            if (r->body[f].neg && !r->body[f].is_guard && !r->body[f].is_provider &&
                m_atom_present(ix, r, &r->body[f], bind))
                return;                                /* ~fluent fails: it is true */
        m_emit(m, r, bind);
        return;
    }
    m_atom *at = &r->body[gorder[gi]];
    int n = at->nargs;
    bool     filt[FACTINDEX_MAXARGS];
    uint32_t want[FACTINDEX_MAXARGS];
    int      freevar[FACTINDEX_MAXARGS];
    for (int k = 0; k < n; k++) {
        int vi = m_var_index(r, at->arg[k]);
        if (vi < 0)                       { filt[k] = true;  want[k] = at->arg[k]; freevar[k] = -1; }
        else if (bind[vi] != INTERN_NONE) { filt[k] = true;  want[k] = bind[vi];   freevar[k] = -1; }
        else                              { filt[k] = false; want[k] = 0;          freevar[k] = vi; }
    }
    factindex_cursor c;
    factindex_scan(ix, at->pred, filt, want, &c);
    uint32_t tup[FACTINDEX_MAXARGS];
    while (factindex_next(&c, tup)) {
        m->probes++;                                   /* one intermediate binding walked */
        int set[FACTINDEX_MAXARGS], nset = 0;
        bool ok = true;
        for (int k = 0; k < n; k++) {
            int vi = freevar[k];
            if (vi < 0) continue;
            if (bind[vi] == INTERN_NONE) { bind[vi] = tup[k]; set[nset++] = vi; }
            else if (bind[vi] != tup[k]) { ok = false; break; }   /* repeated var agrees */
        }
        if (ok) m_match_rec(m, r, gorder, ngen, gi + 1, bind, ix);
        for (int s = 0; s < nset; s++) bind[set[s]] = INTERN_NONE; /* backtrack */
    }
}

static void m_capture_atom(parser *p, m_atom *d, const ast_atom *s)
{
    d->pred = s->pred;
    d->value = s->value;
    d->neg = s->neg;
    d->is_guard = s->is_guard;
    d->is_provider = atom_is_provider(p, s);
    d->cmp = s->cmp;
    d->threshold = s->threshold;
    d->nargs = s->nargs;
    for (int k = 0; k < s->nargs && k < MAX_ARGS; k++) d->arg[k] = s->args[k].name;
}

/* Deep-copy one matchable rule into the retained plan (survives the parser). */
static void matcher_capture(story_matcher *m, parser *p, ast_rule *r, bool island)
{
    if (m->nrules == m->caprules) {
        m->caprules = m->caprules ? m->caprules * 2 : 16;
        m->rules = realloc(m->rules, (size_t)m->caprules * sizeof *m->rules);
    }
    m_rule *d = &m->rules[m->nrules++];
    d->label = m_dup(r->label);
    char pbuf[MAX_NAME + 24];
    d->prov = m_dup(prov_str(p, r->line, pbuf, sizeof pbuf));
    d->kind = r->kind;
    d->nvars = r->nvars;
    for (int i = 0; i < r->nvars && i < MAX_ARGS; i++) d->varname[i] = r->vars[i].name;
    m_capture_atom(p, &d->head, &r->head);
    d->nbody = r->nbody;
    for (int b = 0; b < r->nbody && b < MAX_BODY; b++)
        m_capture_atom(p, &d->body[b], &r->body[b]);
    d->island = island;
    d->view = island ? world_view_new(p->w, r->head.pred, r->head.neg, r->kind)
                     : -1;
}

/* #92: retain the boolean-state-pred schema + entity->sort membership before
 * the parser tables are freed. Only what the schema hook needs — plain boolean
 * preds (mv/num/cell stay densely declared and are never recognized here). */
static void matcher_capture_schema(story_matcher *m, parser *p)
{
    for (int i = 0; i < p->nfluents; i++) {
        ast_fluent *f = &p->fluents[i];
        if (f->is_num || f->is_mv)
            continue;
        m->schemas = realloc(m->schemas,
                             (size_t)(m->nschemas + 1) * sizeof *m->schemas);
        m_schema *s = &m->schemas[m->nschemas++];
        s->pred = f->pred;
        s->nargs = f->nargs;
        for (int k = 0; k < f->nargs; k++)
            s->argsort[k] = decode_sort(p, -(int)f->argsort[k] - 2,
                                        f->line, f->col, "");
    }
    for (int e = 0; e < p->nents; e++) {
        uint32_t a = p->ents[e].atom;
        if (a >= m->ent_sort_cap) {
            uint32_t nc = m->ent_sort_cap ? m->ent_sort_cap : 64;
            while (nc <= a) nc *= 2;
            m->ent_sort = realloc(m->ent_sort, (size_t)nc * sizeof *m->ent_sort);
            for (uint32_t k = m->ent_sort_cap; k < nc; k++) m->ent_sort[k] = -1;
            m->ent_sort_cap = nc;
        }
        m->ent_sort[a] = p->ents[e].sort;
    }
}

/* The schema hook (#92). LOCKSTEP with build_term — the single ground-name
 * printer: this accepts EXACTLY "pred(e1,..,ek)" (bare pred at arity 0) for a
 * retained boolean state pred whose args are entities of the declared sorts,
 * and rejects every other spelling — mv "p(a)=v", guard "p(a)<=5", primed
 * "p(a)'" all fail the final-char-is-')' / bare-pred test. intern_find_n
 * probes substrings without interning, so recognizing (or rejecting) an atom
 * never mutates the table. Pure: a function of the retained schema + intern
 * contents at call time (I4). */
static bool matcher_schema_thunk(void *ctx, uint32_t atom, uint32_t *pred,
                                 uint32_t *args, int *nargs)
{
    story_matcher *m = ctx;
    const char *name = intern_name(m->syms, atom);
    const char *lp = strchr(name, '(');
    if (!lp) {                                     /* arity 0: atom IS the pred */
        for (int i = 0; i < m->nschemas; i++)
            if (m->schemas[i].pred == atom && m->schemas[i].nargs == 0) {
                *pred = atom;
                *nargs = 0;
                return true;
            }
        return false;
    }
    uint32_t pa = intern_find_n(m->syms, name, (uint32_t)(lp - name));
    if (pa == INTERN_NONE)
        return false;
    const m_schema *s = NULL;
    for (int i = 0; i < m->nschemas; i++)
        if (m->schemas[i].pred == pa && m->schemas[i].nargs > 0) { s = &m->schemas[i]; break; }
    if (!s)
        return false;
    size_t len = strlen(name);
    if (name[len - 1] != ')')
        return false;
    const char *tok = lp + 1, *end = name + len - 1;
    int k = 0;
    while (tok < end) {
        const char *c = memchr(tok, ',', (size_t)(end - tok));
        const char *te = c ? c : end;
        if (te == tok || k >= s->nargs)
            return false;
        uint32_t e = intern_find_n(m->syms, tok, (uint32_t)(te - tok));
        if (e == INTERN_NONE || e >= m->ent_sort_cap ||
            m->ent_sort[e] != s->argsort[k])
            return false;
        args[k++] = e;
        tok = te + 1;
    }
    if (k != s->nargs)
        return false;
    *pred = pa;
    *nargs = k;
    return true;
}

/* Materialize-on-why (#80): the world hands back one stored view row; re-emit
 * it through the normal matched-rule path so world_why renders the exact trace
 * full emission would have. */
static void matcher_materialize_thunk(void *ctx, world *w, uint32_t atom,
                                      int view, const uint32_t *bind, int nvars)
{
    (void)w; (void)atom; (void)nvars;
    story_matcher *m = ctx;
    for (int i = 0; i < m->nrules; i++)
        if (m->rules[i].island && m->rules[i].view == view) {
            m_emit_rule(m, &m->rules[i], bind);
            return;
        }
}

void story_matcher_reground(story_matcher *m)
{
    world_matched_reset(m->w);                       /* drop the previous layer */
    world_views_reset(m->w);                         /* islands: previous match set (#80) */
    const factindex *ix = world_fact_index(m->w);    /* refresh from live vals  */
    m->probes = 0;
    uint32_t bind[MAX_ARGS];
    for (int i = 0; i < m->nrules; i++) {
        m_rule *r = &m->rules[i];
        for (int v = 0; v < r->nvars; v++) bind[v] = INTERN_NONE;
        int gorder[MAX_BODY], ngen = 0;
        plan_generators(r, ix, gorder, &ngen);       /* selectivity order for THIS tick */
        m_match_rec(m, r, gorder, ngen, 0, bind, ix);
    }
}

/* Fact-tuples the last reground walked — the join's intermediate work. With
 * selectivity ordering this tracks the smallest generator's extension, not the
 * largest (#46). */
long story_matcher_last_probes(const story_matcher *m) { return m->probes; }

world *story_matcher_world(const story_matcher *m) { return m->w; }

void story_matcher_free(story_matcher *m)
{
    if (!m) return;
    for (int i = 0; i < m->nrules; i++) { free(m->rules[i].label); free(m->rules[i].prov); }
    free(m->rules);
    free(m->schemas);
    free(m->ent_sort);
    free(m);
}

/* Emit one grounded numeric effect. A typed contribution (#83, `as fire`)
 * routes to its enum bucket and registers the response atoms for this target
 * instance — resistant/vulnerable/immune(<subject>, <type>), the fixed
 * response vocabulary — which the commit pipeline consults after summation and
 * before the clamp (#84). Registration is per (instance, type) and idempotent;
 * types never mentioned by an effect need no response. */
static void emit_num_effect(parser *p, int rule, const ast_atom *e,
                            uint32_t num, uint32_t subject,
                            const expr_ins *code, int nc)
{
    if (e->as_value == INTERN_NONE) {
        world_add_num_effect(p->w, rule, num, e->numop, code, nc);
        return;
    }
    int ei = find_entity(p, e->as_value);
    int dt = ei >= 0 ? p->ent_pos[ei] : -1;        /* position within the enum sort */
    world_add_num_effect_typed(p->w, rule, num, e->numop, code, nc, dt);
    uint32_t rargs[2] = { subject, e->as_value };
    world_set_num_response(p->w, num, dt,
        ground_pred(p, intern_id(p->syms, "resistant"),  rargs, 2),
        ground_pred(p, intern_id(p->syms, "vulnerable"), rargs, 2),
        ground_pred(p, intern_id(p->syms, "immune"),     rargs, 2));
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
        /* #95: a failed membership in `requires` kills this instance statically */
        if (!members_ok(p, a->requires, a->nreq, a->vars, a->nvars, binding))
            continue;
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
        int ncond = 0;
        for (int b = 0; b < a->nreq; b++) {
            if (a->requires[b].is_member)          /* #95: held — drop the conjunct */
                continue;
            conds[ncond].lit = ground_lit(p, &a->requires[b], a->vars, a->nvars, binding);
            /* Bare atom = current state; a postfix `'` (ramification bodies
             * only) reads the next state (§5.4). A primed NUMERIC guard is NOT
             * marked primed here: its atom is already the next-value guard,
             * asserted as a strict fact by the stratum loop (§5.8 #87) — the
             * primed flag would wrongly remap it to a boolean primed column. */
            conds[ncond].primed = a->requires[b].primed && !a->requires[b].is_guard;
            ncond++;
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
            /* a join effect value (`at(X) = to`) resolves through the binding */
            uint32_t ev = e->value;
            int evi = var_index(a->vars, a->nvars, ev);
            if (evi >= 0) ev = binding[evi];
            eff[ne++] = dl_pos(ground_mv_atom(p, e->pred, args, e->nargs, ev));
            for (int v = 0; v < pi->nvalues && ne < MAX_BODY; v++)
                if (pi->values[v] != ev)
                    eff[ne++] = dl_neg(ground_mv_atom(p, e->pred, args, e->nargs,
                                                      pi->values[v]));
        }
        int h = world_add_step_rule(p->w, aname, act, conds, ncond, eff, ne);
        if (a->stratum > 0) world_set_step_stratum(p->w, h, a->stratum);   /* #87 */
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
            emit_num_effect(p, h, e, num, nargs[0], code, nc);
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

                /* #95: membership in `where` (or `when`, below) is statically
                 * decided per inner binding — a failed one skips the instance */
                if (!members_ok(p, bnd->where, bnd->nwhere, cv, ncv, cb))
                    continue;
                for (int it = 0; it < bnd->nitems; it++) {
                    binder_item *item = &bnd->items[it];
                    if (!members_ok(p, item->when, item->nwhen, cv, ncv, cb))
                        continue;
                    /* conds = action requires + binder where + item when */
                    step_cond bc[MAX_BODY];
                    int nbc = 0;
                    for (int b = 0; b < a->nreq && nbc < MAX_BODY; b++) {
                        if (a->requires[b].is_member) continue;   /* held (#95) */
                        bc[nbc].lit = ground_lit(p, &a->requires[b], cv, ncv, cb);
                        bc[nbc++].primed = a->requires[b].primed &&
                                           !a->requires[b].is_guard;   /* #87 */
                    }
                    for (int b = 0; b < bnd->nwhere && nbc < MAX_BODY; b++) {
                        if (bnd->where[b].is_member) continue;    /* held (#95) */
                        bc[nbc].lit = ground_lit(p, &bnd->where[b], cv, ncv, cb);
                        bc[nbc++].primed = false;
                    }
                    for (int b = 0; b < item->nwhen && nbc < MAX_BODY; b++) {
                        if (item->when[b].is_member) continue;    /* held (#95) */
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
                            uint32_t ev = e->value;      /* join value → binding */
                            int evi = var_index(cv, ncv, ev);
                            if (evi >= 0) ev = cb[evi];
                            eff2[ne2++] = dl_pos(ground_mv_atom(p, e->pred, mvarg,
                                                                e->nargs, ev));
                            for (int v = 0; v < pi->nvalues && ne2 < MAX_BODY; v++)
                                if (pi->values[v] != ev)
                                    eff2[ne2++] = dl_neg(ground_mv_atom(p, e->pred, mvarg,
                                                                        e->nargs, pi->values[v]));
                        }
                    }
                    char bname[MAX_GROUND];
                    inst_name(p, bname, sizeof bname, a->name, cv, ncv, cb);
                    int h2 = world_add_step_rule(p->w, bname, act, bc, nbc, eff2, ne2);
                    if (a->stratum > 0)
                        world_set_step_stratum(p->w, h2, a->stratum);   /* #87 */
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
                        emit_num_effect(p, h2, e, num, narg[0], code, nc);
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
        if (ra->insts[ai].handle < 0 || rb->insts[bi].handle < 0)
            continue;                              /* #95: membership-dropped instance */
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
    if (ra->head.is_valuedef || rb->head.is_valuedef) {
        /* #82/#94: `>` between two definitions of ONE value orders its layer
         * chain — consumed by order_value_layers, nothing to ground here */
        if (ra->head.is_valuedef && rb->head.is_valuedef &&
            ra->head.pred == rb->head.pred)
            return;
        serr(p, s->aline, s->acol,
             ra->head.is_valuedef != rb->head.is_valuedef
                 ? "'%s' > '%s' mixes a value definition with an ordinary rule "
                   "— superiority orders definitions of ONE value, or ordinary "
                   "rules among themselves"
                 : "'%s' > '%s' orders definitions of two DIFFERENT values — "
                   "each value's chain orders independently",
             s->a, s->b);
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
        if (is_fluent_pred(p, a) || is_head_pred(p, a) ||
            (pi && (pi->is_provider || pi->is_value)))
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
        case TK_SORT: case TK_DOMAIN: case TK_ENTITY: case TK_STATE:
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
            if (r->head.is_valuedef) continue;     /* #82: not a dl rule */
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
        if (r->head.is_valuedef) continue;         /* #82: not a dl rule */
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
    /* Honor the same cardinality cap the eager path enforces: the lane builder
     * below allocates npred×niter×nent uint32 for the ground map, so an
     * over-cap cross product would burn multi-GB (N^arity) to build a lane for
     * a rule that never grounded. Silent: the eager path already reported it. */
    bool of = false;
    (void)instance_count(p, r->vars, r->nvars, &of);
    if (of)
        return;

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
    /* #83/#84: the routed lane numerics don't run the per-type response stage
     * yet — a typed world stays on the N=1 step path (correctness first; the
     * lane-side response is #84's remaining slice). */
    if (p->dtype_sort >= 0)
        return;
    /* #87: a stratified world steps one solve per stratum — N=1 only */
    if (p->has_pguards)
        return;

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
        if (p->rules[i].nvars >= 2 &&              /* the join matcher (2+ vars) */
            p->rules[i].ninst > 0)                 /* skip rules that never ground */
            emit_join_family(p, &p->rules[i]);
}

/* ---- symbol/occurrence model (story_model.h) ------------------------ */

/* The model is a first-class compiler output, harvested from the parser's own
 * spanned tables — never a second parse. Names are copied so the model can
 * outlive `syms` and `src`. */
struct story_model {
    story_symbol *syms;  int nsyms,  capsyms;
    story_occ    *occs;  int noccs,  capoccs;
    story_rule   *rules; int nrules, caprules;
};

static char *sm_dup(const char *s)
{
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void sm_add_sym(story_model *m, const char *name, story_sym_kind k,
                       int line, int col, const char *detail)
{
    if (!name || !name[0]) return;             /* anonymous rule / empty label */
    if (m->nsyms == m->capsyms) {
        m->capsyms = m->capsyms ? m->capsyms * 2 : 32;
        m->syms = realloc(m->syms, (size_t)m->capsyms * sizeof *m->syms);
    }
    story_symbol *s = &m->syms[m->nsyms++];
    s->name = sm_dup(name);
    s->kind = k;
    s->line = line; s->col = col; s->len = (int)strlen(name);
    s->detail = sm_dup(detail ? detail : "");
}

/* Bounded string appender for building `detail` signatures. */
static void dapp(char *buf, size_t *off, size_t cap, const char *s)
{
    while (*s && *off + 1 < cap) buf[(*off)++] = *s++;
    buf[*off] = '\0';
}

static void dapp_int(char *buf, size_t *off, size_t cap, long v)
{
    char t[24];
    snprintf(t, sizeof t, "%ld", v);
    dapp(buf, off, cap, t);
}

/* "(sort1, sort2)" from a list of sort-name atoms; nothing when arity 0.
 * INTERN_NONE renders as "int" (a function's `int` arg/return type). */
static void dapp_args(char *buf, size_t *off, size_t cap, parser *p,
                      const uint32_t *argsort, int nargs)
{
    if (nargs <= 0) return;
    dapp(buf, off, cap, "(");
    for (int i = 0; i < nargs; i++) {
        if (i) dapp(buf, off, cap, ", ");
        dapp(buf, off, cap, argsort[i] ? intern_name(p->syms, argsort[i]) : "int");
    }
    dapp(buf, off, cap, ")");
}

/* Concept word + signature for a base/provider fluent: "fluent(actor) : int
 * in 0..40", "provider(actor, actor)", "fluent : {locked, closed, open}". */
static void build_fluent_detail(char *buf, size_t cap, parser *p,
                                const ast_fluent *f, const char *concept)
{
    size_t off = 0; buf[0] = '\0';
    dapp(buf, &off, cap, concept);
    dapp_args(buf, &off, cap, p, f->argsort, f->nargs);
    if (f->is_num) {
        dapp(buf, &off, cap, " : int");
        if (f->has_range && f->rmin_expr < 0 && f->rmax_expr < 0) {
            dapp(buf, &off, cap, " in ");
            dapp_int(buf, &off, cap, f->rmin);
            dapp(buf, &off, cap, "..");
            dapp_int(buf, &off, cap, f->rmax);
        }
    } else if (f->is_mv) {
        dapp(buf, &off, cap, " : {");
        for (int i = 0; i < f->nvalues; i++) {
            if (i) dapp(buf, &off, cap, ", ");
            dapp(buf, &off, cap, intern_name(p->syms, f->values[i]));
        }
        dapp(buf, &off, cap, "}");
    } else if (f->is_cell && f->val_sort) {
        dapp(buf, &off, cap, " : ");
        dapp(buf, &off, cap, intern_name(p->syms, f->val_sort));
    }
}

static void sm_add_occ(story_model *m, const char *name, story_occ_role r,
                       int line, int col, bool neg, int rule)
{
    if (!name || !name[0]) return;
    if (line <= 0) return;                     /* synthetic node, no source span */
    if (m->noccs == m->capoccs) {
        m->capoccs = m->capoccs ? m->capoccs * 2 : 64;
        m->occs = realloc(m->occs, (size_t)m->capoccs * sizeof *m->occs);
    }
    story_occ *o = &m->occs[m->noccs++];
    o->name = sm_dup(name);
    o->role = r;
    o->line = line; o->col = col; o->len = (int)strlen(name);
    o->neg = neg; o->rule = rule;
}

/* A DECL/arg/init/sup occurrence: no rule owner, no meaningful polarity. */
static void sm_add_ref(story_model *m, const char *name, story_occ_role r,
                       int line, int col)
{
    sm_add_occ(m, name, r, line, col, false, -1);
}

/* Harvest one atom of rule `rule` (-1 for non-rule contexts): the predicate
 * occurrence (carrying its `~` polarity) plus its entity/var arguments. Skips
 * synthetic predicate-less atoms (expr guards like `roll(20)+atk >= ac`, whose
 * fluent reads live in expr trees — a later slice). */
static void sm_atom(story_model *m, parser *p, const ast_atom *a,
                    story_occ_role role, int rule)
{
    if (a->pred != INTERN_NONE)
        sm_add_occ(m, intern_name(p->syms, a->pred), role, a->line, a->col,
                   a->neg, rule);
    for (int i = 0; i < a->nargs; i++) {
        if (a->args[i].is_int) continue;
        sm_add_occ(m, intern_name(p->syms, a->args[i].name), STORY_OCC_ARG,
                   a->args[i].line, a->args[i].col, false, rule);
    }
}

static const char *sort_name(parser *p, int sidx)
{
    return (sidx >= 0 && sidx < p->nsorts) ? p->sorts[sidx].name : "?";
}

/* "(sort1, sort2)" from a var-binding list (entities/params carry sort indices,
 * unlike fluent argsorts which are name atoms). */
static void dapp_varsorts(char *buf, size_t *off, size_t cap, parser *p,
                          const var_bind *v, int n)
{
    if (n <= 0) return;
    dapp(buf, off, cap, "(");
    for (int i = 0; i < n; i++) {
        if (i) dapp(buf, off, cap, ", ");
        dapp(buf, off, cap, sort_name(p, v[i].sort));
    }
    dapp(buf, off, cap, ")");
}

static story_model *harvest_model(parser *p)
{
    story_model *m = calloc(1, sizeof *m);
    char det[512];

    /* rules first — story_occ.rule indexes this list, and it must line up with
     * p->rules[i] so a rule's head/body occurrences share one owner id. */
    if (p->nrules > 0) {
        m->rules = calloc((size_t)p->nrules, sizeof *m->rules);
        m->caprules = p->nrules;
        for (int i = 0; i < p->nrules; i++) {
            m->rules[i].label = sm_dup(p->rules[i].label);   /* "" if anonymous */
            m->rules[i].line  = p->rules[i].line;
            m->rules[i].col   = p->rules[i].col;
        }
        m->nrules = p->nrules;
    }

    /* declaration symbols (the document outline) + a DECL occurrence each, so
     * a references query can surface the declaring site alongside the uses. */
    for (int i = 0; i < p->nsorts; i++) {
        if (p->sorts[i].is_enum) continue;         /* #96: the enum symbol covers it */
        bool dom = p->sorts[i].is_domain;
        sm_add_sym(m, p->sorts[i].name, dom ? STORY_SYM_DOMAIN : STORY_SYM_SORT,
                   p->sorts[i].line, p->sorts[i].col, dom ? "domain" : "sort");
        sm_add_ref(m, p->sorts[i].name, STORY_OCC_DECL, p->sorts[i].line, p->sorts[i].col);
    }
    for (int i = 0; i < p->nents; i++) {
        /* #96: skip enum-value pseudo-entities — the enum symbol covers them
         * (robust on failed compiles, where nuserents may not be set yet) */
        if (p->ents[i].sort >= 0 && p->ents[i].sort < p->nsorts &&
            p->sorts[p->ents[i].sort].is_enum)
            continue;
        const char *nm = intern_name(p->syms, p->ents[i].atom);
        size_t off = 0; det[0] = '\0';
        dapp(det, &off, sizeof det, "entity : ");
        dapp(det, &off, sizeof det, sort_name(p, p->ents[i].sort));
        sm_add_sym(m, nm, STORY_SYM_ENTITY, p->ents[i].line, p->ents[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->ents[i].line, p->ents[i].col);
    }
    for (int i = 0; i < p->nfluents; i++) {
        const char *nm = intern_name(p->syms, p->fluents[i].pred);
        build_fluent_detail(det, sizeof det, p, &p->fluents[i], "fluent");
        sm_add_sym(m, nm, STORY_SYM_FLUENT, p->fluents[i].line, p->fluents[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->fluents[i].line, p->fluents[i].col);
    }
    for (int i = 0; i < p->nproviders; i++) {
        const char *nm = intern_name(p->syms, p->providers[i].pred);
        build_fluent_detail(det, sizeof det, p, &p->providers[i], "provider");
        sm_add_sym(m, nm, STORY_SYM_PROVIDER, p->providers[i].line, p->providers[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->providers[i].line, p->providers[i].col);
    }
    for (int i = 0; i < p->nvaluedecls; i++) {     /* derived values (#82) */
        const char *nm = intern_name(p->syms, p->valuedecls[i].pred);
        build_fluent_detail(det, sizeof det, p, &p->valuedecls[i], "value");
        sm_add_sym(m, nm, STORY_SYM_VALUE, p->valuedecls[i].line, p->valuedecls[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->valuedecls[i].line, p->valuedecls[i].col);
    }
    for (int i = 0; i < p->nfunctions; i++) {
        const char *nm = intern_name(p->syms, p->functions[i].name);
        size_t off = 0; det[0] = '\0';
        dapp(det, &off, sizeof det, "function");
        dapp_args(det, &off, sizeof det, p, p->functions[i].argsort, p->functions[i].nargs);
        dapp(det, &off, sizeof det, " : ");
        dapp(det, &off, sizeof det,
             p->functions[i].ret ? intern_name(p->syms, p->functions[i].ret) : "int");
        sm_add_sym(m, nm, STORY_SYM_FUNCTION, p->functions[i].line, p->functions[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->functions[i].line, p->functions[i].col);
    }
    for (int i = 0; i < p->nenums; i++) {
        size_t off = 0; det[0] = '\0';
        dapp(det, &off, sizeof det, "enum : {");
        for (int v = 0; v < p->enums[i].nvalues; v++) {
            if (v) dapp(det, &off, sizeof det, ", ");
            dapp(det, &off, sizeof det, intern_name(p->syms, p->enums[i].values[v]));
        }
        dapp(det, &off, sizeof det, "}");
        sm_add_sym(m, p->enums[i].name, STORY_SYM_ENUM, p->enums[i].line, p->enums[i].col, det);
        sm_add_ref(m, p->enums[i].name, STORY_OCC_DECL, p->enums[i].line, p->enums[i].col);
    }
    for (int i = 0; i < p->nactions; i++) {
        size_t off = 0; det[0] = '\0';
        dapp(det, &off, sizeof det, "action");
        dapp_varsorts(det, &off, sizeof det, p, p->actions[i].vars, p->actions[i].nvars);
        sm_add_sym(m, p->actions[i].name, STORY_SYM_ACTION,
                   p->actions[i].line, p->actions[i].col, det);
    }
    for (int i = 0; i < p->nrules; i++) {
        size_t off = 0; det[0] = '\0';
        dapp(det, &off, sizeof det, "rule");
        dapp_varsorts(det, &off, sizeof det, p, p->rules[i].vars, p->rules[i].nvars);
        sm_add_sym(m, p->rules[i].label, STORY_SYM_RULE,
                   p->rules[i].line, p->rules[i].col, det);
    }

    /* rule heads (conclusions) / bodies / guards — tagged with the rule id */
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        sm_atom(m, p, &r->head, STORY_OCC_HEAD, i);
        for (int b = 0; b < r->nbody; b++)  sm_atom(m, p, &r->body[b],  STORY_OCC_BODY, i);
        for (int g = 0; g < r->nguard; g++) sm_atom(m, p, &r->guard[g], STORY_OCC_BODY, i);
    }
    /* actions: requires (conditions) + effects (writes) — not logic rules */
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        for (int q = 0; q < a->nreq; q++) sm_atom(m, p, &a->requires[q], STORY_OCC_BODY, -1);
        for (int e = 0; e < a->neff; e++) sm_atom(m, p, &a->effects[e],  STORY_OCC_EFFECT, -1);
    }
    for (int i = 0; i < p->ninits; i++)
        sm_atom(m, p, &p->inits[i], STORY_OCC_BODY, -1);
    for (int i = 0; i < p->nsups; i++) {   /* `a > b` — references two rule labels */
        sm_add_ref(m, p->sups[i].a, STORY_OCC_BODY, p->sups[i].aline, p->sups[i].acol);
        sm_add_ref(m, p->sups[i].b, STORY_OCC_BODY, p->sups[i].bline, p->sups[i].bcol);
    }
    return m;
}

const story_symbol *story_model_symbols(const story_model *m, int *n)
{
    if (n) *n = m ? m->nsyms : 0;
    return m ? m->syms : NULL;
}

const story_occ *story_model_occs(const story_model *m, int *n)
{
    if (n) *n = m ? m->noccs : 0;
    return m ? m->occs : NULL;
}

const story_rule *story_model_rules(const story_model *m, int *n)
{
    if (n) *n = m ? m->nrules : 0;
    return m ? m->rules : NULL;
}

void story_model_free(story_model *m)
{
    if (!m) return;
    for (int i = 0; i < m->nsyms; i++) {
        free((char *)m->syms[i].name);
        free((char *)m->syms[i].detail);
    }
    for (int i = 0; i < m->noccs; i++)  free((char *)m->occs[i].name);
    for (int i = 0; i < m->nrules; i++) free((char *)m->rules[i].label);
    free(m->syms);
    free(m->occs);
    free(m->rules);
    free(m);
}

/* `out` (when non-NULL) receives the harvested span model (story_model.h).
 * `mret` (when non-NULL) retains a tick-time matcher plan (#28): matched rules
 * are captured (not eagerly ground) into `*mret`, and the judgment layer is left
 * for the caller's first story_matcher_reground; implies matched grounding. */
static world *compile_impl(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, bool matched,
                           story_model **out, story_matcher **mret)
{
    parser *p = calloc(1, sizeof *p);
    p->dtype_sort = -1;           /* #83: no damage-type enum until an `as` is seen */
    p->ground_matched = matched || mret != NULL;
    p->sparse = mret != NULL;     /* #92: dense universe only for eager +
                                   * compile-time-matched modes */
    p->rules = calloc(MAX_RULES, sizeof *p->rules);
    p->actions = calloc(MAX_ACTIONS, sizeof *p->actions);
    p->binders = calloc(MAX_BINDERS, sizeof *p->binders);
    p->exprs = calloc(MAX_EXPRS, sizeof *p->exprs);
    p->capinits = 64;                              /* grows geometrically as needed */
    p->inits = malloc((size_t)p->capinits * sizeof *p->inits);
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
        case TK_DOMAIN: parse_domain(p); break;
        case TK_ENUM:   parse_enum(p);   break;
        case TK_ENTITY: parse_entity(p); break;
        case TK_STATE:  parse_state(p);  break;
        case TK_PROVIDER: parse_provider(p); break;
        case TK_FUNCTION: parse_function(p); break;
        case TK_VALUE:  parse_value(p);  break;
        case TK_INIT:   parse_init(p);   break;
        case TK_RULE:   parse_rule(p);   break;
        case TK_ACTION: parse_action(p); break;
        case TK_BANDS:  parse_bands(p);  break;
        case TK_IDENT:  parse_sup(p);    break;
        default: {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a declaration "
                 "(scene/sort/domain/enum/entity/state/init/rule/action/bands) "
                 "or a superiority statement, found %s", d);
            break;
        }
        }
        if (p->err_flag) synchronize(p);
        p->ndecls++;
    }

    world *result = NULL;
    story_matcher *m = NULL;
    if (p->nerrors == 0) {
        /* pass 2: semantic analysis, then build-time grounding */
        semantic_pass(p);
        if (p->nerrors == 0) {
            desugar_bands(p);                     /* band ladders → pairwise `>` (§6.2) */
            if (p->dtype_sort >= 0)               /* #83: size the closed type domain
                                                   * before any response registers */
                world_set_dtypes(p->w, p->domain_n[p->dtype_sort]);
            declare_ground_fluents(p);
            ground_inits(p);
            if (p->ground_matched && !mret) build_fact_index(p);
            if (mret) {
                /* tick-time: ground the static (non-matchable) layer, capture the
                 * matchable rules into the retained plan (capture emits no jrules,
                 * so one interleaved pass suffices). The matched layer is
                 * materialized by the caller's first story_matcher_reground — one
                 * code path grounds it, always. */
                m = calloc(1, sizeof *m);
                m->syms = syms;
                m->w = p->w;
                for (int i = 0; i < p->nrules; i++)
                    if (rule_matchable(p, &p->rules[i]))
                        matcher_capture(m, p, &p->rules[i],
                                        rule_island(p, &p->rules[i]));
                    else
                        ground_rule(p, &p->rules[i]);
                world_matched_checkpoint(p->w);       /* boundary: static | matched */
                matcher_capture_schema(m, p);         /* #92: outlive the parser */
            } else {
                for (int i = 0; i < p->nrules; i++) {
                    if (p->ground_matched && rule_matchable(p, &p->rules[i]))
                        ground_rule_matched(p, &p->rules[i]);
                    else
                        ground_rule(p, &p->rules[i]);
                }
            }
            for (int i = 0; i < p->nactions; i++) ground_action(p, &p->actions[i]);
            for (int i = 0; i < p->nsups; i++)    ground_sup(p, &p->sups[i]);
            check_orphans(p);
            /* Skip lanes in tick-time matcher mode: matchable rules aren't ground
             * into the world (only captured), so a judgment lane family would
             * shadow the re-materialized layer with stale, un-re-ground results.
             * lane↔matcher routing is a later slice (#28 router). */
            if (p->nerrors == 0 && !mret) build_lane_families(p);   /* the DoD thesis, 2a */
        }
    }

    if (p->nerrors == 0) { result = p->w; if (mret) *mret = m; }
    else { world_free(p->w); story_matcher_free(m); if (mret) *mret = NULL; }

    /* Harvest the span model before the parser tables are torn down —
     * best-effort, so navigation works even on a file that failed to compile. */
    if (out) *out = harvest_model(p);

    for (int i = 0; i < p->nrules; i++) free(p->rules[i].insts);
    free(p->rules);
    free(p->actions);
    free(p->binders);
    free(p->exprs);
    free(p->inits);
    free(p->ents);
    free(p->ent_of);
    free(p->ent_pos);
    for (int s = 0; s < p->nsorts; s++) free(p->domain_ents[s]);
    if (p->fidx) factindex_free(p->fidx);
    free(p);
    return result;
}

world *story_compile(const char *src, const char *srcname, intern *syms,
                     story_diags *diags)
{
    return compile_impl(src, srcname, syms, diags, false, NULL, NULL);
}

world *story_compile_model(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, story_model **out)
{
    return compile_impl(src, srcname, syms, diags, false, out, NULL);
}

/* Same grammar and world, but ground rules in the join-matcher kernel via the
 * fact-store extension index (§5.2 item 4, #28) where eligible. Verdicts and
 * why-traces are identical to story_compile (pinned by test_matcher). */
world *story_compile_matched(const char *src, const char *srcname, intern *syms,
                             story_diags *diags)
{
    return compile_impl(src, srcname, syms, diags, true, NULL, NULL);
}

/* Tick-time matcher: compile, retain the matchable-rule plan, and materialize the
 * initial matched layer against the init facts (the same reground path used every
 * tick — so the initial layer and every re-grounding go through one code path). */
/* The world's auto re-ground hook (#45): state calls this before a solve when the
 * matched layer is stale. ctx is the matcher; w == m->w. */
static void matcher_reground_thunk(void *ctx, world *w)
{
    (void)w;
    story_matcher_reground((story_matcher *)ctx);
}

story_matcher *story_compile_matcher(const char *src, const char *srcname,
                                     intern *syms, story_diags *diags, world **out)
{
    story_matcher *m = NULL;
    world *w = compile_impl(src, srcname, syms, diags, true, NULL, &m);
    if (out) *out = w;
    if (!w) return NULL;                 /* compile failed; m already NULL/freed */
    /* Auto re-ground: the world refreshes the matched layer itself before each
     * solve (#45). The host no longer calls story_matcher_reground; the first
     * world_query / world_step builds the initial layer. */
    world_set_reground_fn(w, matcher_reground_thunk, m);
    world_set_materialize_fn(w, matcher_materialize_thunk, m);   /* islands (#80) */
    world_set_schema_fn(w, matcher_schema_thunk, m);   /* sparse universe (#92) */
    return m;
}
