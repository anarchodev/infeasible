#include "lang/story.h"
#include "lang/lexer.h"
#include "state/factindex.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
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
#define MAX_VDEFS      272     /* definitions per value: the layer chain plus its
                                * lookup-table ROWS (#94). Rows are not layers —
                                * each speaks for one instance and none blends
                                * with another — so they are bounded by how many
                                * instances a value has, not by the register
                                * depth the chain costs. */
#define MAX_FLUENTS    256     /* fluent *predicate* schemas */
#define MAX_PREDS      512     /* predicate registry (fluents + heads) */
#define MAX_RULES      256
#define MAX_ACTIONS    128
#define MAX_SUPS       256
#define MAX_INITS      (1 << 24)   /* runaway ceiling only — the list grows to fit */
#define MAX_GROUND     256     /* ground atom name buffer */
#define MAX_EXPRS      4096    /* effect-expression AST node pool */
#define MAX_CODE       128     /* VM bytecode per ground effect (partial-value
 * chains + the #116 definedness epilogue emit ~11-15 ins per layer; overflow
 * is a located error via parser.code_of — never silent truncation) */
#define MAX_ENUMS      16      /* named value domains (`enum school { … }`, §13) */
#define MAX_WHEN       8       /* conjuncts in a binder `where` / item `when` */
#define MAX_ITEMS      8       /* effect items in one `for each` block */
#define MAX_ACT_BINDERS 4      /* `for each` binders per action */
#define MAX_BINDERS    64      /* binder pool across the whole file */
#define MAX_INSTANCES  (1 << 20)   /* per-rule grounding blow-up guard */
#define CARD_WARN      100000      /* cross-product cardinality warning (§5.2) */
#define MAX_LADDERS    16          /* priority ladders (`bands …`, §6.2) */
#define MAX_BANDS      16          /* bands per ladder */
#define MAX_EXCLS      32          /* `exclusive` groups per file (#159) */
#define MAX_EXCL_MEMBERS 16        /* member actions per group — a per-actor
                                    * turn protocol names every action a unit
                                    * may take, so this scales with the verb
                                    * count, not with the group's arity */
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
    bool        is_kinddef;    /* head only: `kind k(S) = expr` (#82 roll kinds) —
                                * a modifier expanded onto every value of kind k;
                                * pred = the kind name, args[0] = the subject var */
    bool        is_member;     /* `X in { v, … }` finite-domain membership (#95):
                                * args[0] = the element var, mem_ix/mem_n index the
                                * mempool, `neg` = `not in`. A static grounding
                                * filter — never emitted, never in the fixpoint. */
    int         mem_ix, mem_n;
    bool        is_defined;    /* `defined v(args)` (#116): the value's
                                * definedness as a first-class body atom — the
                                * disjunction of its prior-free layer markers.
                                * pred/args name the VALUE; body position only. */
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
    EX_ENT,   /* an ENTITY term (#258): a rule variable or a named entity read in
               * an expression, so a value provider can be called with one —
               * `chebyshev(A, B)`. `pred` holds the variable name or entity
               * atom, `konst` the sort index. Lowers to EXPR_CONST with the
               * entity's interned atom, resolved through the binding: the
               * grounder already substitutes an entity per role, so it is a
               * compile-time constant, exactly as #226's constant argument is.
               * An entity is a HANDLE — require_int keeps it out of arithmetic. */
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
    bool     nprimed;         /* EX_LOAD with postfix ' (#84): read the NEXT value
                               * a lower stratum committed this tick */
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
    int       srule_lo, srule_hi; /* the contiguous world srule handle range this
                                   * action's instances ground to (#121 coverage) */
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

/* An `exclusive` group (#159, §5.13): a checked action-exclusivity PROTOCOL.
 * Members are action templates; named variables form the group's KEY —
 * matched by name across members, `_` positions never constrain — and a step
 * may contain at most one member instance per key tuple. The safety that
 * used to be an unverifiable host promise ("we never co-submit east and
 * west for one guard") becomes a declaration the compiler consumes (#98
 * treats covered pairs as exclusive) and the engine checks (world_step
 * rejects a violating action set pre-solve, host-protocol class; the §6.3
 * typed binding retires the check for bound hosts). */
typedef struct {
    struct {
        char     action[MAX_NAME];
        uint32_t vars[MAX_ARGS];      /* key var name per position, 0 = `_` */
        uint32_t typenames[MAX_ARGS]; /* optional `: sort` annotation, 0 = none */
        int      nargs;
        int      line, col;
    } mem[MAX_EXCL_MEMBERS];
    int nmem;
    int line, col;
} ast_excl;

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
    bool     is_split;            /* `split` (#121): per-value step-schema
                                   * specialization hint; MV arity-0 only */
    bool     is_kindpred;         /* #124: a boolean `value` decl with one
                                   * `value`-sorted argument — a kind predicate,
                                   * populated by `fact`s, consumed at build */
    int      line, col;
} ast_fluent;

/* #124: a kind membership fact — `fact save(spell_save, wis)`. Args are bare
 * symbols, vocabulary-checked in the semantic pass (the value-sorted position
 * names a declared value; enum/entity positions name members/entities). */
typedef struct {
    uint32_t pred;
    uint32_t args[MAX_ARGS];
    int      nargs;
    int      line, col;
} ast_kfact;

#define MAX_KFACTS 4096               /* generous; overflow is a loud error */

/* var_bind.sort sentinel: the #124 `value` meta-sort (never a real sort index;
 * a meta-sorted binder is consumed by the functor expansion, or is an error). */
#define SORT_METAVALUE (-1000000)

/* pred_info.argsort sentinel: a `value`-sorted argument position of a kind
 * predicate (#124/#143 — a kind may have several: link predicates like
 * `dmg_of(value, value)` join value variables across positions). Distinct
 * from -1 so a failed sort decode is never mistaken for a value position. */
#define KARG_VALUE (-1000001)

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
    bool     is_emit;             /* #11: a burst cue (§12) — an effect head the
                                   * step FIRES and nothing stores or reads */
    bool     is_value;            /* an engine-derived value (#82): defined by a
                                   * rule, inlined at read sites, never stored */
    bool     is_kindpred;         /* #124: a kind predicate — build-time-only,
                                   * populated by `fact`s, erased at grounding */
    int      kval_pos;            /* its `value`-sorted argument position */
    int      headsort[MAX_ARGS];  /* #205: a JUDGMENT's argument sorts, inferred
                                   * from the rules concluding it (a head has no
                                   * declaration site); -1 = not yet known */
    int      headrule[MAX_ARGS];  /* the rule that fixed each one, for the
                                   * diagnostic that names the other site */
} pred_info;

/* #217: one deferred sort check for an argument READING a judgment. A judgment
 * has no declaration, so its signature is not settled until every rule that
 * concludes it has been seen — which is after the pass that walks the reads.
 * What is recorded is the sort the reading scope already resolved, never the
 * scope itself: the binding rules (which variables are in scope where, domain
 * parameters, binder scopes) stay in one place and this pass cannot drift from
 * them. */
typedef struct {
    int      pred;                /* index into parser.preds (a fixed array) */
    int      arg;                 /* 0-based argument position */
    int      got;                 /* the sort the argument resolved to */
    int      line, col;
} head_read;

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
    /* `is_union` (#231): a declared COVER over other sorts — `sort thing union
     * actor, item`. Not `<:` inheritance: a cover admits its members' entities
     * and adds none of its own, so "everything placed on the map" is one
     * predicate instead of one per sort. Sealed at world-build, and members are
     * base sorts, so an entity is in exactly one member and a cover's entities
     * are the disjoint concatenation of theirs — which is what lets a cover
     * position be `off[m] + ent_pos`, leaving `ent_pos` a single int per
     * entity rather than one per (entity, sort). */
    struct { char name[MAX_NAME]; int line, col; bool is_domain; bool is_enum;
             bool is_union; int nmem; int mem[MAX_SORTS]; int off[MAX_SORTS]; }
        sorts[MAX_SORTS];
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
    ast_fluent  emits[MAX_FLUENTS];       /* burst cues (#11, §12): one-shot events
                                           * a step fires at the presentation
                                           * client — write-only vocabulary */
    int nemits;
    ast_function functions[MAX_FLUENTS];  /* value-returning fn providers (§5.6) */
    int nfunctions;
    ast_fluent  valuedecls[MAX_FLUENTS];  /* engine-derived values (#82): `value v(…) : int` */
    ast_fluent  kindpreds[MAX_FLUENTS];   /* #124 kind predicates: boolean `value`
                                           * decls with one `value`-sorted arg */
    int nkindpreds;
    ast_kfact   kfacts[MAX_KFACTS];       /* #124 membership facts */
    int nkfacts;
    dl_theory  *kth;                      /* #125: the kind-stratum theory,   */
    dl_result  *kres;                     /* solved at build; NULL = no kinds */
    uint32_t   *katoms; int *katom_rule;  /* ground kind atoms (sweep list) + */
    int nkatoms, capkatoms;               /* first concluding rule ix or -1   */
    const char *kwhy_query;               /* #125 build-time why: query atom  */
    FILE       *kwhy_out;                 /* name + sink, or NULL             */
    uint32_t    metaval;                  /* the interned "value" meta-sort name —
                                           * LAZY (metaval()): interning it eagerly
                                           * would shift every later atom id and so
                                           * every §5.10 roll-site key in worlds
                                           * that never use kinds. 0 = not yet
                                           * interned; compares false against any
                                           * real atom (INTERN_NONE is 0). */
    int nvaluedecls;
    bool has_pguards;             /* #87: any primed numeric guard — strata exist,
                                   * step lanes bail, world steps N=1 */
    uint32_t split_pred;          /* the ONE `split` fluent (#121), 0 = none */
    int value_def[MAX_FLUENTS];           /* per value: the BASE definition (the one
                                           * unconditional rule), -1 = none yet */
    int vdefs[MAX_FLUENTS][MAX_VDEFS];        /* all defs, declaration order (#82/#94) */
    int nvdefs[MAX_FLUENTS];
    int value_layers[MAX_FLUENTS][MAX_LAYERS];/* guarded defs, CHAIN order (bottom->top) */
    int value_nlayers[MAX_FLUENTS];
    /* Lookup-table ROWS: unconditional definitions that pin a constant head
     * argument, so each speaks for one ground instance. The base is the
     * catch-all beneath them, if there is one. */
    int value_rows[MAX_FLUENTS][MAX_VDEFS];
    int value_nrows[MAX_FLUENTS];
    int *vmark_of; uint32_t vmark_cap;    /* marker atom -> grounded flag (dedup) */
    bool in_valuedef_expr;                /* `prior` legality context */
    bool in_ramif_eff;                    /* primed-read legality context (#84) */
    int vdepth;                           /* value-inline recursion depth (cycle backstop) */
    bool value_partial[MAX_FLUENTS];      /* #116: no unconditional base — partiality
                                           * is INFERRED (decided 2026-07-30); guards
                                           * over it are tri-valued, arithmetic reads
                                           * need the static safety rule */
    int *vdefd_of; uint32_t vdefd_cap;    /* `defined(v(…))` atom -> grounded (dedup) */
    long encl_marks[2 * MAX_ARGS];        /* enclosing layer-marker EXPR_TEST args
                                           * during nested value inlining (#116): a
                                           * nested partial read's REQDEF is waived
                                           * when any enclosing layer did not fire */
    int nencl;
    bool code_of;                         /* bytecode emission overflowed MAX_CODE —
                                           * checked (and reset) where code is
                                           * consumed; a located error, never silent */
    ast_rule   *rules;            /* heap; MAX_RULES */
    int nrules;
    ast_action *actions;          /* heap; MAX_ACTIONS */
    int nactions;
    ast_binder *binders;          /* heap; MAX_BINDERS — the `for each` pool */
    int nbinders;
    ast_excl    excls[MAX_EXCLS]; /* `exclusive` groups (#159) */
    int nexcls;
    enum_dom enums[MAX_ENUMS];    /* named value domains (§13) */
    int nenums;
    ast_sup     sups[MAX_SUPS];
    int nsups;
    ast_ladder  ladders[MAX_LADDERS];   /* priority ladders (`bands …`, §6.2) */
    int nladders;
    ast_atom   *inits;                  /* grown geometrically (§ loud failures) */
    int ninits, capinits;

    head_read  *hreads;                 /* #217: judgment reads awaiting their
                                         * predicate's signature; grown to fit */
    int nhreads, caphreads;

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
    int ln = p->cur.line;
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
        p->sorts[p->nsorts].is_union = false;
        p->sorts[p->nsorts].nmem = 0;
        int self = p->nsorts;
        ln = p->cur.line;
        p->nsorts++;
        advance(p);
        /* `sort thing union actor, item` (#231) — a declared COVER. Contextual,
         * like `fact` and `exclusive`, so `union` stays a usable identifier.
         * Members may be declared later in the file, so the names are stashed
         * encoded and resolved once every sort is known. */
        if (p->cur.kind == TK_IDENT && ident_is(p->cur, "union")) {
            advance(p);
            do {
                if (p->cur.kind != TK_IDENT) {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected a member sort name after `union`, found %s", d);
                    return;
                }
                if (p->sorts[self].nmem >= MAX_SORTS) {
                    fail(p, p->cur.line, p->cur.col, "too many union members");
                    return;
                }
                p->sorts[self].mem[p->sorts[self].nmem++] =
                    -(int)intern_tok(p, p->cur) - 2;
                advance(p);
            } while (p->cur.kind == TK_COMMA && (advance(p), true));
            p->sorts[self].is_union = true;
            ln = p->cur.line;
            continue;                               /* a cover ends its clause */
        }
        if (p->cur.kind == TK_COMMA) advance(p);    /* optional separator */
        /* The ungrouped form is ONE line: a name on the next line is the next
         * declaration, not another sort. Without this, `sort actor` followed by
         * a contextual-keyword declaration (`emit`, `fact`, `exclusive`) ate
         * the keyword as a sort name and reported the error a token later. */
    } while (p->cur.kind == TK_IDENT && (grouped || p->cur.line == ln));
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
    int ln = p->cur.line;
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
        ln = p->cur.line;
        p->nsorts++;
        advance(p);
        if (p->cur.kind == TK_COMMA) advance(p);    /* optional separator */
    } while (p->cur.kind == TK_IDENT && (grouped || p->cur.line == ln));
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
        if (p->cur.kind != TK_LPAREN && p->cur.kind == TK_PRIME) {
            p->exprs[n].nprimed = true;            /* arity-0 primed read (#84) */
            advance(p);
            return n;
        }
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
        if (p->cur.kind == TK_PRIME) {             /* primed numeric read (#84) */
            p->exprs[n].nprimed = true;
            advance(p);
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

/* Is `name` a numeric fluent DECLARED so far in this parse? (#130 — the same
 * declare-before-use discipline find_value already relies on.) */
static bool is_declared_num(parser *p, uint32_t name)
{
    for (int i = 0; i < p->nfluents; i++)
        if (p->fluents[i].pred == name && p->fluents[i].is_num)
            return true;
    return false;
}

/* A conjunct led by a numeric fluent read is a plain comparison guard
 * (`hp(X) <= 0` — a stored threshold the loader asserts closed-world) or the
 * start of an EXPRESSION guard (`atk_die(A) + atk_mod(A) >= ac(T)`). Peek past
 * the read to tell them apart, on a COPY of the lexer so nothing is consumed:
 *
 *   read <arith> …            -> expression   (`+ - * /`)
 *   read <cmp> INT <end>      -> plain guard  (today's path, unchanged)
 *   read <cmp> anything else  -> expression   (a value, a fluent, a call)
 *
 * Keeping the INT case on the plain path matters: that is the form the primed
 * dying trigger (§5.8 #87) and the threshold-harvesting stratifier read. */
static bool numread_starts_expr(parser *p)
{
    lexer lx = p->lx;                 /* positioned just after p->cur */
    token t = lexer_next(&lx);
    if (t.kind == TK_LPAREN) {        /* skip the argument list */
        int depth = 1;
        while (depth > 0) {
            t = lexer_next(&lx);
            if (t.kind == TK_LPAREN) depth++;
            else if (t.kind == TK_RPAREN) depth--;
            else if (t.kind == TK_EOF || t.kind == TK_ERROR) return false;
        }
        t = lexer_next(&lx);
    }
    if (t.kind == TK_PLUS || t.kind == TK_MINUS || t.kind == TK_STAR ||
        t.kind == TK_SLASH)
        return true;
    if (t.kind != TK_LE && t.kind != TK_LT && t.kind != TK_GE &&
        t.kind != TK_GT && t.kind != TK_EQ)
        return false;                 /* a prime, a `&`, an arrow: not ours */
    t = lexer_next(&lx);              /* the right-hand side */
    if (t.kind == TK_MINUS) t = lexer_next(&lx);       /* a negative threshold */
    if (t.kind != TK_INT)
        return true;                  /* compared against something computed */
    t = lexer_next(&lx);              /* `hp <= 4 + 1` is still an expression */
    return t.kind == TK_PLUS || t.kind == TK_MINUS || t.kind == TK_STAR ||
           t.kind == TK_SLASH;
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
     * roll(20,2)) + atk >= ac`, and `atk_roll(A,T) + atk(A) >= ac(T)`.
     *
     * A conjunct led by a numeric FLUENT read joins them when it continues into
     * arithmetic or compares against something computed (#130) — `atk_die(A) +
     * atk_mod(A) >= ac(T)` is the natural spelling, and demanding the leading
     * paren it used to need produced a diagnostic about rule arrows. */
    if (ident_is(p->cur, "roll") || ident_is(p->cur, "min") || ident_is(p->cur, "max") ||
        ident_is(p->cur, "divup") ||
        (p->cur.kind == TK_IDENT && find_value(p, intern_tok(p, p->cur)) >= 0) ||
        (p->cur.kind == TK_IDENT && find_function(p, intern_tok(p, p->cur)) >= 0) ||
                                    /* #258: a value provider is a MEASUREMENT, and
                                     * `chebyshev(A,B) <= 3` is how a story rules on
                                     * one — so a declared function leads a guard the
                                     * way a declared value does */
        (p->cur.kind == TK_IDENT && is_declared_num(p, intern_tok(p, p->cur)) &&
         numread_starts_expr(p)) ||
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
    /* `defined v(args)` (#116): a partial value's definedness as a body atom —
     * contextual (like `prior`/`test`), so `defined` stays a legal atom name
     * when no identifier follows. */
    if (ident_is(id, "defined") && p->cur.kind == TK_IDENT) {
        out->is_defined = true;
        out->pred = intern_tok(p, p->cur);
        advance(p);
    }
    /* set membership: `T in P` / `T not in P` over a `set of` param — the
     * leading id is the element var, P the set (a host-answered provider
     * relation, §5.6/§13). Lowers to a read of P(T): `not in` negates it. */
    if (!out->is_defined && (p->cur.kind == TK_IN || ident_is(p->cur, "not"))) {
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
        if (p->cur.kind == TK_AS) {
            /* the `as <type>` typed-contribution surface went with the
             * configured response stage (#84) — keep the refusal located
             * and pointed at the modeled idiom */
            fail(p, p->cur.line, p->cur.col,
                 "`as` typed contributions were removed (#84) — author typed "
                 "damage in the modeled form: accumulate into a per-type "
                 "transient (`incoming_fire(T)' += …`), write the response as "
                 "ordinary rules, and commit with one effect (DESIGN.md §5.8)");
            return false;
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

/* Optional `split` (#121) after a finite MV domain — a contextual keyword like
 * `merge`, a compilation hint with zero semantic content: the step schema
 * specializes per value of this fluent. One per world; the schema switches on
 * ONE mode value, so the fluent must be arity-0; the world-side liveness mask
 * is 31 bits (generous, and loud rather than silently capped). */
static bool parse_split_opt(parser *p, ast_fluent *f)
{
    if (!ident_is(p->cur, "split"))
        return true;
    token t = p->cur;
    advance(p);
    if (f->nargs > 0) {
        fail(p, t.line, t.col,
             "`split` needs an arity-0 fluent — the step schema specializes on "
             "one mode value (phase, day/night), not one per entity");
        return false;
    }
    if (f->nvalues > 31) {
        fail(p, t.line, t.col,
             "`split` domain too large: %d values (max 31)", f->nvalues);
        return false;
    }
    if (p->split_pred) {
        fail(p, t.line, t.col,
             "duplicate `split` — one split fluent per world (#121); '%s' "
             "already carries it",
             intern_name(p->syms, p->split_pred));
        return false;
    }
    f->is_split = true;
    p->split_pred = f->pred;
    return true;
}

/* The "value" meta-sort atom, interned on FIRST use (see the field note). */
static uint32_t metaval(parser *p)
{
    if (!p->metaval)
        p->metaval = intern_id(p->syms, "value");
    return p->metaval;
}

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
            /* `value` (a keyword) is legal as an argument sort: the #124
             * meta-sort whose elements are the declared value symbols — the
             * mark of a kind predicate. parse_value validates placement. */
            if (p->cur.kind != TK_IDENT && p->cur.kind != TK_VALUE) {
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
            f->argsort[f->nargs++] = p->cur.kind == TK_VALUE
                                     ? metaval(p) : intern_tok(p, p->cur);
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
            if (ident_is(p->cur, "kind")) {        /* removed surface (#124) */
                fail(p, p->cur.line, p->cur.col,
                     "the `kind` keyword is gone (#124) — a kind is facts: "
                     "declare `value <kind>(value, …)`, assert membership with "
                     "`fact <kind>(%s, …)`, and select in the modifier body "
                     "(`<kind>(V, …)` with `V : value`, head `V(A) = …`)",
                     intern_name(p->syms, f->pred));
                return false;
            }
            if (ident_is(p->cur, "split")) {
                fail(p, p->cur.line, p->cur.col,
                     "`split` needs a finite value domain (`: { … }` or an "
                     "enum) — a numeric fluent has no per-value schemas (#121)");
                return false;
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
                return parse_split_opt(p, f);      /* optional `split` (#121) */
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
        if (!parse_split_opt(p, f))               /* optional `split` (#121) */
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

/* emit := 'emit' ( edecl | '(' edecl* ')' ); edecl := IDENT [ '(' sort,… ')' ]
 *
 * A burst cue (#11, DESIGN.md §12): a one-shot event a step fires at the
 * presentation client — a hit spark, a "resisted!" — with no lasting state.
 * Declared like a boolean state predicate and with no value type: an emission
 * carries only its arguments, because everything numeric about the tick is
 * already in the step's delta/receipt. `emit` is contextual (like `fact`), so
 * a fluent named `emit` stays legal; a superiority statement whose left label
 * is literally `emit` misparses here with a located error — the same trade the
 * other contextual keywords take. */
static void parse_emit(parser *p)
{
    advance(p);                                    /* 'emit' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a cue name, found %s", d);
            return;
        }
        if (p->nemits >= MAX_FLUENTS) {
            fail(p, p->cur.line, p->cur.col, "too many emissions (max %d)",
                 MAX_FLUENTS);
            return;
        }
        ast_fluent tmp;
        if (!parse_fdecl(p, &tmp)) return;
        if (tmp.is_num || tmp.is_mv || tmp.is_cell) {
            fail(p, tmp.line, tmp.col,
                 "an emission has no value type — a cue is fired, not stored "
                 "(#11); put the number in the fluent the step also writes, "
                 "and read it from the delta");
            return;
        }
        p->emits[p->nemits++] = tmp;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* provider := 'provider' ( pdecl | '(' pdecl* ')' ); pdecl := IDENT '(' sort,… ')'
 * A computed relation (§5.6), host-answered — like a boolean fluent decl but with
 * no value type. */
/* provider := 'provider' ( fdecl | '(' fdecl* ')' ) — the host-answered
 * declarations, unified on RETURN TYPE exactly like `state` (#93):
 *
 *     provider adjacent(actor, actor)          -- no return type: a boolean
 *                                              -- RELATION, ground atoms
 *                                              -- registered and loaded as
 *                                              -- closed-world facts
 *     provider neighbor(cell, dir) : cell      -- return type: a value
 *                                              -- FUNCTION, called from the
 *                                              -- effect VM (EXPR_CALL),
 *                                              -- never grounded
 *
 * The statable rule the split keywords obscured: HAS A RETURN TYPE ⇒ CALLED,
 * NOT GROUNDED. `function` remains a spelling alias for the typed form. */
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
        ast_fluent tmp;
        if (!parse_fdecl(p, &tmp)) return;
        /* `: sortname` parses as a sort-valued functional type (is_mv with a
         * val_sort); an inline `{ … }` or a named enum has no callable type */
        bool sortval = tmp.is_mv && tmp.val_sort != 0;
        if (tmp.is_mv && !sortval) {
            fail(p, tmp.line, tmp.col,
                 "a provider returns `int` or a declared sort/domain — an "
                 "inline or enum domain doesn't name a callable type (#93)");
            return;
        }
        if (tmp.has_range) {
            fail(p, tmp.line, tmp.col,
                 "a provider result has no clamp range — ranges clamp stored "
                 "state (§5.8); the host computes whatever it computes");
            return;
        }
        if (tmp.is_num || tmp.is_cell || sortval) {
            /* return type ⇒ a value FUNCTION (#93): same registration as the
             * `function` keyword — called from the effect VM, never grounded */
            if (p->nfunctions >= MAX_FLUENTS) {
                fail(p, tmp.line, tmp.col, "too many functions (max %d)",
                     MAX_FLUENTS);
                return;
            }
            ast_function *fn = &p->functions[p->nfunctions];
            memset(fn, 0, sizeof *fn);
            fn->name = tmp.pred;
            fn->nargs = tmp.nargs;
            for (int k = 0; k < tmp.nargs; k++)
                fn->argsort[k] = ident_atom_is(p, tmp.argsort[k], "int")
                                 ? INTERN_NONE : tmp.argsort[k];
            fn->ret = (tmp.is_num && !tmp.is_cell) ? INTERN_NONE : tmp.val_sort;
            fn->line = tmp.line;
            fn->col = tmp.col;
            p->nfunctions++;
        } else {
            if (p->nproviders >= MAX_FLUENTS) {
                fail(p, tmp.line, tmp.col, "too many providers (max %d)",
                     MAX_FLUENTS);
                return;
            }
            p->providers[p->nproviders++] = tmp;
        }
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
        int nmeta = 0;
        for (int k = 0; k < v->nargs; k++)
            if (v->argsort[k] == p->metaval) nmeta++;
        if (!v->is_num && !v->is_mv && !v->is_cell && nmeta >= 1) {
            /* a KIND PREDICATE (#124): boolean, with one or more
             * `value`-sorted arguments (#143 — several = a LINK predicate,
             * joining value variables); populated by `fact`s, selected in
             * modifier bodies, erased at build — never a runtime value */
            v->is_kindpred = true;
            if (p->nkindpreds >= MAX_FLUENTS) {
                fail(p, v->line, v->col, "too many kind predicates (max %d)",
                     MAX_FLUENTS);
                return;
            }
            p->kindpreds[p->nkindpreds++] = *v;
            continue;
        }
        if (nmeta >= 1) {
            fail(p, v->line, v->col,
                 "a `value`-sorted argument marks a KIND predicate, which is "
                 "boolean — drop the `: int` on '%s', or drop the `value` arg",
                 intern_name(p->syms, v->pred));
            return;
        }
        if (!v->is_num || v->is_mv || v->is_cell) {
            fail(p, v->line, v->col,
                 "a value needs a return type (only `: int` in this slice, "
                 "#82) — or a `value`-sorted argument to be a kind predicate "
                 "(#124)");
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

/* fact := 'fact' ( katom | '(' katom* ')' ) — #124 kind membership. Contextual
 * keyword (like `merge`): a rule label literally named `fact` in a superiority
 * line misparses here with a located error, same trade the other contextual
 * keywords make. Args are bare symbols, vocabulary-checked in the semantic
 * pass against the kind predicate's declared argument sorts. */
static void parse_fact(parser *p)
{
    advance(p);                                    /* 'fact' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }
    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a kind predicate name, found %s", d);
            return;
        }
        if (p->nkfacts >= MAX_KFACTS) {
            fail(p, p->cur.line, p->cur.col,
                 "too many facts (max %d) — raise MAX_KFACTS", MAX_KFACTS);
            return;
        }
        ast_kfact *kf = &p->kfacts[p->nkfacts];
        memset(kf, 0, sizeof *kf);
        kf->pred = intern_tok(p, p->cur);
        kf->line = p->cur.line;
        kf->col = p->cur.col;
        advance(p);
        if (!expect(p, TK_LPAREN)) return;
        for (;;) {
            if (p->cur.kind != TK_IDENT) {
                char d[64]; tok_desc(p->cur, d, sizeof d);
                fail(p, p->cur.line, p->cur.col,
                     "expected a symbol (a value, enum member, or entity), "
                     "found %s", d);
                return;
            }
            if (kf->nargs >= MAX_ARGS) {
                fail(p, p->cur.line, p->cur.col,
                     "too many fact arguments (max %d)", MAX_ARGS);
                return;
            }
            kf->args[kf->nargs++] = intern_tok(p, p->cur);
            advance(p);
            if (p->cur.kind == TK_COMMA) { advance(p); continue; }
            break;
        }
        if (!expect(p, TK_RPAREN)) return;
        p->nkfacts++;
    } while (grouped && p->cur.kind == TK_IDENT);
    if (grouped && !expect(p, TK_RPAREN)) return;
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
        if (p->cur.kind != TK_IDENT && p->cur.kind != TK_VALUE) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a sort name, found %s", d);
            return false;
        }
        var_bind *v = &vars[*nvars];
        v->name = intern_tok(p, nm);
        v->line = nm.line;
        v->col = nm.col;
        /* encode the sort name atom for resolution in the semantic pass;
         * `V : value` (a keyword) binds over the #124 meta-sort */
        v->sort = -(int)(p->cur.kind == TK_VALUE ? metaval(p)
                                                 : intern_tok(p, p->cur)) - 2;
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

    if (ident_is(p->cur, "kind")) {                /* removed surface (#124) */
        fail(p, p->cur.line, p->cur.col,
             "`=> kind k(A) = …` is gone (#124) — bind the kind in the body: "
             "add a parameter `V : value`, select with `k(V, …)` in the body, "
             "and write the head as `V(A) = …`");
        return;
    }

    /* Functor-position modifier head (#124): `V(A[, T]) = expr` where V is a
     * rule parameter over the `value` meta-sort. Looks higher-order, grounds
     * first-order: the semantic pass matches the body's kind atoms against
     * the `fact` set and flattens one layer per member value (HiLog move).
     * Detected here so parse_atom's MV machinery never sees the variable. */
    if (p->cur.kind == TK_IDENT) {
        uint32_t hv = intern_tok(p, p->cur);
        for (int k = 0; k < r->nvars; k++) {
            if (r->vars[k].name != hv)
                continue;
            if (r->vars[k].sort != -(int)p->metaval - 2) {
                /* a rule parameter can never head a conclusion — the only
                 * variable legal in functor position is a `value`-sorted one */
                fail(p, p->cur.line, p->cur.col,
                     "'%s' is a rule parameter — only a `value`-sorted "
                     "parameter may stand in functor position (`V(A) = …` "
                     "with `V : value`)", intern_name(p->syms, hv));
                return;
            }
            token vt = p->cur;
            advance(p);
            memset(&r->head, 0, sizeof r->head);
            r->head.is_kinddef = true;             /* rides #115's guards/expansion */
            r->head.pred = hv;                     /* the functor VARIABLE */
            r->head.line = vt.line;
            r->head.col = vt.col;
            if (!expect(p, TK_LPAREN)) return;
            for (;;) {
                if (p->cur.kind != TK_IDENT) {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected a subject variable in the functor head, "
                         "found %s", d);
                    return;
                }
                if (r->head.nargs >= MAX_ARGS) {
                    fail(p, p->cur.line, p->cur.col,
                         "too many subject variables (max %d)", MAX_ARGS);
                    return;
                }
                r->head.args[r->head.nargs].name = intern_tok(p, p->cur);
                r->head.args[r->head.nargs].line = p->cur.line;
                r->head.args[r->head.nargs].col = p->cur.col;
                r->head.nargs++;
                advance(p);
                if (p->cur.kind == TK_COMMA) { advance(p); continue; }
                break;
            }
            if (!expect(p, TK_RPAREN)) return;
            if (!expect(p, TK_EQ)) return;
            int e = parse_expr(p);
            if (e < 0) return;
            r->head.lhs_root = e;
            p->nrules++;
            return;
        }
    }

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

/* excl := 'exclusive' eitem (',' eitem)*                       (#159)
 * eitem := IDENT [ '(' earg (',' earg)* ')' ]
 * earg  := '_' | IDENT [ ':' IDENT ]
 * `exclusive` is contextual (like `fact`): a declaration-position identifier. */
static void parse_exclusive(parser *p)
{
    token kw = p->cur;
    advance(p);                                    /* 'exclusive' */
    if (p->nexcls >= MAX_EXCLS) {
        fail(p, kw.line, kw.col, "too many `exclusive` groups (max %d)",
             MAX_EXCLS);
        return;
    }
    ast_excl *x = &p->excls[p->nexcls];
    memset(x, 0, sizeof *x);
    x->line = kw.line;
    x->col = kw.col;
    for (;;) {
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected an action name in an `exclusive` group, found %s", d);
            return;
        }
        if (x->nmem >= MAX_EXCL_MEMBERS) {
            fail(p, p->cur.line, p->cur.col,
                 "too many members in one `exclusive` group (max %d)",
                 MAX_EXCL_MEMBERS);
            return;
        }
        copy_ident(x->mem[x->nmem].action, MAX_NAME, p->cur);
        x->mem[x->nmem].line = p->cur.line;
        x->mem[x->nmem].col = p->cur.col;
        advance(p);
        if (p->cur.kind == TK_LPAREN) {
            advance(p);
            for (;;) {
                if (p->cur.kind != TK_IDENT) {
                    char d[64]; tok_desc(p->cur, d, sizeof d);
                    fail(p, p->cur.line, p->cur.col,
                         "expected a key variable or `_` in an `exclusive` "
                         "member, found %s", d);
                    return;
                }
                if (x->mem[x->nmem].nargs >= MAX_ARGS) {
                    fail(p, p->cur.line, p->cur.col,
                         "too many arguments (max %d)", MAX_ARGS);
                    return;
                }
                int k = x->mem[x->nmem].nargs++;
                x->mem[x->nmem].vars[k] =
                    ident_is(p->cur, "_") ? 0 : intern_tok(p, p->cur);
                advance(p);
                if (x->mem[x->nmem].vars[k] && p->cur.kind == TK_COLON) {
                    advance(p);
                    if (p->cur.kind != TK_IDENT) {
                        char d[64]; tok_desc(p->cur, d, sizeof d);
                        fail(p, p->cur.line, p->cur.col,
                             "expected a sort name after ':', found %s", d);
                        return;
                    }
                    x->mem[x->nmem].typenames[k] = intern_tok(p, p->cur);
                    advance(p);
                }
                if (p->cur.kind == TK_COMMA) { advance(p); continue; }
                break;
            }
            if (!expect(p, TK_RPAREN)) return;
        }
        x->nmem++;
        if (p->cur.kind == TK_COMMA) { advance(p); continue; }
        break;
    }
    p->nexcls++;
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
    if (p->cur.kind == TK_DOT) {                   /* expanded label (#82 kinds) */
        advance(p);
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a value name after '.', found %s", d);
            return;
        }
        size_t la = strlen(s->a);
        snprintf(s->a + la, MAX_NAME - la, ".%.*s", p->cur.len, p->cur.start);
        advance(p);
    }
    if (!expect(p, TK_GT)) return;
    if (p->cur.kind != TK_IDENT) {
        char d[64]; tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a rule label, found %s", d);
        return;
    }
    copy_ident(s->b, MAX_NAME, p->cur);
    s->bline = p->cur.line; s->bcol = p->cur.col;
    advance(p);
    if (p->cur.kind == TK_DOT) {                   /* expanded label (#82 kinds) */
        advance(p);
        if (p->cur.kind != TK_IDENT) {
            char d[64]; tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col,
                 "expected a value name after '.', found %s", d);
            return;
        }
        size_t lb = strlen(s->b);
        snprintf(s->b + lb, MAX_NAME - lb, ".%.*s", p->cur.len, p->cur.start);
        advance(p);
    }
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

static const char *sort_name(parser *p, int sidx)
{
    return (sidx >= 0 && sidx < p->nsorts) ? p->sorts[sidx].name : "?";
}

/* An argument position's sort as a name: a declared sort, the `int` sentinel,
 * or "?" for a slot no rule pinned down. */
static const char *arg_sort_name(parser *p, int sidx)
{
    return sidx == INT_SORT ? "int" : sort_name(p, sidx);
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
    if (i < 0) return -1;
    if (p->ents[i].sort == sort) return p->ent_pos[i];
    /* a cover position is the member's offset plus its position there (#231) */
    if (sort >= 0 && sort < p->nsorts && p->sorts[sort].is_union)
        for (int m = 0; m < p->sorts[sort].nmem; m++)
            if (p->sorts[sort].mem[m] == p->ents[i].sort)
                return p->sorts[sort].off[m] + p->ent_pos[i];
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
    pi->val_sort = -1;
    for (int k = 0; k < MAX_ARGS; k++) { pi->headsort[k] = -1; pi->headrule[k] = -1; }
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

    /* Resolve union members (#231), now that every sort is known. Members are
     * BASE sorts: a cover of covers would make membership a graph walk and an
     * entity's position ambiguous, and nothing needs it. */
    for (int u = 0; u < p->nsorts; u++) {
        if (!p->sorts[u].is_union) continue;
        int keep = 0;
        for (int i = 0; i < p->sorts[u].nmem; i++) {
            uint32_t nm = (uint32_t)(-p->sorts[u].mem[i] - 2);
            int m = find_sort(p, nm);
            if (m < 0) {
                serr(p, p->sorts[u].line, p->sorts[u].col,
                     "sort '%s' unions the undeclared sort '%s'",
                     p->sorts[u].name, intern_name(p->syms, nm));
                continue;
            }
            if (m == u) {
                serr(p, p->sorts[u].line, p->sorts[u].col,
                     "sort '%s' cannot union itself", p->sorts[u].name);
                continue;
            }
            if (p->sorts[m].is_union) {
                serr(p, p->sorts[u].line, p->sorts[u].col,
                     "sort '%s' unions '%s', which is itself a union — a cover "
                     "covers base sorts, so that an entity has one member sort "
                     "and one position in each cover (#231)",
                     p->sorts[u].name, p->sorts[m].name);
                continue;
            }
            if (p->sorts[m].is_domain || p->sorts[m].is_enum) {
                serr(p, p->sorts[u].line, p->sorts[u].col,
                     "sort '%s' unions '%s', which is %s — a cover is over "
                     "entity sorts", p->sorts[u].name, p->sorts[m].name,
                     p->sorts[m].is_domain ? "an opaque domain" : "an enum");
                continue;
            }
            bool dup = false;
            for (int j = 0; j < keep; j++) if (p->sorts[u].mem[j] == m) dup = true;
            if (dup) {
                serr(p, p->sorts[u].line, p->sorts[u].col,
                     "sort '%s' names '%s' twice", p->sorts[u].name, p->sorts[m].name);
                continue;
            }
            p->sorts[u].mem[keep++] = m;
        }
        p->sorts[u].nmem = keep;
        if (keep == 0)
            serr(p, p->sorts[u].line, p->sorts[u].col,
                 "sort '%s' is a union with no members", p->sorts[u].name);
    }
    /* an entity may not be declared OF a cover: it belongs to a member, and the
     * cover admits it — declaring one directly would give it no member sort */
    for (int i = 0; i < p->nents; i++) {
        int es = p->ents[i].sort;
        if (es >= 0 && es < p->nsorts && p->sorts[es].is_union)
            serr(p, p->ents[i].line, p->ents[i].col,
                 "'%s' is declared of the union '%s' — declare it of a member "
                 "sort; the cover admits it automatically (#231)",
                 intern_name(p->syms, p->ents[i].atom), p->sorts[es].name);
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

    /* A cover's entities are its members' concatenated, in member-declaration
     * order — so a cover position is `off[m] + ent_pos`, and `ent_pos` stays
     * one int per entity rather than one per (entity, sort). Members are base
     * sorts and an entity has one, so the concatenation is disjoint and needs
     * no dedup. */
    for (int u = 0; u < p->nsorts; u++) {
        if (!p->sorts[u].is_union) continue;
        int total = 0;
        for (int i = 0; i < p->sorts[u].nmem; i++) {
            p->sorts[u].off[i] = total;
            total += p->domain_n[p->sorts[u].mem[i]];
        }
        p->domain_n[u] = total;
        free(p->domain_ents[u]);
        p->domain_ents[u] = malloc((size_t)(total ? total : 1)
                                   * sizeof *p->domain_ents[u]);
        for (int i = 0; i < p->sorts[u].nmem; i++) {
            int m = p->sorts[u].mem[i];
            for (int k = 0; k < p->domain_n[m]; k++)
                p->domain_ents[u][p->sorts[u].off[i] + k] = p->domain_ents[m][k];
        }
    }
}

static bool sort_admits(parser *p, int want, int got);

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

    /* burst cues (#11) register a write-only relation with its arg sorts. Not a
     * fluent (no fact, no inertia, no commit) and not a head (nothing may read
     * one, so it is nobody's premise) — an effect target and nothing else. */
    for (int i = 0; i < p->nemits; i++) {
        ast_fluent *em = &p->emits[i];
        pred_info *pi = find_pred(p, em->pred);
        if (pi && (pi->is_fluent || pi->is_provider || pi->is_emit)) {
            serr(p, em->line, em->col, "'%s' is already declared",
                 intern_name(p->syms, em->pred));
            continue;
        }
        pi = intern_pred(p, em->pred, em->nargs);
        if (!pi) { serr(p, em->line, em->col, "too many predicates"); return; }
        pi->is_emit = true;
        pi->arity = em->nargs;
        for (int k = 0; k < em->nargs; k++)
            pi->argsort[k] = decode_sort(p, -(int)em->argsort[k] - 2,
                                         em->line, em->col, "an emit declaration");
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

    /* kind predicates (#124): build-time-only vocabulary — registered so fact
     * checking and modifier selection see arity + arg sorts, never a runtime
     * object. The `value`-sorted position gets argsort -1 + kval_pos. */
    for (int i = 0; i < p->nkindpreds; i++) {
        ast_fluent *v = &p->kindpreds[i];
        pred_info *pi = find_pred(p, v->pred);
        if (pi && (pi->is_fluent || pi->is_provider || pi->is_value ||
                   pi->is_kindpred)) {
            serr(p, v->line, v->col, "'%s' is already declared",
                 intern_name(p->syms, v->pred));
            continue;
        }
        pi = intern_pred(p, v->pred, v->nargs);
        if (!pi) { serr(p, v->line, v->col, "too many predicates"); return; }
        pi->is_kindpred = true;
        pi->arity = v->nargs;
        pi->kval_pos = -1;
        for (int k = 0; k < v->nargs; k++) {
            if (v->argsort[k] == p->metaval) {
                pi->argsort[k] = KARG_VALUE;
                if (pi->kval_pos < 0) pi->kval_pos = k;   /* first, for hints */
            } else {
                pi->argsort[k] = decode_sort(p, -(int)v->argsort[k] - 2,
                                             v->line, v->col,
                                             "a kind predicate declaration");
            }
        }
    }

    /* the meta-sort never keys a runtime object (#124 acceptance): a
     * `value`-sorted argument on stored state or a provider is an error */
    for (int i = 0; i < p->nfluents; i++)
        for (int k = 0; k < p->fluents[i].nargs; k++)
            if (p->fluents[i].argsort[k] == p->metaval)
                serr(p, p->fluents[i].line, p->fluents[i].col,
                     "`value`-sorted arguments belong to kind predicates "
                     "(`value k(value, …)`) — '%s' is stored state, a runtime "
                     "object", intern_name(p->syms, p->fluents[i].pred));
    for (int i = 0; i < p->nemits; i++)
        for (int k = 0; k < p->emits[i].nargs; k++)
            if (p->emits[i].argsort[k] == p->metaval)
                serr(p, p->emits[i].line, p->emits[i].col,
                     "`value`-sorted arguments belong to kind predicates "
                     "(`value k(value, …)`) — '%s' is a runtime cue, fired at "
                     "entities", intern_name(p->syms, p->emits[i].pred));
    for (int i = 0; i < p->nproviders; i++)
        for (int k = 0; k < p->providers[i].nargs; k++)
            if (p->providers[i].argsort[k] == p->metaval)
                serr(p, p->providers[i].line, p->providers[i].col,
                     "`value`-sorted arguments belong to kind predicates "
                     "(`value k(value, …)`) — '%s' is host-answered, a runtime "
                     "relation", intern_name(p->syms, p->providers[i].pred));

    /* rule heads register the conclusion predicates (arity from the head). */
    for (int i = 0; i < p->nrules; i++) {
        ast_atom *h = &p->rules[i].head;
        if (h->is_valuedef || h->is_kinddef) continue;   /* not boolean conclusions */
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
        if (vars[i].sort == SORT_METAVALUE)
            continue;                              /* already resolved */
        if (vars[i].sort == -(int)p->metaval - 2) {
            vars[i].sort = SORT_METAVALUE;         /* the #124 meta-sort */
            continue;
        }
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

/* #217: a judgment read here cannot be sort-checked yet — its signature is not
 * settled until every rule concluding it has been seen — so the resolved sort
 * is parked for `check_head_reads`. Declared vocabulary is checked in place
 * below; only a conclusion has to wait. */
static void defer_head_read(parser *p, const pred_info *pi, int k, int got,
                            int line, int col)
{
    if (k >= MAX_ARGS || (got < 0 && got != INT_SORT)) return;
    if (p->nhreads == p->caphreads) {
        p->caphreads = p->caphreads ? p->caphreads * 2 : 64;
        p->hreads = realloc(p->hreads, (size_t)p->caphreads * sizeof *p->hreads);
    }
    head_read *hr = &p->hreads[p->nhreads++];
    hr->pred = (int)(pi - p->preds);
    hr->arg = k; hr->got = got; hr->line = line; hr->col = col;
}

/* Every argument is a bound variable or a declared entity, with a sort check
 * against the fluent schema. Shared by atoms, effect targets, and fluent reads
 * inside effect expressions. `is_read` distinguishes a position that CONSULTS
 * the predicate from one that concludes or writes it — only a read defers a
 * judgment's sort check (#217); a conclusion IS the signature (#205). */
static bool sort_admits(parser *p, int want, int got);

static void check_pred_args(parser *p, uint32_t pred, pred_info *pi,
                            const ast_arg *args, int nargs,
                            var_bind *vars, int nvars, bool is_read,
                            const char *ctx)
{
    bool schema = pi && (pi->is_fluent || pi->is_provider || pi->is_emit);
    bool judgment = is_read && !schema && pi && pi->is_head &&
                    !pi->is_value && !pi->is_kindpred;
    for (int k = 0; k < nargs; k++) {
        const ast_arg *arg = &args[k];
        int want = schema ? pi->argsort[k] : -1;
        if (arg->is_int) {                      /* a numeric literal — int slots only */
            if (schema && want != INT_SORT)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' is the integer %ld, but that position is "
                     "not declared `int`", k + 1, intern_name(p->syms, pred), arg->ival);
            else if (judgment)
                defer_head_read(p, pi, k, INT_SORT, arg->line, arg->col);
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
        if (judgment)
            defer_head_read(p, pi, k, vi >= 0 ? vars[vi].sort : p->ents[ei].sort,
                            arg->line, arg->col);
        if (schema) {                           /* sort-check against schema */
            int got = vi >= 0 ? vars[vi].sort : p->ents[ei].sort;
            if (want == INT_SORT)
                serr(p, arg->line, arg->col,
                     "argument %d of '%s' expects an integer but got '%s'",
                     k + 1, intern_name(p->syms, pred), intern_name(p->syms, arg->name));
            else if (want >= 0 && got >= 0 && !sort_admits(p, want, got))
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
    case EX_CONST: case EX_ENT: case EX_ROLL: return false;
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
/* Arithmetic is INT-ONLY (#258). expr_value_sort already tracks a static
 * `int | sort` per expression node, but until here nothing enforced it: both
 * are a `long` in the VM, so `hp(X) := at(X) * 3 + 1` compiled and did
 * arithmetic on an opaque cell handle — the very thing the fluent is tagged
 * is_cell to forbid, enforced on the write side only. A sorted value is a
 * handle: copy it, pass it, compare it to another of its own sort, but never
 * compute with it. */
static void require_int(parser *p, int e)
{
    if (e < 0) return;
    int s = expr_value_sort(p, e);
    if (s < 0) return;                              /* already an integer */
    ex_node *n = &p->exprs[e];
    const char *what = n->kind == EX_CALL ? "the result of '%s'" : "'%s'";
    char sub[MAX_NAME + 32];
    snprintf(sub, sizeof sub, what, intern_name(p->syms, n->pred));
    serr(p, n->line, n->col,
         "%s is a %s handle, not a number — arithmetic takes integers, and a "
         "value of a declared domain is opaque (§5.6): copy it, pass it to a "
         "function, or compare it to another %s, but never compute with it",
         sub, value_sort_name(p, s), value_sort_name(p, s));
}

static void check_expr(parser *p, int e, var_bind *vars, int nvars)
{
    if (e < 0) return;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST:
    case EX_ROLL:                    /* a seeded draw — nothing to resolve */
        return;
    case EX_LOAD: {
        pred_info *pi = find_pred(p, n->pred);
        if (!pi && n->nargs == 0) {
            /* #258: a bare identifier naming a rule VARIABLE or a declared
             * ENTITY is an entity term, not a fluent read — how a value
             * provider gets its subject (`chebyshev(A, B)`). Reclassified here
             * rather than at parse time because only the checker knows the
             * rule's variables, and BEFORE note_ref: a variable is not a
             * condition, so the orphan analysis must not see it as one. */
            int vi = var_index(vars, nvars, n->pred);
            int ei = find_entity(p, n->pred);
            if (vi >= 0 || ei >= 0) {
                n->kind  = EX_ENT;
                n->konst = vi >= 0 ? vars[vi].sort : p->ents[ei].sort;
                return;
            }
        }
        note_ref(p, n->pred, n->line, n->col);
        if (n->nprimed) {                      /* primed numeric read (#84/#87) */
            if (!p->in_ramif_eff) {
                serr(p, n->line, n->col,
                     "a primed read (`%s'`) is only legal in a ramification's "
                     "effect expression — it reads the next value a lower "
                     "stratum committed this tick (§5.8)",
                     intern_name(p->syms, n->pred));
                return;
            }
            if (pi && pi->is_value) {
                serr(p, n->line, n->col,
                     "a derived value has no primed form — '%s' is recomputed "
                     "from state, not committed; read it unprimed",
                     intern_name(p->syms, n->pred));
                return;
            }
        }
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
                            true, "a value read");
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
                        true, "an effect expression");
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
            /* a cover admits its members here too (#231): a `placed`-typed
             * parameter takes an actor or a cell, which is what lets one
             * measurement serve every placed sort */
            if (!(want < 0 ? got == want : sort_admits(p, want, got)))
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
                            true, "a test(…)");
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
        require_int(p, n->lhs);
        return;
    default:
        check_expr(p, n->lhs, vars, nvars);
        check_expr(p, n->rhs, vars, nvars);
        require_int(p, n->lhs);
        require_int(p, n->rhs);
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
    if (n->kind == EX_ENT) return (int)n->konst;
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
    case EX_CONST: case EX_ENT: case EX_ROLL: return;
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

/* `defined v(args)` (#116): validate a definedness read — the pred must be a
 * declared derived value, read positively, in a condition position. Over a
 * value with an unconditional base it always holds (warn, keep — a generic
 * rule may mix total and partial subjects later). */
static void check_defined_read(parser *p, ast_atom *at, var_bind *vars,
                               int nvars, bool note, bool in_effect,
                               const char *ctx)
{
    const char *nm = intern_name(p->syms, at->pred);
    if (note) note_ref(p, at->pred, at->line, at->col);
    if (in_effect || at->is_num_effect) {
        serr(p, at->line, at->col,
             "`defined %s(…)` is a condition, not an effect — it cannot "
             "appear in a `causes` clause", nm);
        return;
    }
    if (at->neg) {
        serr(p, at->line, at->col,
             "negated `defined` is not supported yet — no rule concludes its "
             "negation, so `~defined %s(…)` could never fire; guard the "
             "fallback with its own condition instead", nm);
        return;
    }
    if (at->primed || at->is_guard || at->is_expr_guard ||
        at->value != INTERN_NONE) {
        serr(p, at->line, at->col,
             "`defined %s(…)` is itself the condition — it takes no "
             "comparison, prime, or value", nm);
        return;
    }
    int vi = find_value(p, at->pred);
    pred_info *pi = find_pred(p, at->pred);
    if (vi < 0 || !pi || !pi->is_value) {
        serr(p, at->line, at->col,
             "'%s' is not a declared value — `defined` reads a derived "
             "value's definedness (#116)", nm);
        return;
    }
    check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars, true, ctx);
    for (int d = 0; d < p->nvdefs[vi]; d++) {
        ast_rule *r = &p->rules[p->vdefs[vi][d]];
        if (r->nbody == 0 && !r->has_guard) {
            warn(p, at->line, at->col,
                 "'%s' is total (definition '%s' is its unconditional base) — "
                 "`defined %s(…)` always holds", nm, r->label, nm);
            break;
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
    {   /* #124: kind predicates are build-time vocabulary — the only place
         * one may appear is a functor modifier's body (which never reaches
         * this checker; its selection is consumed at expansion) */
        pred_info *kpi = find_pred(p, at->pred);
        if (kpi && kpi->is_kindpred) {
            const char *kn = intern_name(p->syms, at->pred);
            serr(p, at->line, at->col,
                 "'%s' is a kind predicate — build-time only, readable in a "
                 "functor modifier's body (`%s(V, …)` with `V : value`)",
                 kn, kn);
            return;
        }
    }
    {   /* #11 burst cues (§12): write-only vocabulary. An emission is an OUTPUT
         * of a step — the transient twin of an action — so the one legal place
         * to name one is an effect, positively. Reading one back would make a
         * cue a fact (I1) and give the renderer's channel a way into the logic. */
        pred_info *epi = find_pred(p, at->pred);
        if (epi && epi->is_emit) {
            const char *en = intern_name(p->syms, at->pred);
            if (!in_effect) {
                serr(p, at->line, at->col,
                     "'%s' is an emission — a one-shot cue a step fires at the "
                     "client (#11). It is never a fact: it cannot be read in "
                     "%s, only fired in a `causes` clause. Guard on the state "
                     "the same rule writes instead", en, ctx);
                return;
            }
            if (at->neg) {
                serr(p, at->line, at->col,
                     "an emission is fired, never suppressed — `~%s` has no "
                     "meaning (#11). A cue fires when its rule does, so put "
                     "the condition in the rule's body", en);
                return;
            }
            if (at->primed) {
                serr(p, at->line, at->col,
                     "`%s'` marks an emission next-state, but an emission is "
                     "only ever about this transition — drop the `'`", en);
                return;
            }
            if (at->value != INTERN_NONE || at->is_num_effect || at->is_guard) {
                serr(p, at->line, at->col,
                     "'%s' is an emission — it carries only its arguments, no "
                     "value (#11); fire it bare (`%s(…)`)", en, en);
                return;
            }
            if (epi->arity != at->nargs) {
                serr(p, at->line, at->col,
                     "'%s' takes %d argument%s but %d given", en, epi->arity,
                     epi->arity == 1 ? "" : "s", at->nargs);
                return;
            }
            check_pred_args(p, at->pred, epi, at->args, at->nargs, vars, nvars,
                            false, ctx);
            return;
        }
    }
    if (at->is_defined) {                           /* `defined v(args)` (#116) */
        check_defined_read(p, at, vars, nvars, note, in_effect, ctx);
        return;
    }
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
            check_pred_args(p, at->pred, pg, at->args, at->nargs, vars, nvars,
                            true, ctx);
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
        check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars,
                        false, ctx);
        check_expr(p, at->expr_root, vars, nvars);
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
        if (at->primed && at->pred == p->split_pred) {
            serr(p, at->line, at->col,
                 "primed read of the split fluent — schema selection is by the "
                 "PRE-step value (#121), so `%s' = …` cannot be phase-filtered; "
                 "read it unprimed, or drop `split`",
                 intern_name(p->syms, at->pred));
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
    /* An effect writes state, so its target must be vocabulary the step can
     * write: declared state, or a burst cue (#11). Anything else — a typo, a
     * provider, a judgment — used to compile clean and abort inside the step
     * family ("effect on undeclared fluent"); a located error is the same
     * regain-totality trade the rest of the front end takes (EPIC #154). */
    if (in_effect && (!pi || !pi->is_fluent)) {
        const char *n = intern_name(p->syms, at->pred);
        serr(p, at->line, at->col,
             "'%s' is written by a `causes` clause but is not declared state — "
             "declare `state %s%s` for a fact that persists, or `emit %s%s` for "
             "a one-shot cue the client renders (#11); typo?",
             n, n, at->nargs ? "(…)" : "", n, at->nargs ? "(…)" : "");
        return;
    }
    /* `note` marks the CONDITION positions — a body, an `unless`, a
     * `requires`, a binder's `where`/`when` — which are exactly the reads */
    check_pred_args(p, at->pred, pi, at->args, at->nargs, vars, nvars, note, ctx);
}

/* Does expression tree e contain a `roll()` (an EX_ROLL draw)? */
static bool expr_reads_roll(parser *p, int e)
{
    if (e < 0) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_ENT: case EX_LOAD: return false;
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
    case EX_CONST: case EX_ENT: case EX_ROLL: return false;
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
    if (p->nvdefs[vi] >= MAX_VDEFS) {
        serr(p, r->line, r->col,
             "'%s' has too many definitions (max %d — one base, up to %d "
             "guarded layers, and lookup-table rows)",
             intern_name(p->syms, r->head.pred), MAX_VDEFS, MAX_LAYERS);
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
    case EX_CONST: case EX_ENT: case EX_ROLL: case EX_LOAD: return false;
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
/* Is this ground symbol a member of sort index `s`? A value definition's head
 * argument may be one, which makes the definition speak for that instance
 * alone — a lookup table rather than a function. */
static bool value_arg_is_member(parser *p, int s, uint32_t sym)
{
    for (int e = 0; e < domain_size(p, s); e++)
        if (domain_at(p, s, e) == sym) return true;
    return false;
}

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
    if (h->nargs != v->nargs) {
        serr(p, h->line, h->col,
             "the definition of '%s' takes %d argument%s, not %d",
             nm, v->nargs, v->nargs == 1 ? "" : "s", h->nargs);
        return;
    }
    /* A head argument is either a rule PARAMETER — the definition speaks for
     * every instance — or a CONSTANT, and then it speaks only for that one.
     * The second is how a value becomes a lookup table (`sw(a_wide) = 200`),
     * which is what per-instance geometry needs and what a single all-parameter
     * definition cannot say. Applicability is then per ground instance, and so
     * is "exactly one unconditional base" (#94). */
    bool used[MAX_ARGS] = { false };
    int nparam = 0;
    for (int k = 0; k < h->nargs; k++) {
        int f = var_index(r->vars, r->nvars, h->args[k].name);
        if (f < 0 && pi && pi->argsort[k] >= 0 &&
            value_arg_is_member(p, pi->argsort[k], h->args[k].name)) continue;
        nparam++;
        if (f < 0 || used[f]) {
            serr(p, h->args[k].line, h->args[k].col,
                 "value-definition argument %d of '%s' must be a distinct rule "
                 "parameter or a member of its declared sort, got '%s'",
                 k + 1, nm, intern_name(p->syms, h->args[k].name));
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
    case EX_CONST: case EX_ENT: case EX_ROLL: return false;
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
/* Does this definition speak for this ground instance? A head argument that is
 * a rule parameter matches anything; one that is a constant matches only
 * itself. `rargs` may be NULL, meaning "any instance" — the shape the
 * declaration-time checks use. */
static bool vdef_applies(const ast_rule *r, const uint32_t *rargs)
{
    if (!rargs) return true;
    for (int k = 0; k < r->head.nargs; k++)
        if (var_index((var_bind *)r->vars, r->nvars, r->head.args[k].name) < 0 &&
            r->head.args[k].name != rargs[k])
            return false;
    return true;
}

/* Is this definition a lookup-table row — does any head argument pin a
 * constant? Such a definition is unconditional for its own instance only, so
 * it does not collide with another row's base. */
static bool vdef_is_row(const ast_rule *r)
{
    for (int k = 0; k < r->head.nargs; k++)
        if (var_index((var_bind *)r->vars, r->nvars, r->head.args[k].name) < 0)
            return true;
    return false;
}

static void order_value_layers(parser *p)
{
    for (int vi = 0; vi < p->nvaluedecls; vi++) {
        int nds = p->nvdefs[vi];
        if (nds == 0) continue;
        const char *vn = intern_name(p->syms, p->valuedecls[vi].pred);

        /* the base: exactly one unconditional, prior-free definition */
        int base = -1;
        int gl[MAX_LAYERS], ngl = 0;               /* guarded defs, decl order */
        int rows[MAX_VDEFS], nrows = 0;            /* per-instance rows (#94) */
        for (int d = 0; d < nds; d++) {
            int ri = p->vdefs[vi][d];
            ast_rule *r = &p->rules[ri];
            if (r->nbody == 0 && !r->has_guard) {
                /* A lookup-table row (`sw(a_wide) = 200`) is unconditional for
                 * ONE instance, so rows never collide with each other. What
                 * still collides is two definitions that both apply to the
                 * same instance — a catch-all beside a row, or two catch-alls
                 * (#94's rule, restated per instance). */
                if (vdef_is_row(r)) {
                    /* Rows are not LAYERS: `rows` is MAX_VDEFS wide, and
                     * bounding it by MAX_LAYERS clamped every write past the
                     * eighth to index 0 while `nrows` kept counting — so the
                     * copy below read uninitialised slots and stored them as
                     * RULE INDICES. Loud, not capped (#94). */
                    if (nrows >= MAX_VDEFS) {
                        serr(p, r->line, r->col,
                             "'%s' has more than %d lookup-table rows — split "
                             "the value across two (#94)", vn, MAX_VDEFS);
                        return;
                    }
                    rows[nrows++] = ri;
                    continue;
                }
                if (base >= 0) {
                    serr(p, r->line, r->col,
                         "'%s' has two unconditional definitions ('%s' and "
                         "'%s') — exactly one is the base; give the other a "
                         "body, or pin an argument to make it a row (#94)",
                         vn, p->rules[base].label, r->label);
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
            /* #116: partiality is INFERRED — zero unconditional bases makes
             * the value PARTIAL (no keyword; the static safety rule keeps a
             * forgotten base loud at the first arithmetic consumer). At least
             * one prior-free definition must exist, or no layer can ever
             * ground the chain and the value could never be defined. A table
             * of rows with no catch-all is partial in the same honest sense:
             * defined exactly where a row speaks, undefined elsewhere. */
            p->value_partial[vi] = nrows == 0;
            bool grounding = false;
            for (int i = 0; i < nrows && !grounding; i++)
                if (!expr_has_prior(p, p->rules[rows[i]].head.lhs_root))
                    grounding = true;
            for (int i = 0; i < ngl && !grounding; i++)
                if (!expr_has_prior(p, p->rules[gl[i]].head.lhs_root))
                    grounding = true;
            if (!grounding) {
                serr(p, p->valuedecls[vi].line, p->valuedecls[vi].col,
                     "'%s' can never be defined — every definition reads "
                     "`prior`, which layers on the value beneath; add a "
                     "definition that does not read `prior` (#116)", vn);
                return;
            }
        }
        p->value_def[vi] = base;
        p->value_nrows[vi] = nrows;
        for (int i = 0; i < nrows; i++) p->value_rows[vi][i] = rows[i];

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

/* ---- #116 static safety rule ----------------------------------------
 *
 * An ARITHMETIC position (an effect RHS, a clamp bound, a value definition's
 * expression) may read a PARTIAL value only if the same rule's condition also
 * reads it — through any guard over it, or the explicit `defined` atom.
 * Soundness is syntactic, no entailment needed: when the value is undefined,
 * a condition reading it is UNDECIDED, the rule cannot fire, and the RHS
 * never evaluates. An unguarded read is a located compile error; the runtime
 * EXPR_REQDEF trap survives only as defense in depth (world.c). Clamp bounds
 * have no guarding condition, so a partial read there is always an error.
 * The check is per-atom TEXTUAL (same pred, same written args): grounding
 * substitutes uniformly across a rule, so a textual match is a ground match
 * for every binding — and a false positive (definedness implied indirectly,
 * e.g. via `caster(A)`) costs one self-documenting conjunct, the Elm trade. */

static bool expr_tree_reads_value(parser *p, int e, uint32_t pred,
                                  const ast_arg *args, int nargs, int depth)
{
    if (e < 0 || depth > 2 * MAX_ARGS) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_LOAD:
        if (n->pred == pred && n->nargs == nargs) {
            bool same = true;
            for (int k = 0; k < nargs && same; k++)
                same = n->args[k].name == args[k].name;
            if (same) return true;
        }
        return false;
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (expr_tree_reads_value(p, n->cargs[k], pred, args, nargs,
                                      depth + 1))
                return true;
        return false;
    case EX_CONST: case EX_ENT: case EX_ROLL: case EX_TEST: case EX_PRIOR:
        return false;
    case EX_NEG:
        return expr_tree_reads_value(p, n->lhs, pred, args, nargs, depth + 1);
    default:
        return expr_tree_reads_value(p, n->lhs, pred, args, nargs, depth + 1) ||
               expr_tree_reads_value(p, n->rhs, pred, args, nargs, depth + 1);
    }
}

/* Does one condition atom read value (pred, args)? A `defined` atom names it
 * directly; a comparison / body-equality guard was canonicalized into an
 * expression guard whose sides may inline it. */
static bool cond_reads_value(parser *p, const ast_atom *at, uint32_t pred,
                             const ast_arg *args, int nargs)
{
    if (at->is_defined && at->pred == pred && at->nargs == nargs) {
        bool same = true;
        for (int k = 0; k < nargs && same; k++)
            same = at->args[k].name == args[k].name;
        if (same) return true;
    }
    if (at->is_expr_guard)
        return expr_tree_reads_value(p, at->lhs_root, pred, args, nargs, 0) ||
               expr_tree_reads_value(p, at->rhs_root, pred, args, nargs, 0);
    return false;
}

typedef struct {
    const ast_atom *conds[3]; int nconds[3]; int ngroups;
    const char *what, *rulename;       /* diagnostic shape; NULL for clamps */
} psafe_ctx;

static void check_partial_expr(parser *p, int e, const psafe_ctx *cx, int depth)
{
    if (e < 0 || depth > 2 * MAX_ARGS) return;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_LOAD: {
        int vi = find_value(p, n->pred);
        if (vi >= 0 && p->value_partial[vi]) {
            bool covered = false;
            for (int g = 0; g < cx->ngroups && !covered; g++)
                for (int b = 0; b < cx->nconds[g] && !covered; b++)
                    covered = cond_reads_value(p, &cx->conds[g][b], n->pred,
                                               n->args, n->nargs);
            if (!covered) {
                const char *nm = intern_name(p->syms, n->pred);
                if (!cx->what)
                    serr(p, n->line, n->col,
                         "a clamp bound reads '%s', which is partial (no "
                         "unconditional base) — bounds have no guarding "
                         "condition; give '%s' a base or clamp with a total "
                         "value (#116)", nm, nm);
                else
                    serr(p, n->line, n->col,
                         "%s '%s' reads '%s', which is partial (no "
                         "unconditional base), without its condition reading "
                         "it — add `defined %s(…)` (or a comparison over it) "
                         "so the rule cannot fire while '%s' is undefined "
                         "(#116)", cx->what, cx->rulename, nm, nm, nm);
            }
        }
        return;          /* nested definitions are checked at their own rule */
    }
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            check_partial_expr(p, n->cargs[k], cx, depth + 1);
        return;
    case EX_CONST: case EX_ENT: case EX_ROLL: case EX_TEST: case EX_PRIOR:
        return;
    case EX_NEG: check_partial_expr(p, n->lhs, cx, depth + 1); return;
    default:
        check_partial_expr(p, n->lhs, cx, depth + 1);
        check_partial_expr(p, n->rhs, cx, depth + 1);
        return;
    }
}

static void check_partial_arith(parser *p)
{
    bool any = false;
    for (int v = 0; v < p->nvaluedecls && !any; v++)
        any = p->value_partial[v];
    if (!any) return;
    /* value definitions: the expr vs the definition's own body + guard */
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (!r->head.is_valuedef) continue;
        psafe_ctx cx = { { r->body, r->guard }, { r->nbody, r->nguard }, 2,
                         "definition", r->label };
        check_partial_expr(p, r->head.lhs_root, &cx, 0);
    }
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        const char *what = a->is_ramif ? "ramification" : "effect of action";
        for (int b = 0; b < a->neff; b++) {
            if (!a->effects[b].is_num_effect) continue;
            psafe_ctx cx = { { a->requires }, { a->nreq }, 1, what, a->name };
            check_partial_expr(p, a->effects[b].expr_root, &cx, 0);
        }
        for (int bi = 0; bi < a->nbind; bi++) {
            ast_binder *bnd = &p->binders[a->bind_ix[bi]];
            for (int it = 0; it < bnd->nitems; it++) {
                if (!bnd->items[it].eff.is_num_effect) continue;
                psafe_ctx cx = { { a->requires, bnd->where,
                                   bnd->items[it].when },
                                 { a->nreq, bnd->nwhere,
                                   bnd->items[it].nwhen }, 3, what, a->name };
                check_partial_expr(p, bnd->items[it].eff.expr_root, &cx, 0);
            }
        }
    }
    for (int i = 0; i < p->nfluents; i++) {        /* clamp bounds */
        ast_fluent *fl = &p->fluents[i];
        psafe_ctx cx = { { NULL }, { 0 }, 0, NULL, NULL };
        if (fl->rmin_expr >= 0) check_partial_expr(p, fl->rmin_expr, &cx, 0);
        if (fl->rmax_expr >= 0) check_partial_expr(p, fl->rmax_expr, &cx, 0);
    }
}

static ast_action *find_action_named(parser *p, const char *name)
{
    for (int i = 0; i < p->nactions; i++)
        if (strcmp(p->actions[i].name, name) == 0) return &p->actions[i];
    return NULL;
}

/* ---- #159 `exclusive` group well-formedness -------------------------
 *
 * Members name distinct declared ACTIONS (never ramifications — those fire
 * from state; their exclusivity is a rules question) at matching arity; a
 * named variable is a group KEY and must appear in every member at an
 * agreeing sort (`_` never constrains); an optional `: sort` annotation must
 * match the action's declared parameter. A single member keying every
 * position forbids nothing (two distinct instances never share a key) —
 * warned as a no-op. */
static void check_exclusives(parser *p)
{
    for (int e = 0; e < p->nexcls; e++) {
        ast_excl *x = &p->excls[e];
        for (int m = 0; m < x->nmem; m++) {
            for (int j = 0; j < m; j++)
                if (strcmp(x->mem[j].action, x->mem[m].action) == 0)
                    serr(p, x->mem[m].line, x->mem[m].col,
                         "'%s' appears twice in one `exclusive` group",
                         x->mem[m].action);
            ast_action *a = find_action_named(p, x->mem[m].action);
            if (!a) {
                serr(p, x->mem[m].line, x->mem[m].col,
                     "'%s' is not a declared action — `exclusive` groups "
                     "range over submitted actions (#159)",
                     x->mem[m].action);
                continue;
            }
            if (a->is_ramif) {
                serr(p, x->mem[m].line, x->mem[m].col,
                     "'%s' is a ramification — it fires from state, not from "
                     "a submitted action; make its conditions exclusive "
                     "instead (#159)", x->mem[m].action);
                continue;
            }
            if (x->mem[m].nargs != a->nvars) {
                serr(p, x->mem[m].line, x->mem[m].col,
                     "'%s' takes %d argument%s but the `exclusive` member "
                     "lists %d", x->mem[m].action, a->nvars,
                     a->nvars == 1 ? "" : "s", x->mem[m].nargs);
                continue;
            }
            for (int k = 0; k < x->mem[m].nargs; k++) {
                uint32_t v = x->mem[m].vars[k];
                if (!v) continue;
                for (int k2 = 0; k2 < k; k2++)
                    if (x->mem[m].vars[k2] == v)
                        serr(p, x->mem[m].line, x->mem[m].col,
                             "key variable '%s' repeats within one member — "
                             "one position per key variable",
                             intern_name(p->syms, v));
                if (x->mem[m].typenames[k] && a->vars[k].sort >= 0 &&
                    strcmp(intern_name(p->syms, x->mem[m].typenames[k]),
                           p->sorts[a->vars[k].sort].name) != 0)
                    serr(p, x->mem[m].line, x->mem[m].col,
                         "'%s' argument %d ranges over '%s' but the group "
                         "annotates it '%s'", x->mem[m].action, k + 1,
                         p->sorts[a->vars[k].sort].name,
                         intern_name(p->syms, x->mem[m].typenames[k]));
            }
        }
        /* every key var in every member, sorts agreeing */
        for (int m = 0; m < x->nmem; m++)
            for (int k = 0; k < x->mem[m].nargs; k++) {
                uint32_t v = x->mem[m].vars[k];
                if (!v) continue;
                bool firstm = true;
                for (int mp = 0; mp < m && firstm; mp++)
                    for (int kp = 0; kp < x->mem[mp].nargs; kp++)
                        if (x->mem[mp].vars[kp] == v) { firstm = false; break; }
                if (!firstm) continue;
                ast_action *am = find_action_named(p, x->mem[m].action);
                for (int m2 = 0; m2 < x->nmem; m2++) {
                    if (m2 == m) continue;
                    int j = -1;
                    for (int k2 = 0; k2 < x->mem[m2].nargs; k2++)
                        if (x->mem[m2].vars[k2] == v) { j = k2; break; }
                    if (j < 0) {
                        serr(p, x->mem[m2].line, x->mem[m2].col,
                             "key variable '%s' is missing from member '%s' — "
                             "a group key must appear in every member (mark "
                             "don't-care positions `_`)",
                             intern_name(p->syms, v), x->mem[m2].action);
                        continue;
                    }
                    ast_action *a2 = find_action_named(p, x->mem[m2].action);
                    if (am && a2 && am->vars[k].sort >= 0 &&
                        a2->vars[j].sort >= 0 &&
                        am->vars[k].sort != a2->vars[j].sort)
                        serr(p, x->mem[m2].line, x->mem[m2].col,
                             "key variable '%s' ranges over '%s' in '%s' but "
                             "'%s' in '%s'", intern_name(p->syms, v),
                             p->sorts[am->vars[k].sort].name,
                             x->mem[m].action,
                             p->sorts[a2->vars[j].sort].name,
                             x->mem[m2].action);
                }
            }
        if (x->nmem == 1) {
            bool wild = false;
            for (int k = 0; k < x->mem[0].nargs; k++)
                if (!x->mem[0].vars[k]) wild = true;
            if (!wild)
                warn(p, x->line, x->col,
                     "`exclusive %s` keys every argument — two distinct "
                     "instances never share a key, so the group forbids "
                     "nothing; mark the positions that may vary `_` (#159)",
                     x->mem[0].action);
        }
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

/* Preds read PRIMED inside an effect expression (#84's LOADN), collected the
 * same way primed guards are: the reading rule must sit above every writer. */
static void collect_primed_loads(parser *p, int e, uint32_t *pg, int *npgf)
{
    if (e < 0) return;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_LOAD:
        if (n->nprimed) {
            bool seen = false;
            for (int k = 0; k < *npgf && !seen; k++) seen = pg[k] == n->pred;
            if (!seen && *npgf < MAX_PREDS) pg[(*npgf)++] = n->pred;
        }
        return;
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            collect_primed_loads(p, n->cargs[k], pg, npgf);
        return;
    case EX_CONST: case EX_ENT: case EX_ROLL: case EX_TEST: case EX_PRIOR: return;
    case EX_NEG: collect_primed_loads(p, n->lhs, pg, npgf); return;
    default:
        collect_primed_loads(p, n->lhs, pg, npgf);
        collect_primed_loads(p, n->rhs, pg, npgf);
        return;
    }
}

/* max over primed loads in expr e of (writer stratum + 1) — the same edge a
 * primed GUARD contributes; a fluent with no writers floors nothing (its next
 * value is its current value, readable at any stratum). */
static int primed_read_floor(parser *p, int e)
{
    if (e < 0) return 0;
    ex_node *n = &p->exprs[e];
    int m = 0, c;
    switch (n->kind) {
    case EX_LOAD:
        if (n->nprimed)
            for (int j = 0; j < p->nactions; j++)
                if (action_writes_num(p, &p->actions[j], n->pred) &&
                    p->actions[j].stratum + 1 > m)
                    m = p->actions[j].stratum + 1;
        return m;
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++) {
            c = primed_read_floor(p, n->cargs[k]);
            if (c > m) m = c;
        }
        return m;
    case EX_CONST: case EX_ENT: case EX_ROLL: case EX_TEST: case EX_PRIOR: return 0;
    case EX_NEG: return primed_read_floor(p, n->lhs);
    default:
        m = primed_read_floor(p, n->lhs);
        c = primed_read_floor(p, n->rhs);
        return c > m ? c : m;
    }
}

static void stratify_steps(parser *p)
{
    /* count the distinct primed-guarded/primed-read numeric preds */
    uint32_t pg[MAX_PREDS];
    int npgf = 0;
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        for (int b = 0; b < a->nreq; b++) {
            ast_atom *at = &a->requires[b];
            if (!(at->primed && at->is_guard)) continue;
            bool seen = false;
            for (int k = 0; k < npgf && !seen; k++) seen = pg[k] == at->pred;
            if (!seen && npgf < MAX_PREDS) pg[npgf++] = at->pred;
        }
        for (int b = 0; b < a->neff; b++)
            if (a->effects[b].is_num_effect)
                collect_primed_loads(p, a->effects[b].expr_root, pg, &npgf);
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
            for (int b = 0; b < a->neff; b++)      /* primed READS float too (#84) */
                if (a->effects[b].is_num_effect) {
                    int fl = primed_read_floor(p, a->effects[b].expr_root);
                    if (fl > ns) ns = fl;
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
        /* a cycle induced purely through primed READS (#84): name the fluent */
        for (int i = 0; i < p->nactions; i++)
            for (int b = 0; b < p->actions[i].neff; b++) {
                if (!p->actions[i].effects[b].is_num_effect) continue;
                uint32_t lp[MAX_PREDS];
                int nlp = 0;
                collect_primed_loads(p, p->actions[i].effects[b].expr_root,
                                     lp, &nlp);
                for (int k = 0; k < nlp; k++)
                    for (int j = 0; j < p->nactions; j++)
                        if (action_writes_num(p, &p->actions[j], lp[k]) &&
                            p->actions[j].stratum > npgf) {
                            serr(p, p->actions[i].effects[b].line,
                                 p->actions[i].effects[b].col,
                                 "the next value of '%s' depends on itself "
                                 "through a primed read — `%s'` feeds an "
                                 "effect that (directly or through others) "
                                 "writes '%s'; break the loop or read the "
                                 "current value (§5.8)",
                                 intern_name(p->syms, lp[k]),
                                 intern_name(p->syms, lp[k]),
                                 intern_name(p->syms, lp[k]));
                            return;
                        }
            }
        /* fallback (shouldn't be reachable): a located error on the first guard */
        serr(p, p->actions[0].line, p->actions[0].col,
             "primed-guard cycle detected among step rules (§5.8)");
    }
}

/* Deep-copy an expression subtree (#82 roll kinds): each expansion of a kind
 * modifier gets its OWN nodes, so its roll sites key per (value, binding) —
 * bless's d4 on the spell save and on the death save are different dice. */
static int clone_expr(parser *p, int e)
{
    if (e < 0) return -1;
    ex_node src = p->exprs[e];                     /* copy before alloc moves */
    int n = alloc_expr(p, src.kind, src.line, src.col);
    if (n < 0) return -1;
    ex_node *d = &p->exprs[n];
    d->konst = src.konst;
    d->pred = src.pred;
    d->nargs = src.nargs;
    for (int k = 0; k < src.nargs; k++) d->args[k] = src.args[k];
    d->nprimed = src.nprimed;
    if (src.kind == EX_CALL)
        for (int k = 0; k < src.nargs; k++)
            d->cargs[k] = clone_expr(p, src.cargs[k]);
    d->lhs = clone_expr(p, src.lhs);
    d->rhs = clone_expr(p, src.rhs);
    return n;
}

/* #124 fact checking: every membership fact names a declared kind predicate,
 * matches its arity, and every argument is vocabulary — the value position
 * names a declared `: int` value, other positions name members of their sort
 * (entities or enum members). Typos die here, not silently match nothing. */
static void check_kfacts(parser *p)
{
    for (int i = 0; i < p->nkfacts; i++) {
        ast_kfact *kf = &p->kfacts[i];
        pred_info *pi = find_pred(p, kf->pred);
        if (!pi || !pi->is_kindpred) {
            serr(p, kf->line, kf->col,
                 "'%s' is not a declared kind predicate — declare "
                 "`value %s(value, …)` first",
                 intern_name(p->syms, kf->pred), intern_name(p->syms, kf->pred));
            continue;
        }
        if (kf->nargs != pi->arity) {
            serr(p, kf->line, kf->col,
                 "'%s' takes %d arguments, this fact has %d",
                 intern_name(p->syms, kf->pred), pi->arity, kf->nargs);
            continue;
        }
        for (int k = 0; k < kf->nargs; k++) {
            if (pi->argsort[k] == KARG_VALUE) {
                if (find_value(p, kf->args[k]) < 0)
                    serr(p, kf->line, kf->col,
                         "'%s' is not a declared value — the `value` position "
                         "of '%s' names a `value … : int` declaration",
                         intern_name(p->syms, kf->args[k]),
                         intern_name(p->syms, kf->pred));
                continue;
            }
            int s = pi->argsort[k];
            if (s < 0) continue;                   /* decode already reported */
            bool in = false;
            for (int e = 0; e < domain_size(p, s) && !in; e++)
                in = domain_at(p, s, e) == kf->args[k];
            if (!in)
                serr(p, kf->line, kf->col,
                     "'%s' is not a member of sort '%s'",
                     intern_name(p->syms, kf->args[k]), p->sorts[s].name);
        }
    }
}

/* ---- #125: the kind stratum is rules ----------------------------------
 * "A world with no step function": membership facts + kind rules over the
 * sealed value domain, evaluated at grounding by the SAME scalar DL engine
 * (dl_solve — build-time, small), with the same verdicts and the same
 * dl_why. The grounder is a two-valued consumer: any UNDECIDED kind atom is
 * a located authoring error. Modifier selection queries these verdicts, so
 * a derived kind expands identically to the equivalent fact-only spelling. */

static uint32_t ground_pred(parser *p, uint32_t pred, const uint32_t *args, int n);
static const char *prov_str(parser *p, int line, char *buf, size_t n);
static void inst_name(parser *p, char *buf, size_t n, const char *label,
                      var_bind *vars, int nvars, const uint32_t *binding);
static bool members_ok(parser *p, ast_atom *body, int n, var_bind *vars,
                       int nvars, const uint32_t *binding);
static ast_rule *find_rule(parser *p, const char *label);

/* A rule concluding a kind predicate — a build-time taxonomy rule. */
static bool rule_is_kind(parser *p, ast_rule *r)
{
    if (r->head.is_valuedef || r->head.is_kinddef) return false;
    pred_info *pi = find_pred(p, r->head.pred);
    return pi && pi->is_kindpred;
}

static int kindpred_index(parser *p, uint32_t pred)
{
    for (int i = 0; i < p->nkindpreds; i++)
        if (p->kindpreds[i].pred == pred) return i;
    return -1;
}

/* Domain of one kind-stratum dimension: the value symbols for the meta-sort,
 * a sort's members (entities or enum values) otherwise. */
static long kdim_size(parser *p, int sort)
{
    return sort == SORT_METAVALUE ? p->nvaluedecls : domain_size(p, sort);
}
static uint32_t kdim_at(parser *p, int sort, long i)
{
    return sort == SORT_METAVALUE ? p->valuedecls[i].pred : domain_at(p, sort, i);
}

/* Append a ground kind atom to the sweep list (deduped; the list is small). */
static void kadd_atom(parser *p, uint32_t atom, int rule_ix)
{
    for (int i = 0; i < p->nkatoms; i++)
        if (p->katoms[i] == atom) {
            if (p->katom_rule[i] < 0) p->katom_rule[i] = rule_ix;
            return;
        }
    if (p->nkatoms == p->capkatoms) {
        p->capkatoms = p->capkatoms ? p->capkatoms * 2 : 64;
        p->katoms = realloc(p->katoms, (size_t)p->capkatoms * sizeof *p->katoms);
        p->katom_rule = realloc(p->katom_rule,
                                (size_t)p->capkatoms * sizeof *p->katom_rule);
    }
    p->katoms[p->nkatoms] = atom;
    p->katom_rule[p->nkatoms] = rule_ix;
    p->nkatoms++;
}

/* One kind-rule atom's ground literal under a binding. `dims` is the rule's
 * variables extended with one synthetic dimension per `_` occurrence
 * (wdim[b][a] maps atom b, arg a to its dimension, -1 = not a wildcard). */
static dl_lit kind_ground_lit(parser *p, ast_atom *at, int b, var_bind *dims,
                              int ndims, const uint32_t *bind,
                              int wdim[][MAX_ARGS])
{
    uint32_t args[MAX_ARGS];
    for (int a = 0; a < at->nargs; a++) {
        int vi = var_index(dims, ndims, at->args[a].name);
        if (wdim[b][a] >= 0)      args[a] = bind[wdim[b][a]];
        else if (vi >= 0)         args[a] = bind[vi];
        else                      args[a] = at->args[a].name;   /* a symbol */
    }
    dl_lit l = { ground_pred(p, at->pred, args, at->nargs), at->neg };
    return l;
}

/* Validate one kind-atom argument list against its predicate: variables must
 * carry the position's sort (the meta-sort at kval_pos), symbols must be
 * members of the position's domain; `_` is legal only where allowed. */
static void kind_check_args(parser *p, ast_atom *at, pred_info *pi,
                            var_bind *vars, int nvars, uint32_t wild,
                            bool allow_wild)
{
    for (int a = 0; a < at->nargs; a++) {
        uint32_t nm = at->args[a].name;
        if (nm == wild) {
            if (!allow_wild)
                serr(p, at->args[a].line, at->args[a].col,
                     "a head names what it concludes — no `_` wildcards");
            continue;
        }
        int vi = var_index(vars, nvars, nm);
        if (vi >= 0) {
            int want = pi->argsort[a] == KARG_VALUE ? SORT_METAVALUE
                                                    : pi->argsort[a];
            if (vars[vi].sort != want)
                serr(p, at->args[a].line, at->args[a].col,
                     "'%s' has the wrong sort for this position of '%s'",
                     intern_name(p->syms, nm), intern_name(p->syms, at->pred));
            continue;
        }
        if (pi->argsort[a] == KARG_VALUE) {
            if (find_value(p, nm) < 0)
                serr(p, at->args[a].line, at->args[a].col,
                     "'%s' is not a declared value", intern_name(p->syms, nm));
            continue;
        }
        int s = pi->argsort[a];
        if (s < 0) continue;
        bool in = false;
        for (long e = 0; e < domain_size(p, s) && !in; e++)
            in = domain_at(p, s, e) == nm;
        if (!in)
            serr(p, at->args[a].line, at->args[a].col,
                 "'%s' is not a member of sort '%s'",
                 intern_name(p->syms, nm), p->sorts[s].name);
    }
}

static void solve_kind_stratum(parser *p)
{
    if (p->nkindpreds == 0)
        return;
    uint32_t wild = intern_id(p->syms, "_");

    /* which kind preds have concluding rules (any polarity) — those are
     * DERIVED: never closed-world, never negatable in a body */
    bool concluded[MAX_FLUENTS] = { false }, negated[MAX_FLUENTS] = { false };
    for (int i = 0; i < p->nrules; i++)
        if (rule_is_kind(p, &p->rules[i])) {
            int ki = kindpred_index(p, p->rules[i].head.pred);
            if (ki >= 0) concluded[ki] = true;
        }

    /* orphan kinds (#126): a declared kind predicate with no membership
     * facts and no deriving rules classifies nothing — a typo or a stub */
    for (int ki = 0; ki < p->nkindpreds; ki++) {
        if (concluded[ki]) continue;
        bool has_fact = false;
        for (int f = 0; f < p->nkfacts && !has_fact; f++)
            has_fact = p->kfacts[f].pred == p->kindpreds[ki].pred;
        if (!has_fact)
            warn(p, p->kindpreds[ki].line, p->kindpreds[ki].col,
                 "kind '%s' has no membership facts and no deriving rules — "
                 "it classifies nothing",
                 intern_name(p->syms, p->kindpreds[ki].pred));
    }

    /* ---- validate the kind rules ---- */
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (!rule_is_kind(p, r)) continue;
        resolve_vars(p, r->vars, r->nvars, "a kind rule");
        if (r->has_guard || r->nguard)
            serr(p, r->line, r->col,
                 "`unless` on a kind rule is not supported in this slice — "
                 "use a defeater (`~>`) or superiority");
        pred_info *hp = find_pred(p, r->head.pred);
        if (r->head.nargs != hp->arity) {
            serr(p, r->head.line, r->head.col, "'%s' takes %d arguments, not %d",
                 intern_name(p->syms, r->head.pred), hp->arity, r->head.nargs);
            continue;
        }
        kind_check_args(p, &r->head, hp, r->vars, r->nvars, wild, false);
        for (int b = 0; b < r->nbody; b++) {
            ast_atom *at = &r->body[b];
            if (at->is_member) continue;           /* #95 static filter */
            pred_info *bp = find_pred(p, at->pred);
            if (!bp || !bp->is_kindpred) {
                serr(p, at->line, at->col,
                     "kind rules run at world-build and cannot read '%s' — "
                     "fluents, providers, rolls and judgments are runtime; "
                     "guard the modifier instead",
                     intern_name(p->syms, at->pred));
                continue;
            }
            if (at->primed || at->value != INTERN_NONE || at->is_guard ||
                at->is_expr_guard || at->is_num_effect) {
                serr(p, at->line, at->col,
                     "a kind atom is a plain (possibly negated) literal");
                continue;
            }
            if (at->nargs != bp->arity) {
                serr(p, at->line, at->col, "'%s' takes %d arguments, not %d",
                     intern_name(p->syms, at->pred), bp->arity, at->nargs);
                continue;
            }
            if (at->neg) {
                int ki = kindpred_index(p, at->pred);
                if (ki >= 0 && concluded[ki])
                    serr(p, at->line, at->col,
                         "negation over the DERIVED kind '%s' is not "
                         "closed-world — only facts-only kinds close at "
                         "world-build; conclude the complement with a rule",
                         intern_name(p->syms, at->pred));
                else if (ki >= 0)
                    negated[ki] = true;
            }
            kind_check_args(p, at, bp, r->vars, r->nvars, wild, true);
        }
    }
    if (p->nerrors)
        return;

    /* ---- assemble the theory ---- */
    p->kth = dl_theory_new(p->syms);
    for (int i = 0; i < p->nkfacts; i++) {
        ast_kfact *kf = &p->kfacts[i];
        uint32_t a = ground_pred(p, kf->pred, kf->args, kf->nargs);
        dl_add_fact(p->kth, dl_pos(a));
        kadd_atom(p, a, -1);
    }
    /* closed-world negatives, only where a body actually negates a
     * facts-only kind: every unasserted combination of its argument domains */
    for (int ki = 0; ki < p->nkindpreds; ki++) {
        if (!negated[ki] || concluded[ki]) continue;
        pred_info *pi = find_pred(p, p->kindpreds[ki].pred);
        long total = 1;
        for (int a = 0; a < pi->arity; a++)
            total *= kdim_size(p, pi->argsort[a] == KARG_VALUE
                                  ? SORT_METAVALUE : pi->argsort[a]);
        for (long ix = 0; ix < total; ix++) {
            uint32_t args[MAX_ARGS];
            long rem = ix;
            for (int a = 0; a < pi->arity; a++) {
                int s = pi->argsort[a] == KARG_VALUE ? SORT_METAVALUE
                                                     : pi->argsort[a];
                long d = kdim_size(p, s);
                args[a] = kdim_at(p, s, rem % d);
                rem /= d;
            }
            bool asserted = false;
            for (int f = 0; f < p->nkfacts && !asserted; f++) {
                ast_kfact *kf = &p->kfacts[f];
                if (kf->pred != pi->pred || kf->nargs != pi->arity) continue;
                asserted = true;
                for (int a = 0; a < pi->arity && asserted; a++)
                    asserted = kf->args[a] == args[a];
            }
            if (!asserted)
                dl_add_fact(p->kth, dl_neg(ground_pred(p, pi->pred, args,
                                                       pi->arity)));
        }
    }

    /* ground the kind rules: variables plus one dimension per `_`. Each
     * ground instance is recorded with its literals so the contested sweep
     * below can re-derive applicability from the solved result. */
    struct kinst { int rule, id; dl_lit head; dl_lit body[MAX_BODY]; int nb; };
    struct kinst *kmap = NULL;
    int nkmap = 0, capkmap = 0;
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (!rule_is_kind(p, r)) continue;
        var_bind dims[MAX_BODY * MAX_ARGS];
        int ndims = r->nvars;
        for (int v = 0; v < r->nvars; v++) dims[v] = r->vars[v];
        int wdim[MAX_BODY][MAX_ARGS];
        for (int b = 0; b < r->nbody; b++) {
            ast_atom *at = &r->body[b];
            pred_info *bp = find_pred(p, at->pred);
            for (int a = 0; a < at->nargs; a++) {
                wdim[b][a] = -1;
                if (at->is_member || !bp || !bp->is_kindpred) continue;
                if (at->args[a].name != wild) continue;
                dims[ndims].name = wild;
                dims[ndims].sort = bp->argsort[a] == KARG_VALUE
                                       ? SORT_METAVALUE : bp->argsort[a];
                dims[ndims].line = at->line;
                dims[ndims].col = at->col;
                wdim[b][a] = ndims++;
            }
        }
        long total = 1;
        for (int d = 0; d < ndims; d++) {
            long sz = kdim_size(p, dims[d].sort);
            if (sz <= 0) { total = 0; break; }
            total *= sz;
        }
        for (long ix = 0; ix < total; ix++) {
            uint32_t bind[MAX_BODY * MAX_ARGS];
            long rem = ix;
            for (int d = 0; d < ndims; d++) {
                long sz = kdim_size(p, dims[d].sort);
                bind[d] = kdim_at(p, dims[d].sort, rem % sz);
                rem /= sz;
            }
            if (!members_ok(p, r->body, r->nbody, dims, ndims, bind))
                continue;
            dl_lit body[MAX_BODY];
            int nb = 0;
            for (int b = 0; b < r->nbody; b++) {
                if (r->body[b].is_member) continue;
                body[nb] = kind_ground_lit(p, &r->body[b], b, dims, ndims,
                                           bind, wdim);
                kadd_atom(p, body[nb].atom, -1);
                nb++;
            }
            int hw[MAX_BODY][MAX_ARGS] = { { -1 } };
            for (int a = 0; a < MAX_ARGS; a++) hw[0][a] = -1;
            dl_lit head = kind_ground_lit(p, &r->head, 0, dims, ndims, bind, hw);
            kadd_atom(p, head.atom, i);
            char nm[MAX_GROUND];
            inst_name(p, nm, sizeof nm, r->label, r->vars, r->nvars, bind);
            int id = dl_add_rule(p->kth, nm, r->kind, head, body, nb);
            char pb[MAX_NAME + 24];
            dl_set_prov(p->kth, id, prov_str(p, r->line, pb, sizeof pb));
            if (nkmap == capkmap) {
                capkmap = capkmap ? capkmap * 2 : 64;
                kmap = realloc(kmap, (size_t)capkmap * sizeof *kmap);
            }
            kmap[nkmap].rule = i;
            kmap[nkmap].id = id;
            kmap[nkmap].head = head;
            kmap[nkmap].nb = nb;
            for (int b = 0; b < nb; b++) kmap[nkmap].body[b] = body[b];
            nkmap++;
        }
    }

    /* superiority between kind rules: every ground-instance pair (dl consults
     * a sup edge only when the two heads actually conflict) */
    for (int s = 0; s < p->nsups; s++) {
        ast_rule *ra = find_rule(p, p->sups[s].a);
        ast_rule *rb = find_rule(p, p->sups[s].b);
        bool ka = ra && rule_is_kind(p, ra), kb = rb && rule_is_kind(p, rb);
        if (!ka && !kb) continue;
        if (ka != kb) {
            serr(p, p->sups[s].aline, p->sups[s].acol,
                 "'%s' > '%s': a kind rule orders only against kind rules — "
                 "the strata never conflict", p->sups[s].a, p->sups[s].b);
            continue;
        }
        for (int x = 0; x < nkmap; x++) {
            if (&p->rules[kmap[x].rule] != ra) continue;
            for (int y = 0; y < nkmap; y++)
                if (&p->rules[kmap[y].rule] == rb)
                    dl_add_sup(p->kth, kmap[x].id, kmap[y].id);
        }
    }

    p->kres = dl_solve(p->kth);

    /* ---- the two-valued consumer's sweep. Two failure shapes:
     *  - a CYCLE leaves a verdict UNDECIDED (the scaffold engine; §5.2's
     *    cycle rule is #109) — located error;
     *  - a CONTESTED membership: this engine is ambiguity-BLOCKING, so an
     *    unresolved conflict REFUTES both polarities — detected as
     *    "applicable support on both sides, neither proved", re-derived
     *    from our own ground records against the solved result. ---- */
    for (int i = 0; i < p->nkatoms; i++) {
        uint32_t a = p->katoms[i];
        dl_verdict pv = dl_defeasible(p->kres, dl_pos(a));
        dl_verdict nv = dl_defeasible(p->kres, dl_neg(a));
        if (pv == DL_UNDECIDED || nv == DL_UNDECIDED) {
            int ri = p->katom_rule[i];
            serr(p, ri >= 0 ? p->rules[ri].line : 1,
                 ri >= 0 ? p->rules[ri].col : 1,
                 "kind membership '%s' is UNDECIDED at world-build — its "
                 "support is cyclic (§5.2, #109); break the cycle",
                 intern_name(p->syms, a));
            continue;
        }
        if (pv == DL_PROVED || nv == DL_PROVED)
            continue;                              /* decided, either way */
        const char *pl = NULL, *nl = NULL;
        int line = 0, col = 0;
        for (int x = 0; x < nkmap && !(pl && nl); x++) {
            if (kmap[x].head.atom != a) continue;
            bool app = true;
            for (int b = 0; b < kmap[x].nb && app; b++)
                app = dl_defeasible(p->kres, kmap[x].body[b]) == DL_PROVED;
            if (!app) continue;
            ast_rule *r = &p->rules[kmap[x].rule];
            if (!kmap[x].head.neg && !pl) {
                pl = r->label;
                line = r->line; col = r->col;
            } else if (kmap[x].head.neg && !nl) {
                nl = r->label;
            }
        }
        if (pl && nl)
            serr(p, line, col,
                 "kind membership '%s' is CONTESTED at world-build — '%s' "
                 "and '%s' both fire with neither superior; add "
                 "`%s > %s` (or the reverse), or a fact",
                 intern_name(p->syms, a), pl, nl, nl, pl);
    }
    free(kmap);

    /* build-time why (#125): render the trace for one queried kind atom with
     * the ordinary dl_trace renderer — byte-identical to the runtime format */
    if (p->kwhy_query && p->kwhy_out) {
        uint32_t qa = intern_id(p->syms, p->kwhy_query);
        dl_why(p->kth, p->kres, dl_pos(qa), p->kwhy_out);
    }
}


/* #124 kinds-are-facts: expand each functor-position modifier
 *
 *     rule L(A: actor, V: value): k(V, …) & body => V(A[, T]) = expr
 *
 * into one LAYER definition per declared value whose memberships satisfy the
 * body's kind-atom conjunction (matched against the `fact` set — constants
 * and `_` wildcards, this slice). The kind atoms are consumed by selection;
 * the runtime body rides into every expansion. Everything downstream is
 * #115's machinery unchanged: expanded labels `L.valuename` (real, orderable
 * by `>`), per-(member, binding) roll sites via expression clones, and the
 * commuting-layer shape check so unordered coexistence stays legal. The
 * subject tuple binds each member's leading arguments (V(A) = arg 0, the #83
 * roller convention; V(A, T) adds the target — Dodge-shaped selection);
 * members whose arity or leading sorts do not fit simply do not match, and a
 * modifier matching nothing is an orphan-style warning, not an error. */
static void expand_kind_rules(parser *p)
{
    uint32_t wild = 0;                             /* interned on first use —
                                                    * never perturb kind-free
                                                    * worlds' atom streams */
    int n0 = p->nrules;                            /* expansions append */
    for (int i = 0; i < n0; i++) {
        ast_rule *k = &p->rules[i];
        if (!k->head.is_kinddef) continue;
        if (!wild) wild = intern_id(p->syms, "_");
        resolve_vars(p, k->vars, k->nvars, "a kind modifier");
        if (k->kind != DL_DEFEASIBLE) {
            serr(p, k->head.line, k->head.col,
                 "a kind modifier is defeasible — write '=>'");
            continue;
        }
        int nsubj = k->head.nargs;
        int subj[MAX_ARGS];
        bool bad = false;
        for (int a = 0; a < nsubj && !bad; a++) {
            int si = var_index(k->vars, k->nvars, k->head.args[a].name);
            if (si < 0) {
                serr(p, k->head.args[a].line, k->head.args[a].col,
                     "functor head argument '%s' is not a rule parameter",
                     intern_name(p->syms, k->head.args[a].name));
                bad = true;
            } else if (k->vars[si].sort == SORT_METAVALUE) {
                serr(p, k->head.args[a].line, k->head.args[a].col,
                     "a functor head's subject binds an entity-sorted "
                     "parameter, not `value`");
                bad = true;
            } else {
                subj[a] = si;
            }
        }
        if (bad) continue;
        int cls = valuedef_class(p, k->head.lhs_root);
        if (cls == 0) {
            serr(p, k->head.line, k->head.col,
                 "a kind modifier layers on every member — its expression must "
                 "mention `prior` (an override would REPLACE each member's "
                 "definition; write per-value definitions for that)");
            continue;
        }
        /* #144: any `prior`-mentioning shape is admitted. The commuting
         * classes (`prior + e`, max, min) coexist unordered as before;
         * a NON-commuting shape (`prior / 2`, `prior - e`) expands like any
         * other and #94's per-member ordering check then demands it be
         * totally ordered against its neighbors — one kind-level `A > B`
         * (#145) usually says all of it. */

        /* split the body: kind atoms are the SELECTION, the rest is runtime */
        int ksel[MAX_BODY], nksel = 0;
        ast_atom rbody[MAX_BODY];
        int nrbody = 0;
        for (int b = 0; b < k->nbody && !bad; b++) {
            ast_atom *ka = &k->body[b];
            pred_info *pi = find_pred(p, ka->pred);
            if (!pi || !pi->is_kindpred) {
                rbody[nrbody++] = *ka;
                continue;
            }
            if (ka->neg || ka->primed || ka->value != INTERN_NONE) {
                serr(p, ka->line, ka->col,
                     "this slice selects with positive kind-atom conjunctions "
                     "— negation and derived kinds land with #125");
                bad = true;
                break;
            }
            if (ka->nargs != pi->arity) {
                serr(p, ka->line, ka->col, "'%s' takes %d arguments, not %d",
                     intern_name(p->syms, ka->pred), pi->arity, ka->nargs);
                bad = true;
                break;
            }
            for (int a = 0; a < ka->nargs && !bad; a++) {
                uint32_t nm = ka->args[a].name;
                int vi = var_index(k->vars, k->nvars, nm);
                if (pi->argsort[a] == KARG_VALUE) {
                    /* a value position (#143): the functor variable, any
                     * other `value`-sorted parameter (a LINK join), a
                     * declared value symbol, or `_` */
                    if (vi >= 0) {
                        if (k->vars[vi].sort != SORT_METAVALUE) {
                            serr(p, ka->args[a].line, ka->args[a].col,
                                 "'%s' fills a value position of '%s' but is "
                                 "not a `value`-sorted parameter",
                                 intern_name(p->syms, nm),
                                 intern_name(p->syms, ka->pred));
                            bad = true;
                        }
                        continue;
                    }
                    if (nm == wild)
                        continue;
                    if (find_value(p, nm) < 0) {
                        serr(p, ka->args[a].line, ka->args[a].col,
                             "'%s' is not a declared value",
                             intern_name(p->syms, nm));
                        bad = true;
                    }
                    continue;
                }
                if (nm == wild)
                    continue;                      /* wildcard facet */
                if (vi >= 0) {
                    serr(p, ka->args[a].line, ka->args[a].col,
                         "a variable facet ('%s') needs selector expansion over "
                         "the solved stratum (#179) — this slice takes a "
                         "constant or `_`",
                         intern_name(p->syms, nm));
                    bad = true;
                    continue;
                }
                int s = pi->argsort[a];
                if (s >= 0) {
                    bool in = false;
                    for (int e = 0; e < domain_size(p, s) && !in; e++)
                        in = domain_at(p, s, e) == nm;
                    if (!in) {
                        serr(p, ka->args[a].line, ka->args[a].col,
                             "'%s' is not a member of sort '%s'",
                             intern_name(p->syms, nm), p->sorts[s].name);
                        bad = true;
                    }
                }
            }
            ksel[nksel++] = b;
        }
        if (bad) continue;
        /* every value parameter — the functor variable and any link-joined
         * one — must be selected by at least one kind atom */
        for (int vv = 0; vv < k->nvars && !bad; vv++) {
            if (k->vars[vv].sort != SORT_METAVALUE)
                continue;
            bool seen = false;
            for (int q = 0; q < nksel && !seen; q++) {
                ast_atom *ka = &k->body[ksel[q]];
                for (int a = 0; a < ka->nargs && !seen; a++)
                    seen = ka->args[a].name == k->vars[vv].name;
            }
            if (!seen) {
                serr(p, k->vars[vv].line, k->vars[vv].col,
                     "the value parameter '%s' is not selected by any kind "
                     "atom — add `<kind>(%s, …)` to the body",
                     intern_name(p->syms, k->vars[vv].name),
                     intern_name(p->syms, k->vars[vv].name));
                bad = true;
            }
        }
        if (bad) continue;
        if (nksel == 0) {
            serr(p, k->head.line, k->head.col,
                 "the functor variable '%s' is not selected by any kind atom "
                 "— add `<kind>(%s, …)` to the body",
                 intern_name(p->syms, k->head.pred),
                 intern_name(p->syms, k->head.pred));
            continue;
        }
        /* the link-joined value parameters, enumerated jointly below */
        int ovv[MAX_ARGS], novv = 0;
        for (int vv = 0; vv < k->nvars; vv++)
            if (k->vars[vv].sort == SORT_METAVALUE &&
                k->vars[vv].name != k->head.pred)
                ovv[novv++] = vv;

        int matched = 0;
        for (int vi = 0; vi < p->nvaluedecls; vi++) {
            ast_fluent *v = &p->valuedecls[vi];
            pred_info *vpi = find_pred(p, v->pred);
            /* #125/#143: selection queries the solved kind stratum — a fact
             * and a derived membership answer identically. The functor
             * variable is fixed to this member; link-joined value parameters
             * are EXISTENTIAL, enumerated jointly (consistent across atoms);
             * `_` wildcards stay per-atom existentials over the position's
             * domain. Selected iff some joint assignment proves them all. */
            long jtotal = 1;
            for (int q = 0; q < novv; q++)
                jtotal *= p->nvaluedecls ? p->nvaluedecls : 1;
            bool sel = false;
            for (long jix = 0; jix < jtotal && !sel; jix++) {
                uint32_t assign[MAX_ARGS];         /* per ovv slot */
                long jrem = jix;
                for (int q = 0; q < novv; q++) {
                    assign[q] = p->valuedecls[jrem % p->nvaluedecls].pred;
                    jrem /= p->nvaluedecls;
                }
                bool all = true;
                for (int q2 = 0; q2 < nksel && all; q2++) {
                    ast_atom *ka = &k->body[ksel[q2]];
                    pred_info *pi = find_pred(p, ka->pred);
                    uint32_t args[MAX_ARGS];
                    int wpos[MAX_ARGS], wsort[MAX_ARGS], nw = 0;
                    for (int a = 0; a < pi->arity; a++) {
                        uint32_t nm = ka->args[a].name;
                        int isv = pi->argsort[a] == KARG_VALUE;
                        if (nm == k->head.pred) {
                            args[a] = v->pred;
                            continue;
                        }
                        int oq = -1;
                        for (int q3 = 0; q3 < novv && oq < 0; q3++)
                            if (k->vars[ovv[q3]].name == nm) oq = q3;
                        if (oq >= 0) {
                            args[a] = assign[oq];
                            continue;
                        }
                        if (nm == wild) {
                            wpos[nw] = a;
                            wsort[nw] = isv ? SORT_METAVALUE : pi->argsort[a];
                            nw++;
                            continue;
                        }
                        args[a] = nm;              /* a constant symbol */
                    }
                    long total = 1;
                    for (int wq = 0; wq < nw; wq++)
                        total *= kdim_size(p, wsort[wq]);
                    bool any = false;
                    for (long ix = 0; ix < total && !any; ix++) {
                        long rem = ix;
                        for (int wq = 0; wq < nw; wq++) {
                            long dsz = kdim_size(p, wsort[wq]);
                            args[wpos[wq]] = kdim_at(p, wsort[wq], rem % dsz);
                            rem /= dsz;
                        }
                        uint32_t ga = ground_pred(p, ka->pred, args, pi->arity);
                        any = p->kres &&
                              dl_defeasible(p->kres, dl_pos(ga)) == DL_PROVED;
                    }
                    all = any;
                }
                sel = all;
            }
            if (!sel)
                continue;
            /* the member must fit the subject tuple: enough arguments, and
             * the leading sorts equal the bound parameters' sorts */
            if (!vpi || v->nargs < nsubj)
                continue;
            bool fit = true;
            for (int a = 0; a < nsubj && fit; a++)
                fit = vpi->argsort[a] == k->vars[subj[a]].sort;
            if (!fit)
                continue;
            matched++;

            if (p->nrules >= MAX_RULES) {
                serr(p, k->head.line, k->head.col,
                     "too many rules (max %d) expanding modifier '%s'",
                     MAX_RULES, k->label);
                return;
            }
            ast_rule *r = &p->rules[p->nrules];
            memset(r, 0, sizeof *r);
            int ll = snprintf(r->label, MAX_NAME, "%s.%s", k->label,
                              intern_name(p->syms, v->pred));
            if (ll >= MAX_NAME) {                  /* loud, never silently cut */
                serr(p, k->head.line, k->head.col,
                     "expanded label '%s.%s' exceeds %d chars — shorten the "
                     "modifier or value name", k->label,
                     intern_name(p->syms, v->pred), MAX_NAME);
                continue;
            }
            r->line = k->line;
            r->col = k->col;
            r->kind = DL_DEFEASIBLE;
            r->nvars = v->nargs;
            for (int a = 0; a < nsubj; a++)
                r->vars[a] = k->vars[subj[a]];     /* the subjects, resolved */
            for (int a = nsubj; a < v->nargs; a++) {
                char vb[16];
                snprintf(vb, sizeof vb, "__k%d", a);
                r->vars[a].name = intern_id(p->syms, vb);
                r->vars[a].sort = vpi->argsort[a]; /* resolved index */
                r->vars[a].line = k->line;
                r->vars[a].col = k->col;
            }
            r->nbody = nrbody;
            for (int b = 0; b < nrbody; b++) r->body[b] = rbody[b];
            r->nguard = k->nguard;
            for (int g = 0; g < k->nguard; g++) r->guard[g] = k->guard[g];
            r->has_guard = k->has_guard;
            r->head.is_valuedef = true;
            r->head.pred = v->pred;
            r->head.line = k->head.line;
            r->head.col = k->head.col;
            r->head.nargs = v->nargs;
            for (int a = 0; a < v->nargs; a++) {
                r->head.args[a].name = r->vars[a].name;
                r->head.args[a].line = k->line;
                r->head.args[a].col = k->col;
            }
            r->head.lhs_root = clone_expr(p, k->head.lhs_root);
            p->nrules++;
        }
        if (matched == 0) {
            /* #126: name the nearest miss — the member satisfying the most
             * selector atoms, and the first one it fails (a why-shaped hint) */
            int best = -1, bestn = 0, bestfail = -1;
            for (int vi = 0; vi < p->nvaluedecls; vi++) {
                int n = 0, firstfail = -1;
                for (int q = 0; q < nksel; q++) {
                    ast_atom *ka = &k->body[ksel[q]];
                    pred_info *pi = find_pred(p, ka->pred);
                    uint32_t args[MAX_ARGS];
                    bool ok = true;
                    for (int a = 0; a < pi->arity; a++) {
                        uint32_t nm = ka->args[a].name;
                        if (nm == k->head.pred)
                            args[a] = p->valuedecls[vi].pred;
                        else if (nm == wild ||
                                 var_index(k->vars, k->nvars, nm) >= 0)
                            ok = false;    /* wild / link var: skip for the hint */
                        else
                            args[a] = nm;
                    }
                    bool prv = ok && p->kres &&
                        dl_defeasible(p->kres,
                            dl_pos(ground_pred(p, ka->pred, args,
                                               pi->arity))) == DL_PROVED;
                    if (prv) n++;
                    else if (firstfail < 0) firstfail = ksel[q];
                }
                if (n > bestn) { bestn = n; best = vi; bestfail = firstfail; }
            }
            if (best >= 0 && bestfail >= 0)
                warn(p, k->line, k->col,
                     "modifier '%s' matches no member value — '%s' comes "
                     "closest but is not proved '%s'",
                     k->label, intern_name(p->syms, p->valuedecls[best].pred),
                     intern_name(p->syms, k->body[bestfail].pred));
            else
                warn(p, k->line, k->col,
                     "modifier '%s' matches no member value — its kind atoms "
                     "select nothing (missing `fact`s, or no selected value "
                     "fits the %d-argument subject tuple)", k->label, nsubj);
        }
    }
}

/* #145: kind-level superiority. `halfling_luck > bless` between two functor
 * modifiers is the sentence "Luck applies above Bless, everywhere they meet"
 * — it desugars to the pairwise dotted sups over the INTERSECTION of their
 * member sets (recovered from the expansion's own `A.member` labels), the
 * same erasure story as bands: the engine never learns. An explicit dotted
 * sup between a specific pair — either direction — suppresses the blanket
 * for that member (most-specific wins, as everywhere in the language). A
 * blanket over modifiers that share no member is an orphan-style warning. */
static void desugar_kind_sups(parser *p)
{
    int n0 = p->nsups;
    for (int i = 0; i < n0; i++) {
        ast_sup *s = &p->sups[i];
        ast_rule *ra = find_rule(p, s->a);
        ast_rule *rb = find_rule(p, s->b);
        if (!ra || !rb || !ra->head.is_kinddef || !rb->head.is_kinddef)
            continue;                  /* ground_sup owns every other shape */
        int shared = 0;
        for (int vi = 0; vi < p->nvaluedecls; vi++) {
            const char *vn = intern_name(p->syms, p->valuedecls[vi].pred);
            char la[MAX_NAME], lb[MAX_NAME];
            if (snprintf(la, sizeof la, "%s.%s", s->a, vn) >= MAX_NAME ||
                snprintf(lb, sizeof lb, "%s.%s", s->b, vn) >= MAX_NAME)
                continue;              /* expansion already errored the label */
            if (!find_rule(p, la) || !find_rule(p, lb))
                continue;              /* not a shared member */
            shared++;
            bool explicit_ = false;
            for (int j = 0; j < n0 && !explicit_; j++) {
                if (j == i) continue;
                explicit_ =
                    (strcmp(p->sups[j].a, la) == 0 &&
                     strcmp(p->sups[j].b, lb) == 0) ||
                    (strcmp(p->sups[j].a, lb) == 0 &&
                     strcmp(p->sups[j].b, la) == 0);
            }
            if (explicit_)
                continue;              /* most-specific wins */
            if (p->nsups >= MAX_SUPS) {
                serr(p, s->aline, s->acol,
                     "too many superiority edges (max %d) desugaring "
                     "'%s > %s' — raise MAX_SUPS", MAX_SUPS, s->a, s->b);
                return;
            }
            ast_sup *d = &p->sups[p->nsups++];
            memset(d, 0, sizeof *d);
            snprintf(d->a, MAX_NAME, "%s", la);
            snprintf(d->b, MAX_NAME, "%s", lb);
            d->aline = s->aline; d->acol = s->acol;
            d->bline = s->bline; d->bcol = s->bcol;
        }
        if (shared == 0)
            warn(p, s->aline, s->acol,
                 "'%s > %s' orders kind modifiers that share no member value "
                 "— it does nothing", s->a, s->b);
    }
}

/* The sort of one head argument: a rule parameter carries its own, a ground
 * entity its declaration's, an integer literal the `int` sentinel. */
static int head_arg_sort(parser *p, ast_rule *r, const ast_arg *a)
{
    if (a->is_int) return INT_SORT;
    int vi = var_index(r->vars, r->nvars, a->name);
    if (vi >= 0) return r->vars[vi].sort;
    int ei = find_entity(p, a->name);
    return ei >= 0 ? p->ents[ei].sort : -1;
}

/* #205: a judgment has no declaration site, so its argument sorts are INFERRED
 * from the rules that conclude it — and a predicate has ONE signature. Rules
 * concluding `shows(hero, …)` and `shows(rusty_key, …)` are a located error
 * rather than a narrowing to whichever came first, because the signature
 * settled here is what the §6.3 artifact publishes: a generic client crossing
 * the published domains would never ask about the sort that got dropped, and
 * every atom it skips reads as "not proved" rather than "never asked" — the
 * silent-always-false failure the artifact exists to end. The two predicates
 * the author writes instead are explicit; a smaller published world is not. */
static void infer_head_sorts(parser *p)
{
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->head.is_valuedef || r->head.is_kinddef || rule_is_kind(p, r))
            continue;                              /* not a boolean conclusion */
        pred_info *pi = find_pred(p, r->head.pred);
        if (!pi || !pi->is_head || pi->arity != r->head.nargs) continue;
        if (pi->is_fluent || pi->is_provider || pi->is_emit || pi->is_value ||
            pi->is_kindpred) continue;             /* declared: it has a schema */
        for (int k = 0; k < r->head.nargs; k++) {
            int s = head_arg_sort(p, r, &r->head.args[k]);
            if (s < 0 && s != INT_SORT) continue;  /* unresolved: reported already */
            if (pi->headsort[k] == -1) {           /* first rule to pin it */
                pi->headsort[k] = s;
                pi->headrule[k] = i;
            } else if (pi->headsort[k] != s) {
                /* #231: two rules concluding at different sorts pin the COVER
                 * when one is declared over both — the join in the cover
                 * lattice. `shows(hero, …)` and `shows(key, …)` under a
                 * `drawable union actor, item` are one predicate, which is the
                 * whole point of a cover. Ambiguity is refused rather than
                 * guessed: two covers admitting both leaves no least answer. */
                int join = -1, njoin = 0;
                for (int u = 0; u < p->nsorts; u++)
                    if (p->sorts[u].is_union &&
                        sort_admits(p, u, s) && sort_admits(p, u, pi->headsort[k]))
                        { join = u; njoin++; }
                if (njoin == 1) { pi->headsort[k] = join; continue; }
                ast_rule *first = &p->rules[pi->headrule[k]];
                if (njoin > 1)
                    serr(p, r->head.args[k].line, r->head.args[k].col,
                         "argument %d of '%s' is '%s' here and '%s' at line %d, "
                         "and more than one union covers both — say which by "
                         "declaring the signature",
                         k + 1, intern_name(p->syms, r->head.pred),
                         arg_sort_name(p, s), arg_sort_name(p, pi->headsort[k]),
                         first->head.args[k].line);
                else
                    serr(p, r->head.args[k].line, r->head.args[k].col,
                         "argument %d of '%s' is '%s' here but '%s' at line %d — a "
                         "predicate has one signature; conclude the other sort "
                         "under its own predicate, or declare a union covering "
                         "both (`sort <name> union %s, %s`)",
                         k + 1, intern_name(p->syms, r->head.pred),
                         arg_sort_name(p, s), arg_sort_name(p, pi->headsort[k]),
                         first->head.args[k].line,
                         arg_sort_name(p, pi->headsort[k]), arg_sort_name(p, s));
            }
        }
    }
}

/* Does an argument of sort `got` satisfy a signature position of sort `want`?
 * One predicate rather than an `==` at each site, because sort union (#231) —
 * a declared COVER, `sort thing union actor, item` — makes admission a
 * question with a real answer: is `got` one of `want`'s members?
 *
 * A cover is not `<:` inheritance. It admits its members' entities and adds
 * none of its own, so it is exactly the "everything placed on the map" case
 * that otherwise needs one predicate per sort. Members are base sorts, so an
 * entity belongs to exactly one, and admission is a flat membership test
 * rather than a lattice walk. */
static bool sort_admits(parser *p, int want, int got)
{
    if (want == got) return true;
    if (want < 0 || want >= p->nsorts || got < 0) return false;
    if (!p->sorts[want].is_union) return false;
    for (int i = 0; i < p->sorts[want].nmem; i++)
        if (p->sorts[want].mem[i] == got) return true;
    return false;
}

/* #217: settle the judgment reads parked by `check_pred_args`, now that every
 * rule has been seen and each judgment's signature is fixed. Reading a
 * judgment at the wrong sort names an atom no rule can conclude, so the
 * condition never holds and the rule never fires — the same always-false
 * silence #205 closed on the concluding side, pointing the other way: there it
 * was the artifact seeing a smaller world than the engine, here it is a rule
 * the author believes is live. Nothing concludes the predicate at all is the
 * ORPHAN pass's business (it warns), not this one's. */
static void check_head_reads(parser *p)
{
    for (int i = 0; i < p->nhreads; i++) {
        head_read *hr = &p->hreads[i];
        pred_info *pi = &p->preds[hr->pred];
        if (hr->arg >= pi->arity) continue;
        int want = pi->headsort[hr->arg];
        if (want == -1) continue;                  /* no rule pinned this slot */
        if (!sort_admits(p, want, hr->got))
            serr(p, hr->line, hr->col,
                 "argument %d of '%s' expects sort '%s' (from the rules that "
                 "conclude it) but got '%s' — nothing concludes the atom this "
                 "names, so the condition never holds",
                 hr->arg + 1, intern_name(p->syms, hr->pred),
                 arg_sort_name(p, want), arg_sort_name(p, hr->got));
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

    check_kfacts(p);                   /* #124: membership vocabulary first */
    solve_kind_stratum(p);             /* #125: the taxonomy solves at build */
    expand_kind_rules(p);              /* #124 kinds-are-facts: before defs register */
    desugar_kind_sups(p);              /* #145: blanket modifier ordering */

    /* #124 staging boundary: a `value`-sorted binder exists only to feed a
     * functor-modifier head this slice — anywhere else (a rule concluding a
     * kind, an action over values) is the derived-kind stratum, #125 */
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->head.is_kinddef || rule_is_kind(p, r)) continue;
        for (int v = 0; v < r->nvars; v++)
            if (r->vars[v].sort == SORT_METAVALUE ||
                r->vars[v].sort == -(int)p->metaval - 2)
                serr(p, r->vars[v].line, r->vars[v].col,
                     "a `value`-sorted parameter belongs to a kind rule "
                     "(`… => k(V)`) or a functor-modifier head (`V(A) = …`) — "
                     "this rule is neither");
    }
    for (int i = 0; i < p->nactions; i++)
        for (int v = 0; v < p->actions[i].nvars; v++)
            if (p->actions[i].vars[v].sort == SORT_METAVALUE ||
                p->actions[i].vars[v].sort == -(int)p->metaval - 2)
                serr(p, p->actions[i].vars[v].line, p->actions[i].vars[v].col,
                     "an action cannot range over `value` — values are "
                     "build-time vocabulary, not runtime objects");

    /* value definitions register first (#82), so any read checked below can
     * see whether its definition exists regardless of declaration order */
    for (int i = 0; i < p->nrules; i++)
        if (p->rules[i].head.is_valuedef) register_valuedef(p, i);

    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->head.is_kinddef) {
            /* validated + expanded in expand_kind_rules; only labels here */
            for (int j = i + 1; j < p->nrules; j++)
                if (strcmp(r->label, p->rules[j].label) == 0)
                    serr(p, p->rules[j].line, p->rules[j].col,
                         "duplicate rule label '%s'", r->label);
            continue;
        }
        if (r->head.is_valuedef) {
            check_valuedef(p, i);
            for (int j = i + 1; j < p->nrules; j++)
                if (strcmp(r->label, p->rules[j].label) == 0)
                    serr(p, p->rules[j].line, p->rules[j].col,
                         "duplicate rule label '%s'", r->label);
            continue;
        }
        if (rule_is_kind(p, r)) {      /* #125: validated in solve_kind_stratum */
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

    infer_head_sorts(p);               /* #205: one signature per judgment */
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
        p->in_ramif_eff = a->is_ramif;         /* primed reads legal here (#84) */
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
        p->in_ramif_eff = false;
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

    /* every read has been walked by now (rules above, actions and binders just
     * past them), so the deferred judgment reads can be settled against the
     * signatures — #205 fixes those, #217 checks the reads against them */
    check_head_reads(p);
    check_partial_arith(p);            /* #116 static safety rule */
    check_exclusives(p);               /* #159 exclusivity groups */
    check_bands(p);
    stratify_steps(p);                 /* §5.8 strata + cycle rejection (#87) */
}

/* ---- grounding: emit ground rules into world_* ---------------------- */

/* Write the ground term "pred(e1,e2)" (bare "pred" at arity 0) into buf. */
/* Takes the intern table directly (not the parser) so the tick-time matcher
 * (#28) can reuse the EXACT ground-atom spelling post-compile — byte-identical
 * atoms and why-traces by construction. */
/* snprintf-compatible append: copies what fits (always NUL-terminating) and
 * returns the length it WANTED to write, so a caller accumulates an offset
 * exactly as it did with snprintf. Unlike the snprintf chain it replaces, this
 * stays safe once the offset passes `cap` — that chain computed `cap - off` as
 * a size_t, which underflowed. */
static int append_term(char *buf, size_t cap, int off, const char *s)
{
    size_t l = strlen(s);
    if (off >= 0 && (size_t)off < cap) {
        size_t room = cap - (size_t)off - 1;
        size_t c = l < room ? l : room;
        memcpy(buf + off, s, c);
        buf[(size_t)off + c] = '\0';
    }
    return (int)l;
}

/* Spell a ground atom "pred(e1,e2)" into buf. HOT: once per instance in the
 * eager grounder, and once per match per tick in the tick-time matcher, where
 * it plus intern_id is ~85% of a re-ground. Hand-written rather than a chain of
 * snprintf calls, each of which re-parses a format string to copy one string.
 * Output is byte-identical, which is load-bearing: ground atom spellings are
 * the ABI a host interning the same name relies on (§6.3). */
static int build_term(intern *syms, uint32_t pred, const uint32_t *args, int n,
                      char *buf, size_t cap)
{
    int off = append_term(buf, cap, 0, intern_name(syms, pred));
    if (n == 0) return off;
    off += append_term(buf, cap, off, "(");
    for (int i = 0; i < n && off < (int)cap; i++) {
        if (i) off += append_term(buf, cap, off, ",");
        off += append_term(buf, cap, off, intern_name(syms, args[i]));
    }
    if (off < (int)cap) off += append_term(buf, cap, off, ")");
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
    case EX_ENT:   return false;              /* an entity handle, not an int (#258) */
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

/* Render expr node `e` under `binding` back into source-like text (#132): the
 * author's own words for a guard, so a why-trace can say
 * `atk_die(grunk) + atk_mod(grunk) >= ac(vera)` instead of naming the synthetic
 * marker atom the guard compiled to. Precedence-aware, so it parenthesises only
 * where the spelling would otherwise change meaning. Reads the SAME binding the
 * bytecode does, so the text and the code cannot describe different things.
 * Truncation is clamped by `append_term`, never silent past the buffer. */
static int expr_prec(ex_kind k)
{
    switch (k) {
    case EX_ADD: case EX_SUB: return 1;
    case EX_MUL: case EX_DIV: return 2;
    case EX_NEG:              return 3;
    default:                  return 4;    /* leaves and call-like forms */
    }
}

static void render_expr(parser *p, int e, var_bind *vars, int nvars,
                        const uint32_t *binding, char *buf, size_t cap, int *off)
{
    ex_node *n = &p->exprs[e];
    char tmp[64];
    switch (n->kind) {
    case EX_CONST:
        snprintf(tmp, sizeof tmp, "%ld", n->konst);
        *off += append_term(buf, cap, *off, tmp);
        return;
    case EX_PRIOR:
        *off += append_term(buf, cap, *off, "prior");
        return;
    case EX_ENT: {                     /* #258: the entity BOUND here, by name —
                                        * `chebyshev(scout, sentry) <= 3` is what
                                        * makes a measurement explicable, where
                                        * the variable would say nothing */
        ast_arg ea = { 0 }; ea.name = n->pred;
        uint32_t ent = resolve_arg(vars, nvars, binding, ea);
        *off += append_term(buf, cap, *off, intern_name(p->syms, ent));
        return;
    }
    case EX_ROLL:
        snprintf(tmp, sizeof tmp, "roll(%ld)", n->konst);
        *off += append_term(buf, cap, *off, tmp);
        return;
    case EX_LOAD: case EX_TEST: case EX_CALL: {
        if (n->kind == EX_TEST) {
            *off += append_term(buf, cap, *off, "test(");
            if (n->konst) *off += append_term(buf, cap, *off, "~");
        }
        *off += append_term(buf, cap, *off, intern_name(p->syms, n->pred));
        if (n->kind == EX_CALL) {
            *off += append_term(buf, cap, *off, "(");
            for (int k = 0; k < n->nargs; k++) {
                if (k) *off += append_term(buf, cap, *off, ", ");
                render_expr(p, n->cargs[k], vars, nvars, binding, buf, cap, off);
            }
            *off += append_term(buf, cap, *off, ")");
        } else if (n->nargs > 0) {
            *off += append_term(buf, cap, *off, "(");
            for (int k = 0; k < n->nargs; k++) {
                if (k) *off += append_term(buf, cap, *off, ",");
                uint32_t a = resolve_arg(vars, nvars, binding, n->args[k]);
                *off += append_term(buf, cap, *off, intern_name(p->syms, a));
            }
            *off += append_term(buf, cap, *off, ")");
        }
        if (n->nprimed) *off += append_term(buf, cap, *off, "'");
        if (n->kind == EX_TEST) *off += append_term(buf, cap, *off, ")");
        return;
    }
    case EX_MIN: case EX_MAX:
        *off += append_term(buf, cap, *off, n->kind == EX_MIN ? "min(" : "max(");
        render_expr(p, n->lhs, vars, nvars, binding, buf, cap, off);
        *off += append_term(buf, cap, *off, ", ");
        render_expr(p, n->rhs, vars, nvars, binding, buf, cap, off);
        *off += append_term(buf, cap, *off, ")");
        return;
    case EX_NEG:
        *off += append_term(buf, cap, *off, "-");
        goto unary;
    default: break;
    }
    {   /* the binary arithmetic set */
        const char *opsym = n->kind == EX_ADD ? " + " : n->kind == EX_SUB ? " - "
                          : n->kind == EX_MUL ? " * " : " / ";
        int me = expr_prec(n->kind);
        bool lp = expr_prec(p->exprs[n->lhs].kind) < me;
        /* a right operand at EQUAL precedence still needs parens under the
         * non-associative operators: `a - (b - c)` is not `a - b - c` */
        bool rp = expr_prec(p->exprs[n->rhs].kind) < me ||
                  (expr_prec(p->exprs[n->rhs].kind) == me &&
                   (n->kind == EX_SUB || n->kind == EX_DIV));
        if (lp) *off += append_term(buf, cap, *off, "(");
        render_expr(p, n->lhs, vars, nvars, binding, buf, cap, off);
        if (lp) *off += append_term(buf, cap, *off, ")");
        *off += append_term(buf, cap, *off, opsym);
        if (rp) *off += append_term(buf, cap, *off, "(");
        render_expr(p, n->rhs, vars, nvars, binding, buf, cap, off);
        if (rp) *off += append_term(buf, cap, *off, ")");
        return;
    }
unary:
    {
        bool par = expr_prec(p->exprs[n->lhs].kind) < expr_prec(EX_NEG);
        if (par) *off += append_term(buf, cap, *off, "(");
        render_expr(p, n->lhs, vars, nvars, binding, buf, cap, off);
        if (par) *off += append_term(buf, cap, *off, ")");
    }
}

/* Emit RPN bytecode for expr node `e` under `binding`, folding constant
 * subtrees and resolving fluent reads to their ground value-store atom. */
static void emit_expr(parser *p, int e, var_bind *vars, int nvars,
                      const uint32_t *binding, expr_ins *code, int *pos)
{
    long cv;
    if (expr_fold(p, e, &cv)) {
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_CONST; code[(*pos)++].arg = cv; } else p->code_of = true;
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
        if (*pos < MAX_CODE) {
            code[*pos].op = n->nprimed ? EXPR_LOADN : EXPR_LOAD;   /* #84 */
            code[(*pos)++].arg = (long)g;
        } else p->code_of = true;
        return;
    }
    if (n->kind == EX_ENT) {                   /* #258: the entity, as its atom */
        ast_arg ea = { 0 }; ea.name = n->pred;
        uint32_t ent = resolve_arg(vars, nvars, binding, ea);
        if (*pos < MAX_CODE) {
            code[*pos].op = EXPR_CONST;
            code[(*pos)++].arg = (long)ent;
        } else p->code_of = true;
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
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_ROLL; code[(*pos)++].arg = (long)idx; } else p->code_of = true;
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
        } else p->code_of = true;
        return;
    }
    if (n->kind == EX_PRIOR) {                 /* the chain's running value (#82) */
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_P; code[(*pos)++].arg = 0; } else p->code_of = true;
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
        } else p->code_of = true;
        return;
    }
    if (n->kind == EX_NEG) {
        emit_expr(p, n->lhs, vars, nvars, binding, code, pos);
        if (*pos < MAX_CODE) { code[*pos].op = EXPR_NEG; code[(*pos)++].arg = 0; } else p->code_of = true;
        return;
    }
    emit_expr(p, n->lhs, vars, nvars, binding, code, pos);
    emit_expr(p, n->rhs, vars, nvars, binding, code, pos);
    expr_op op = n->kind == EX_ADD ? EXPR_ADD : n->kind == EX_SUB ? EXPR_SUB
               : n->kind == EX_MUL ? EXPR_MUL : n->kind == EX_DIV ? EXPR_DIV
               : n->kind == EX_MIN ? EXPR_MIN                     : EXPR_MAX;
    if (*pos < MAX_CODE) { code[*pos].op = op; code[(*pos)++].arg = 0; } else p->code_of = true;
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

/* Ground the `defined(v(args))` judgment for one binding (#116): one
 * defeasible rule per prior-free definition — `marker => defined(v(…))`, the
 * disjunction via shared heads — deduped by the defined atom. A prior-bearing
 * definition contributes nothing (it propagates undefinedness, never grounds
 * it). Over a value with an unconditional base the atom is trivially true
 * (one bodyless rule); the check pass already warned. An ordinary literal:
 * queryable, `why?`-traceable, usable in any body or step condition. */
static uint32_t ensure_defined_grounded(parser *p, uint32_t pred,
                                        const uint32_t *args, int nargs)
{
    int vi = find_value(p, pred);
    uint32_t vg = ground_pred(p, pred, args, nargs);
    char nm[MAX_GROUND + 12];
    snprintf(nm, sizeof nm, "defined(%s)", intern_name(p->syms, vg));
    uint32_t g = intern_id(p->syms, nm);
    if (g < p->vdefd_cap && p->vdefd_of[g] >= 0) return g;
    atom_map_set(&p->vdefd_of, &p->vdefd_cap, g, 1);
    if (vi < 0) return g;
    char pbuf[MAX_NAME + 24];
    if (!p->value_partial[vi]) {
        dl_lit none = dl_pos(g);                   /* unused at nbody = 0 */
        int h = world_add_rule(p->w, nm, DL_DEFEASIBLE, dl_pos(g), &none, 0);
        world_set_rule_prov(p->w, h,
            prov_str(p, p->valuedecls[vi].line, pbuf, sizeof pbuf));
        return g;
    }
    for (int d = 0; d < p->nvdefs[vi]; d++) {
        int ri = p->vdefs[vi][d];
        if (expr_has_prior(p, p->rules[ri].head.lhs_root))
            continue;                              /* propagates, never grounds */
        uint32_t m = ensure_marker_grounded(p, ri, args);
        dl_lit b = dl_pos(m);
        char rn[MAX_GROUND + 12];
        snprintf(rn, sizeof rn, "%s.defines", intern_name(p->syms, m));
        int h = world_add_rule(p->w, rn, DL_DEFEASIBLE, dl_pos(g), &b, 1);
        world_set_rule_prov(p->w, h,
            prov_str(p, p->rules[ri].line, pbuf, sizeof pbuf));
    }
    return g;
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
    if (vi < 0 || p->nvdefs[vi] == 0) return;  /* no defs: reported in check pass */
    /* A lookup-table row speaks for one instance and beats the catch-all
     * beneath it; with no row and no catch-all this instance is genuinely
     * undefined, which is what the partial epilogue is for (#116). */
    int base = p->value_def[vi];
    for (int i = 0; i < p->value_nrows[vi]; i++)
        if (vdef_applies(&p->rules[p->value_rows[vi][i]], rargs)) {
            base = p->value_rows[vi][i];
            break;
        }
    bool partial = p->value_partial[vi] || (base < 0 && p->value_nrows[vi] > 0);
    if (base < 0 && !partial) return;          /* reported in the check pass */
    if (++p->vdepth > MAX_ARGS * 2) { p->vdepth--; return; }   /* cycle backstop */
#define VEMIT(o, a) do { if (*pos < MAX_CODE) {         code[*pos].op = (o); code[(*pos)++].arg = (a); } else p->code_of = true; } while (0)
    if (base >= 0) {
        ast_rule *bd = &p->rules[base];
        uint32_t sub[MAX_ARGS] = { 0 };
        emit_valuedef_sub(p, bd, rargs, sub);
        emit_expr(p, bd->head.lhs_root, bd->vars, bd->nvars, sub, code, pos);
    } else {
        /* partial (#116): no base — a masked placeholder; the REQDEF epilogue
         * below refuses to let an undefined chain deliver it */
        VEMIT(EXPR_CONST, 0);
    }
    for (int L = 0; L < p->value_nlayers[vi]; L++) {
        int ri = p->value_layers[vi][L];
        ast_rule *ld = &p->rules[ri];
        if (!vdef_applies(ld, rargs)) continue;
        uint32_t lsub[MAX_ARGS] = { 0 };
        emit_valuedef_sub(p, ld, rargs, lsub);
        long targ = (long)ensure_marker_grounded(p, ri, rargs) << 1;
        VEMIT(EXPR_PPUSH, 0);                  /* v -> prior slot   */
        VEMIT(EXPR_P, 0);                      /* [v]               */
        VEMIT(EXPR_TEST, targ);                /* [v, t]            */
        {   /* #116: a nested partial read inside f only demands definedness
             * when THIS layer fired — record the marker for its epilogue */
            bool pushed = p->nencl < 2 * MAX_ARGS;
            if (pushed) p->encl_marks[p->nencl++] = targ;
            emit_expr(p, ld->head.lhs_root, ld->vars, ld->nvars, lsub, code, pos);
            if (pushed) p->nencl--;
        }
        VEMIT(EXPR_MUL, 0);                    /* [v, t*f]          */
        VEMIT(EXPR_ADD, 0);                    /* [v + t*f]         */
        VEMIT(EXPR_TEST, targ);
        VEMIT(EXPR_P, 0);
        VEMIT(EXPR_MUL, 0);                    /* [.., t*v]         */
        VEMIT(EXPR_SUB, 0);                    /* [v + t*(f - v)]   */
        VEMIT(EXPR_PPOP, 0);
    }
    if (partial) {
        /* Every read site of a partial value also grounds its `defined(…)`
         * judgment (#116): the atom is part of the value's observable
         * surface — hosts and `why?` can always ask it, and the M2 boundary
         * reads a partial value as the option pair (defined, value). */
        ensure_defined_grounded(p, pred, rargs, p->valuedecls[vi].nargs);
        /* Definedness epilogue (#116): push
         *     x = OR(prior-free layer markers) + Σ(1 − test(enclosing marker))
         * then EXPR_REQDEF pops x and flags the evaluation UNDEFINED iff
         * x <= 0 — no grounding layer fired AND every enclosing layer did
         * (the evaluate-all-and-mask shape must not poison masked-off
         * branches). A prior-bearing layer never grounds the chain: `prior`
         * over nothing PROPAGATES undefinedness (Bless on a save that does
         * not exist — still does not exist). */
        int nd = 0;
        for (int L = 0; L < p->value_nlayers[vi]; L++) {
            int ri = p->value_layers[vi][L];
            if (expr_has_prior(p, p->rules[ri].head.lhs_root))
                continue;
            long targ = (long)ensure_marker_grounded(p, ri, rargs) << 1;
            VEMIT(EXPR_TEST, targ);
            if (nd++) VEMIT(EXPR_MAX, 0);
        }
        for (int k = 0; k < p->nencl; k++) {
            VEMIT(EXPR_CONST, 1);
            VEMIT(EXPR_TEST, p->encl_marks[k]);
            VEMIT(EXPR_SUB, 0);
            VEMIT(EXPR_ADD, 0);
        }
        VEMIT(EXPR_REQDEF, 0);
    }
#undef VEMIT
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
    if (at->is_defined) {                          /* `defined v(args)` (#116) */
        uint32_t dargs[MAX_ARGS];
        for (int k = 0; k < at->nargs; k++)
            dargs[k] = resolve_arg(vars, nvars, binding, at->args[k]);
        uint32_t g = ensure_defined_grounded(p, at->pred, dargs, at->nargs);
        return at->neg ? dl_neg(g) : dl_pos(g);    /* neg rejected in checks */
    }
    if (at->is_expr_guard) {                       /* `expr <op> expr` — e.g. the d20 */
        expr_ins lcode[MAX_CODE], rcode[MAX_CODE];
        int nl = 0, nr = 0;
        p->code_of = false;
        emit_expr(p, at->lhs_root, vars, nvars, binding, lcode, &nl);
        emit_expr(p, at->rhs_root, vars, nvars, binding, rcode, &nr);
        if (p->code_of) {
            serr(p, at->line, at->col,
                 "guard expression too long — its compiled form (including "
                 "inlined value chains) exceeds %d VM instructions; simplify "
                 "the expression or the value's layers", MAX_CODE);
            p->code_of = false;
        }
        char label[24], nm[MAX_GROUND];
        snprintf(label, sizeof label, "eg%d", at->lhs_root);   /* per guard occurrence */
        inst_name(p, nm, sizeof nm, label, vars, nvars, binding);
        uint32_t g = intern_id(p->syms, nm);
        world_add_expr_guard(p->w, g, lcode, nl, rcode, nr, at->cmp);
        {   /* the author's spelling, for the why-trace (#132) — a guard that
             * renders as `eg14[A=grunk,T=vera]` makes the reader do archaeology */
            char src[MAX_GROUND];
            int off = 0;
            src[0] = '\0';
            render_expr(p, at->lhs_root, vars, nvars, binding, src, sizeof src, &off);
            off += append_term(src, sizeof src, off, " ");
            off += append_term(src, sizeof src, off, cmp_spelling(at->cmp));
            off += append_term(src, sizeof src, off, " ");
            render_expr(p, at->rhs_root, vars, nvars, binding, src, sizeof src, &off);
            world_set_expr_guard_src(p->w, g, src);
        }
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
                    p->code_of = false;
                    if (f->rmin_expr >= 0)
                        emit_expr(p, f->rmin_expr, kv, f->nargs, binding, lo, &nlo);
                    if (f->rmax_expr >= 0)
                        emit_expr(p, f->rmax_expr, kv, f->nargs, binding, hi, &nhi);
                    if (p->code_of) {
                        serr(p, f->line, f->col,
                             "clamp bound expression too long — its compiled "
                             "form exceeds %d VM instructions", MAX_CODE);
                        p->code_of = false;
                    }
                    world_set_num_clamp(p->w, atom,
                                        f->rmin_expr >= 0 ? lo : NULL, nlo,
                                        f->rmax_expr >= 0 ? hi : NULL, nhi);
                }
            }
            else if (f->is_mv) {                   /* one boolean atom per value */
                uint32_t vat[MAX_DOMAIN];
                for (int v = 0; v < f->nvalues; v++) {
                    uint32_t a = ground_mv_atom(p, f->pred, binding, f->nargs,
                                                f->values[v]);
                    world_declare_fluent(p->w, a);
                    world_set_fluent_prov(p->w, a, decl);
                    vat[v] = a;
                }
                /* `split` (#121): hand the world the family's value atoms —
                 * arity-0 (parse-enforced), so this binding loop runs once */
                if (f->is_split &&
                    world_set_split(p->w, vat, f->nvalues) != 0)
                    fail(p, f->line, f->col,
                         "internal: the world rejected `split` on '%s'",
                         intern_name(p->syms, f->pred));
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

/* Would eager grounding of this rule blow the cardinality ceiling? Split out of
 * ground_rule so the router (#59) can ask BEFORE committing to the eager path. */
static bool rule_over_cap(parser *p, ast_rule *r)
{
    if (r->head.is_valuedef || r->head.is_kinddef) return false;
    if (rule_is_kind(p, r)) return false;
    bool of = false;
    (void)instance_count(p, r->vars, r->nvars, &of);
    return of;
}

static void ground_rule(parser *p, ast_rule *r)
{
    if (r->head.is_valuedef || r->head.is_kinddef) return;   /* #82: never dl rules */
    if (rule_is_kind(p, r)) return;    /* #125: solved at build, never runtime */
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
             "rule '%s' grounds to more than %d instances and cannot be matched "
             "at tick time — every variable must be bound by a positive base-fluent "
             "atom for that; add a sparser anchor or split the sorts "
             "(§5.2 cardinality cap)",
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
    if (r->head.is_valuedef || r->head.is_kinddef) return false;   /* #82 */
    if (rule_is_kind(p, r)) return false;              /* #125: build-time */
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
    /* #257: a generator-capable provider can BIND its free argument instead of
     * filtering a pair the join already formed. `gen_yield` is what it produced
     * last reground and `gen_off` turns it back into a filter when it turned
     * out to be denser than the fluent scan it displaced — §8.1's adaptive
     * lever rather than a guess that is wrong invisibly. */
    long      gen_yield;
    long      gen_runs;
    bool      gen_off;
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

/* A provider that can ENUMERATE (#254) binds its free argument rather than
 * testing a pair the join already built. Usable only in the connected position
 * — exactly one argument bound, the other free — because the protocol asks
 * `pred(a, ·)` for a known `a`. A var reachable ONLY through a provider still
 * leaves the rule eager: enumerability is a runtime registration and the
 * compile-time eligibility check cannot see it, so this is an optimisation
 * inside an already-matchable rule, never a widening of what matches. */
static bool m_provider_generator(const story_matcher *m, const m_rule *r,
                                 const m_atom *a, const bool *vb, int *pbound,
                                 int *pfree)
{
    if (a->neg || a->is_guard || !a->is_provider || a->gen_off) return false;
    if (a->nargs != 2 || !world_provider_generates(m->w, a->pred)) return false;
    int v0 = m_var_index((m_rule *)r, a->arg[0]);
    int v1 = m_var_index((m_rule *)r, a->arg[1]);
    if (v0 < 0 || v1 < 0 || v0 == v1) return false;   /* a constant arg: later */
    if (vb[v0] == vb[v1]) return false;               /* both or neither bound */
    *pbound = vb[v0] ? v0 : v1;
    *pfree  = vb[v0] ? v1 : v0;
    return true;
}

/* Selectivity-ordered join plan (#46): visit the generator atoms smallest live
 * extension first (factindex_count), each connected to an already-bound var so we
 * never form an accidental cartesian sub-join. The order is a deterministic
 * function of the current extension sizes (I4). Reordering is semantically
 * invisible — the join commutes, so the same instance set is produced — it only
 * shrinks the intermediate bindings walked. `gorder` gets the body indices of the
 * generators in visitation order; filters (negation, guards, providers) are not
 * ordered here — they apply at the leaf / to the solver. */
static void plan_generators(const story_matcher *m, m_rule *r,
                            const factindex *ix, int *gorder, uint8_t *gkind,
                            int *pngen)
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
        gorder[ngen] = best; gkind[ngen] = 0; ngen++;
        used[best] = true;
        for (int k = 0; k < r->body[best].nargs; k++) {
            int vi = m_var_index(r, r->body[best].arg[k]);
            if (vi >= 0) vb[vi] = true;
        }
        /* #257: a provider that can enumerate binds its free argument as soon
         * as its other one is bound. Inserted rather than substituted — every
         * fluent scan stays in the plan, so no constraint is lost and a scan
         * whose var is now bound degenerates to a point probe. That is where
         * the win is: |F| x run intermediates instead of |F|^2. */
        for (int b = 0; b < r->nbody; b++) {
            int bound, freev;
            if (used[b]) continue;
            if (!m_provider_generator(m, r, &r->body[b], vb, &bound, &freev))
                continue;
            gorder[ngen] = b; gkind[ngen] = 1; ngen++;
            used[b] = true;
            vb[freev] = true;
        }
    }
    *pngen = ngen;
}

/* Semi-naïve nested-loop join over the compact plan and a live fact index,
 * visiting generators in the selectivity order `gorder` (see plan_generators).
 * At the leaf every var is bound; the negated fluent filters are applied, then
 * the instance is emitted with its full body. */
static void m_match_rec(story_matcher *m, m_rule *r, const int *gorder,
                        const uint8_t *gkind, int ngen,
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
    if (gkind[gi]) {                        /* #257: enumerate from the provider */
        int v0 = m_var_index(r, at->arg[0]), v1 = m_var_index(r, at->arg[1]);
        int bv = bind[v0] != INTERN_NONE ? v0 : v1;
        int fv = bv == v0 ? v1 : v0;
        int cap = world_provider_gen(m->w, at->pred, bind[bv], NULL, 0);
        uint32_t *run = malloc((size_t)(cap ? cap : 1) * sizeof *run);
        int got = world_provider_gen(m->w, at->pred, bind[bv], run, cap);
        at->gen_runs++;
        at->gen_yield += got;
        for (int k = 0; k < got; k++) {
            m->probes++;
            bind[fv] = run[k];
            m_match_rec(m, r, gorder, gkind, ngen, gi + 1, bind, ix);
        }
        bind[fv] = INTERN_NONE;
        free(run);
        return;
    }
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
        if (ok) m_match_rec(m, r, gorder, gkind, ngen, gi + 1, bind, ix);
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
    d->gen_yield = 0; d->gen_runs = 0; d->gen_off = false;   /* #257 */
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
    /* #45: tell the world which predicates this rule READS, so a base-fact edit
     * that cannot change the match set does not force a re-ground. Every body
     * atom is registered regardless of kind — negated bodies decide a match just
     * as positive ones do, and a guard's numeric fluent is read the same way.
     * The head goes in too: a matchable rule may read another's conclusion once
     * derived bodies land (#44), and over-registering only costs a re-ground. */
    world_matcher_watch(p->w, r->head.pred);
    for (int b = 0; b < r->nbody && b < MAX_BODY; b++)
        world_matcher_watch(p->w, r->body[b].pred);
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
        uint8_t gkind[MAX_BODY];
        plan_generators(m, r, ix, gorder, gkind, &ngen); /* order for THIS tick */
        m_match_rec(m, r, gorder, gkind, ngen, 0, bind, ix);
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

/* Emit one grounded numeric effect. */
static void emit_num_effect(parser *p, int rule, const ast_atom *e,
                            uint32_t num, uint32_t subject,
                            const expr_ins *code, int nc)
{
    (void)subject;
    world_add_num_effect(p->w, rule, num, e->numop, code, nc);
}

/* A burst cue is declared where it is FIRED (#11), not by cross product: the
 * ground atoms that exist are exactly the ones some rule instance can emit, so
 * a cue over a sort costs one atom per firing instance and an unused arm of the
 * sort costs nothing. Idempotent in the world, so repeats are free. */
static void declare_if_emit(parser *p, uint32_t pred, uint32_t ground)
{
    pred_info *pi = find_pred(p, pred);
    if (pi && pi->is_emit)
        world_declare_emit(p->w, ground);
}

static void ground_action(parser *p, ast_action *a)
{
    a->srule_lo = a->srule_hi = world_step_rule_count(p->w);
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
                eff[ne] = ground_lit(p, e, a->vars, a->nvars, binding);
                declare_if_emit(p, e->pred, eff[ne].atom);   /* #11 */
                ne++;
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
        /* the same identity the instance name spells, kept STRUCTURED (#88):
         * a commit receipt hands the client `strike` + `A=hero, T=goblin`
         * rather than a string to parse back apart */
        {
            uint32_t vn[MAX_ARGS];
            for (int k = 0; k < a->nvars; k++) vn[k] = a->vars[k].name;
            world_set_step_binding(p->w, h, intern_id(p->syms, a->name),
                                   vn, binding, a->nvars);
        }

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
            p->code_of = false;
            emit_expr(p, e->expr_root, a->vars, a->nvars, binding, code, &nc);
            if (p->code_of) {
                serr(p, e->line, e->col,
                     "effect expression too long — its compiled form "
                     "(including inlined value chains) exceeds %d VM "
                     "instructions; simplify the expression or the value's "
                     "layers", MAX_CODE);
                p->code_of = false;
            }
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
                            eff2[ne2] = ground_lit(p, e, cv, ncv, cb);
                            declare_if_emit(p, e->pred, eff2[ne2].atom);   /* #11 */
                            ne2++;
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
                    {   /* #88: the cast's binding EXTENDED by the binder's —
                         * one Fireball target's row names both C and T */
                        uint32_t vn[2 * MAX_ARGS];
                        int nvn = ncv < 2 * MAX_ARGS ? ncv : 2 * MAX_ARGS;
                        for (int k = 0; k < nvn; k++) vn[k] = cv[k].name;
                        world_set_step_binding(p->w, h2, intern_id(p->syms, a->name),
                                               vn, cb, nvn);
                    }
                    if (e->is_num_effect) {
                        uint32_t narg[MAX_ARGS];
                        for (int k = 0; k < e->nargs; k++)
                            narg[k] = resolve_arg(cv, ncv, cb, e->args[k]);
                        uint32_t num = ground_pred(p, e->pred, narg, e->nargs);
                        expr_ins code[MAX_CODE];
                        int nc = 0;
                        p->code_of = false;
                        emit_expr(p, e->expr_root, cv, ncv, cb, code, &nc);
                        if (p->code_of) {
                            serr(p, e->line, e->col,
                                 "effect expression too long — its compiled "
                                 "form (including inlined value chains) "
                                 "exceeds %d VM instructions; simplify the "
                                 "expression or the value's layers", MAX_CODE);
                            p->code_of = false;
                        }
                        emit_num_effect(p, h2, e, num, narg[0], code, nc);
                    }
                }
            }
        }
    }
    a->srule_hi = world_step_rule_count(p->w);      /* coverage range end (#121) */
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
    if (rule_is_kind(p, ra) || rule_is_kind(p, rb))
        return;                        /* #125: applied inside the kind stratum
                                        * (mixed pairs already errored there) */
    if (ra->head.is_kinddef && rb->head.is_kinddef)
        return;                        /* #145: desugared to the dotted pairs */
    if (ra->head.is_kinddef || rb->head.is_kinddef) {
        serr(p, s->aline, s->acol,
             "'%s' > '%s': a kind modifier's expansions are the orderable "
             "rules — target the expanded label (`<modifier>.<value>`), or "
             "order two kind modifiers wholesale (`A > B`, #145)",
             s->a, s->b);
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

/* #82: make every ground value instance READABLE.
 *
 * A value's chain is inlined into the bytecode of whatever reads it, so a value
 * nothing in the world reads has nowhere to be read from — which is exactly the
 * case presentation cares about, since a bar's width is computed for a client
 * and for nobody else. Compiling each instance once more and registering it
 * under its ground atom costs one arena copy per instance and turns "the number
 * behind this bar" into the world's answer instead of arithmetic frozen inside
 * whatever widget draws it.
 *
 * The enumeration domain is the value's BASE definition's parameters, which
 * bind its head arguments by construction (#94: exactly one unconditional
 * base, each head argument a distinct rule parameter). */
static void register_value_reads(parser *p)
{
    for (int i = 0; i < p->npreds; i++) {
        pred_info *pi = &p->preds[i];
        if (!pi->is_value) continue;
        int vi = find_value(p, pi->pred);
        if (vi < 0 || p->nvdefs[vi] == 0) continue;
        int nargs = p->valuedecls[vi].nargs;

        /* Enumerate over the value's DECLARED argument sorts, not over any one
         * definition's parameters: a lookup table's rows each bind a constant,
         * so no single definition's parameter list covers the instances. */
        long total = 1;
        for (int k = 0; k < nargs; k++) {
            int sz = pi->argsort[k] >= 0 ? domain_size(p, pi->argsort[k]) : 0;
            if (sz <= 0) { total = 0; break; }
            if (total > MAX_INSTANCES / sz) { total = 0; break; }   /* no silent cap */
            total *= sz;
        }
        if (total <= 0) continue;

        uint32_t args[MAX_ARGS];
        for (long idx = 0; idx < total; idx++) {
            long rest = idx;
            for (int k = nargs - 1; k >= 0; k--) {
                int sz = domain_size(p, pi->argsort[k]);
                args[k] = domain_at(p, pi->argsort[k], (int)(rest % sz));
                rest /= sz;
            }
            expr_ins code[MAX_CODE];
            int pos = 0;
            emit_value_inline(p, pi->pred, args, code, &pos);
            if (pos <= 0 || pos >= MAX_CODE) continue;
            world_add_value(p->w, ground_pred(p, pi->pred, args, nargs), code, pos);
        }
    }
}

/* #159: ground the `exclusive` groups. Per group: the KEY is the named vars
 * in first-appearance order; per member, every ground action instance
 * registers under (group, key-odometer-index) — instances sharing a key
 * conflict at step time (world_step's pre-solve scan), distinct keys are
 * independent. The label ("east/west") and the declaration span render in
 * the rejection. */
static void ground_exclusives(parser *p)
{
    for (int e = 0; e < p->nexcls; e++) {
        ast_excl *x = &p->excls[e];
        char label[MAX_NAME * 2];
        int off = 0;
        for (int m = 0; m < x->nmem && off < (int)sizeof label; m++)
            off += snprintf(label + off, sizeof label - (size_t)off, "%s%s",
                            m ? "/" : "", x->mem[m].action);
        char pbuf[MAX_NAME + 24];
        int g = world_new_excl_group(p->w, label,
                                     prov_str(p, x->line, pbuf, sizeof pbuf));
        uint32_t kv[MAX_ARGS];
        int ks[MAX_ARGS], nk = 0;
        for (int m = 0; m < x->nmem; m++) {
            ast_action *a = find_action_named(p, x->mem[m].action);
            if (!a) continue;
            for (int k = 0; k < x->mem[m].nargs && k < a->nvars; k++) {
                uint32_t v = x->mem[m].vars[k];
                if (!v) continue;
                bool have = false;
                for (int i = 0; i < nk; i++) if (kv[i] == v) have = true;
                if (!have && nk < MAX_ARGS) {
                    kv[nk] = v;
                    ks[nk] = a->vars[k].sort;
                    nk++;
                }
            }
        }
        long keyspace = 1;
        bool of = false;
        for (int i = 0; i < nk && !of; i++) {
            int d = domain_size(p, ks[i]);
            if (d <= 0) of = true;
            else { keyspace *= d; if (keyspace > MAX_INSTANCES) of = true; }
        }
        if (of) {
            serr(p, x->line, x->col,
                 "`exclusive` group '%s' keys more than %d instances — "
                 "narrow the key (#159)", label, MAX_INSTANCES);
            continue;
        }
        for (int m = 0; m < x->nmem; m++) {
            ast_action *a = find_action_named(p, x->mem[m].action);
            if (!a || a->is_ramif) continue;
            bool aof = false;
            long total = instance_count(p, a->vars, a->nvars, &aof);
            if (aof) continue;             /* the action's own grounding
                                            * already reported the blow-up */
            uint32_t binding[MAX_ARGS];
            uint32_t nameatom = intern_id(p->syms, a->name);
            for (long idx = 0; idx < total; idx++) {
                decode_binding(p, a->vars, a->nvars, idx, binding);
                uint32_t key = 0;
                for (int i = 0; i < nk; i++) {
                    int jpos = -1;
                    for (int k = 0; k < x->mem[m].nargs; k++)
                        if (x->mem[m].vars[k] == kv[i]) { jpos = k; break; }
                    int d = domain_size(p, ks[i]);
                    int pos = jpos >= 0 ? entity_pos(p, ks[i], binding[jpos])
                                        : 0;
                    if (pos < 0) pos = 0;
                    key = key * (uint32_t)(d > 0 ? d : 1) + (uint32_t)pos;
                }
                world_add_excl_member(p->w, g,
                                      ground_pred(p, nameatom, binding,
                                                  a->nvars), key);
            }
        }
    }
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

/* ---- #98 conflictable pairs (EPIC #154) -------------------------------
 *
 * The static answer to two silent/loud failure classes:
 *
 *  1. STEP side (the contested `-1`s): two step-emitting constructs whose
 *     effects can land on the SAME ground atom with conflicting content —
 *     complementary boolean effects, multi-valued effects with different
 *     values, or merge-less `:=` assigns — and whose conditions do not
 *     exclude co-firing. A step where both fire is the runtime contested
 *     error; the warning moves it to compile time. Acceptance (the epic's
 *     totality contract): a zero-warning story cannot take those paths.
 *  2. JUDGMENT side (the silently-REFUTED null, §6.2): two concluding rules
 *     with complementary heads and nothing deciding the conflict — no `>`
 *     (hand-written or band-desugared, both live in p->sups after
 *     desugar_bands), no team member ordered over the opponent, no exclusive
 *     bodies. Under team defeat both sides read REFUTED whenever both fire.
 *     Two STRICT rules are their own case: superiority never orders the
 *     strict layer, so a co-firing pair is a definite contradiction.
 *
 * Everything is RULE-TEMPLATE level (ground pairs explode at scale): two
 * atoms may collide when their argument lists unify — variables are
 * wildcards, entities constants — and a collision's substitution is a set of
 * equivalence classes over (side, name) terms. Conditions exclude co-firing
 * when some pair of them is complementary UNDER that substitution: opposite
 * polarity on one atom, different multi-valued constants, disjoint numeric
 * comparison intervals, or disjoint #95 membership lists. Conservative both
 * ways a warning needs: a pair that cannot collide never warns; a pair whose
 * exclusivity we cannot see does (one self-documenting condition is the
 * fix — the same Elm trade as #116's safety rule).
 *
 * Warning severity, never an error: resolved pairs stay quiet (a resolved
 * pair cannot contest, and the zero-warning contract must be reachable);
 * the "how does every pair resolve" report is LSP-surface work on #98.
 * Defeaters are excluded — a blocked head reads UNDECIDED by intent, and a
 * defeater never concludes, so it cannot contest a step. */

#define MAX_UNIF (4 * MAX_ARGS)

typedef struct { int side; uint32_t name; } cp_term;   /* side 2 = constant */

typedef struct {
    cp_term t[MAX_UNIF];
    int     cls[MAX_UNIF];                 /* class id per term */
    int     n;
} cp_subst;

/* A name is a variable of `vars` or a constant (entity / enum value / int). */
static bool cp_is_var(var_bind *vars, int nvars, uint32_t name)
{
    return var_index(vars, nvars, name) >= 0;
}

static int cp_find(cp_subst *u, int side, uint32_t name)
{
    for (int i = 0; i < u->n; i++)
        if (u->t[i].side == side && u->t[i].name == name) return i;
    return -1;
}

static int cp_intern(cp_subst *u, int side, uint32_t name)
{
    int i = cp_find(u, side, name);
    if (i >= 0) return i;
    if (u->n >= MAX_UNIF) return -1;       /* saturated: give up conservatively */
    u->t[u->n].side = side;
    u->t[u->n].name = name;
    u->cls[u->n] = u->n;
    return u->n++;
}

static int cp_root(cp_subst *u, int i)
{
    while (u->cls[i] != i) i = u->cls[i];
    return i;
}

/* Union the classes of terms a and b. Two DISTINCT constants in one class
 * mean the unification is impossible: returns false. */
static bool cp_union(cp_subst *u, int a, int b)
{
    if (a < 0 || b < 0) return true;       /* saturated: assume unifiable */
    int ra = cp_root(u, a), rb = cp_root(u, b);
    if (ra == rb) return true;
    if (u->t[ra].side == 2 && u->t[rb].side == 2)
        return u->t[ra].name == u->t[rb].name;
    if (u->t[ra].side == 2) { int t = ra; ra = rb; rb = t; }   /* const as root */
    u->cls[ra] = rb;
    return true;
}

static int cp_term_of(cp_subst *u, var_bind *vars, int nvars, int side,
                      uint32_t name)
{
    return cp_intern(u, cp_is_var(vars, nvars, name) ? side : 2, name);
}

/* Are two argument terms provably EQUAL under the substitution? (Same class,
 * or the same constant.) Interns on demand; unknown terms are never equal. */
static bool cp_args_equal(cp_subst *u, var_bind *va, int na, uint32_t a,
                          var_bind *vb, int nb, uint32_t b)
{
    int ia = cp_term_of(u, va, na, 0, a);
    int ib = cp_term_of(u, vb, nb, 1, b);
    if (ia < 0 || ib < 0) return false;
    int ra = cp_root(u, ia), rb = cp_root(u, ib);
    if (ra == rb) return true;
    return u->t[ra].side == 2 && u->t[rb].side == 2 &&
           u->t[ra].name == u->t[rb].name;
}

/* Unify two atoms' argument lists into `u`. False = they can never name the
 * same ground atom (distinct constants at some position). */
static bool cp_unify_args(cp_subst *u, const ast_atom *a, var_bind *va, int na,
                          const ast_atom *b, var_bind *vb, int nb)
{
    if (a->nargs != b->nargs) return false;
    for (int k = 0; k < a->nargs; k++)
        if (!cp_union(u, cp_term_of(u, va, na, 0, a->args[k].name),
                         cp_term_of(u, vb, nb, 1, b->args[k].name)))
            return false;
    return true;
}

/* Comparison guard as a closed interval [lo, hi]. */
static void cp_guard_range(const ast_atom *g, long *lo, long *hi)
{
    switch (g->cmp) {
    case WORLD_CMP_LE: *lo = LONG_MIN;         *hi = g->threshold;     break;
    case WORLD_CMP_LT: *lo = LONG_MIN;         *hi = g->threshold - 1; break;
    case WORLD_CMP_GE: *lo = g->threshold;     *hi = LONG_MAX;         break;
    case WORLD_CMP_GT: *lo = g->threshold + 1; *hi = LONG_MAX;         break;
    default:           *lo = g->threshold;     *hi = g->threshold;     break;
    }
}

/* Are two condition atoms complementary under the substitution — can they
 * never hold together? The recognized shapes: opposite polarity on one
 * (boolean, same primedness) atom; different multi-valued constants on one
 * fluent; disjoint comparison intervals on one numeric; disjoint #95
 * membership lists on one variable. */
static bool cp_conds_exclusive(parser *p, cp_subst *u,
                               const ast_atom *ca, var_bind *va, int na,
                               const ast_atom *cb, var_bind *vb, int nb)
{
    if (ca->is_expr_guard || cb->is_expr_guard) return false;
    if (ca->is_member && cb->is_member) {      /* disjoint membership lists */
        if (ca->neg || cb->neg) return false;
        if (!cp_args_equal(u, va, na, ca->args[0].name,
                              vb, nb, cb->args[0].name))
            return false;
        for (int i = 0; i < ca->mem_n; i++)
            for (int j = 0; j < cb->mem_n; j++)
                if (p->mempool[ca->mem_ix + i] == p->mempool[cb->mem_ix + j])
                    return false;
        return true;
    }
    if (ca->is_member || cb->is_member) return false;
    if (ca->pred != cb->pred || ca->nargs != cb->nargs) return false;
    if (ca->primed != cb->primed) return false;
    for (int k = 0; k < ca->nargs; k++)
        if (!cp_args_equal(u, va, na, ca->args[k].name,
                              vb, nb, cb->args[k].name))
            return false;
    if (ca->is_guard && cb->is_guard) {        /* disjoint numeric intervals */
        long la, ha, lb, hb;
        cp_guard_range(ca, &la, &ha);
        cp_guard_range(cb, &lb, &hb);
        return ha < lb || hb < la;
    }
    if (ca->is_guard || cb->is_guard) return false;
    if (ca->value != INTERN_NONE && cb->value != INTERN_NONE) {
        /* different multi-valued CONSTANTS exclude; join values (variables)
         * exclude nothing we can see */
        if (ca->neg || cb->neg) return false;
        if (cp_is_var(va, na, ca->value) || cp_is_var(vb, nb, cb->value))
            return false;
        return ca->value != cb->value;
    }
    if (ca->value != INTERN_NONE || cb->value != INTERN_NONE) return false;
    return ca->neg != cb->neg;                 /* p vs ~p */
}

/* Does any pair drawn from the two condition groups exclude co-firing? */
static bool cp_groups_exclusive(parser *p, cp_subst *u,
                                ast_atom *const *ga, const int *na, int ng_a,
                                var_bind *va, int nva,
                                ast_atom *const *gb, const int *nb, int ng_b,
                                var_bind *vb, int nvb)
{
    for (int i = 0; i < ng_a; i++)
        for (int x = 0; x < na[i]; x++)
            for (int j = 0; j < ng_b; j++)
                for (int y = 0; y < nb[j]; y++) {
                    cp_subst save = *u;    /* interning must not leak between
                                            * candidate pairs' arg probes */
                    if (cp_conds_exclusive(p, &save, &ga[i][x], va, nva,
                                           &gb[j][y], vb, nvb))
                        return true;
                }
    return false;
}

/* One step-side writer: an effect atom with its owning construct's variable
 * scope and condition groups. */
typedef struct {
    ast_atom   *eff;
    const char *owner;                     /* action / ramification name */
    const ast_action *act;                 /* the owning construct (#159) */
    bool        is_ramif;
    var_bind    vars[3 * MAX_ARGS];
    int         nvars;
    ast_atom   *conds[3];
    int         nconds[3];
    int         ng;
    int         line, col;
} cp_writer;

/* Does expression `e` vary across bindings that agree on the effect's own
 * arguments — i.e. does it read a scope variable OUTSIDE `covered`, or draw a
 * roll (§5.10 keys sites by the FULL binding)? Such an expression can hand
 * two colliding instances different values. */
static bool cp_expr_varies(parser *p, int e, var_bind *vars, int nvars,
                           const bool *covered, int depth)
{
    if (e < 0 || depth > 2 * MAX_ARGS) return false;
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_ENT: return false;
    case EX_ROLL:  return true;
    case EX_LOAD: case EX_TEST: {
        for (int k = 0; k < n->nargs; k++) {
            int vi = var_index(vars, nvars, n->args[k].name);
            if (vi >= 0 && !covered[vi]) return true;
        }
        return false;
    }
    case EX_CALL:
        for (int k = 0; k < n->nargs; k++)
            if (cp_expr_varies(p, n->cargs[k], vars, nvars, covered, depth + 1))
                return true;
        return false;
    case EX_NEG: case EX_PRIOR:
        return cp_expr_varies(p, n->lhs, vars, nvars, covered, depth + 1);
    default:
        return cp_expr_varies(p, n->lhs, vars, nvars, covered, depth + 1) ||
               cp_expr_varies(p, n->rhs, vars, nvars, covered, depth + 1);
    }
}

static int cp_fluent_merge(parser *p, uint32_t pred)
{
    for (int i = 0; i < p->nfluents; i++)
        if (p->fluents[i].pred == pred) return p->fluents[i].merge_mode;
    return 0;
}

/* #159: is the (a, b) collision forbidden by a declared `exclusive` group?
 * Both owners must be members of one group, and the collision's unifier must
 * FORCE every key variable equal (the two members' key positions land in one
 * σ-class through the effect args) — then any pair of instances that could
 * actually collide shares the key, and the group rejects their co-submission
 * at step time. A collision that leaves a key free (an arity-0 fluent, say)
 * is NOT covered: instances with different keys still contest. */
static bool cp_excl_covers_pair(parser *p, cp_subst *u, const cp_writer *a,
                                const cp_writer *b)
{
    if (a->is_ramif || b->is_ramif || !a->act || !b->act || a->act == b->act)
        return false;
    for (int e = 0; e < p->nexcls; e++) {
        ast_excl *x = &p->excls[e];
        int ma = -1, mb = -1;
        for (int m = 0; m < x->nmem; m++) {
            if (strcmp(x->mem[m].action, a->act->name) == 0) ma = m;
            if (strcmp(x->mem[m].action, b->act->name) == 0) mb = m;
        }
        if (ma < 0 || mb < 0 || ma == mb) continue;
        if (x->mem[ma].nargs != a->act->nvars ||
            x->mem[mb].nargs != b->act->nvars)
            continue;                              /* malformed: errored */
        bool forced = true;
        for (int k = 0; k < x->mem[ma].nargs && forced; k++) {
            uint32_t v = x->mem[ma].vars[k];
            if (!v) continue;
            int jb = -1;
            for (int j = 0; j < x->mem[mb].nargs; j++)
                if (x->mem[mb].vars[j] == v) { jb = j; break; }
            if (jb < 0) { forced = false; break; }
            cp_subst save = *u;
            if (!cp_args_equal(&save, (var_bind *)a->vars, a->nvars,
                               a->act->vars[k].name,
                               (var_bind *)b->vars, b->nvars,
                               b->act->vars[jb].name))
                forced = false;
        }
        if (forced) return true;
    }
    return false;
}

/* #159, the SELF-collision analog: colliding instances agree on the effect's
 * target args, so if every key variable's position is among them the two
 * instances share the key and the group forbids the pair. An uncovered
 * BINDER variable disqualifies outright — it collides within ONE submitted
 * action atom, which no protocol group can forbid. */
static bool cp_excl_covers_self(parser *p, const cp_writer *w2,
                                const bool *covered)
{
    if (w2->is_ramif || !w2->act) return false;
    for (int i = w2->act->nvars; i < w2->nvars; i++)
        if (!covered[i]) return false;
    for (int e = 0; e < p->nexcls; e++) {
        ast_excl *x = &p->excls[e];
        for (int m = 0; m < x->nmem; m++) {
            if (strcmp(x->mem[m].action, w2->act->name) != 0) continue;
            if (x->mem[m].nargs != w2->act->nvars) continue;
            bool ok = true;
            for (int k = 0; k < x->mem[m].nargs && ok; k++) {
                uint32_t v = x->mem[m].vars[k];
                if (!v) continue;
                int vi = var_index((var_bind *)w2->vars, w2->nvars,
                                   w2->act->vars[k].name);
                if (vi < 0 || !covered[vi]) ok = false;
            }
            if (ok) return true;
        }
    }
    return false;
}

/* #160: step-side conflictable pairs are ERRORS — a contested step has no
 * meaning to ship (the `-1` is per-step and has no principled recovery), and
 * with #159 every safe construction has a checkable spelling. The judgment
 * side stays a warning (contested judgments are defined, sometimes intended
 * semantics; their hazard — the silent null — is answered by visibility). */
static void cp_err_step(parser *p, const cp_writer *a, const cp_writer *b,
                        bool assign)
{
    const char *an = intern_name(p->syms, a->eff->pred);
    bool acts = !a->is_ramif && !b->is_ramif;
    if (assign)
        serr(p, b->line, b->col,
             "%s '%s' and %s '%s' can fire in the same step and both assign "
             "(`:=`) '%s' — conflicting assigns are a contested step (§5.8); "
             "declare `merge min|max` on '%s', make their conditions "
             "exclusive%s (#160)",
             a->is_ramif ? "ramification" : "action", a->owner,
             b->is_ramif ? "ramification" : "action", b->owner, an, an,
             acts ? ", or declare the actions `exclusive` (#159)" : "");
    else
        serr(p, b->line, b->col,
             "%s '%s' and %s '%s' can fire in the same step with conflicting "
             "effects on '%s' — a step where both apply is a contested step, "
             "not a defeat (§5.8); make their conditions exclusive%s (#160)",
             a->is_ramif ? "ramification" : "action", a->owner,
             b->is_ramif ? "ramification" : "action", b->owner, an,
             acts ? ", or declare the actions `exclusive` (#159)" : "");
}

static void cp_check_writer_pair(parser *p, const cp_writer *a,
                                 const cp_writer *b)
{
    ast_atom *ea = a->eff, *eb = b->eff;
    if (ea->pred != eb->pred) return;
    bool na = ea->is_num_effect, nb2 = eb->is_num_effect;
    if (na != nb2) return;
    bool conflict, assign = false;
    if (na) {                                  /* numeric / cell `:=` pair */
        if (ea->numop != WORLD_OP_ASSIGN || eb->numop != WORLD_OP_ASSIGN)
            return;                            /* deltas sum, never contest */
        if (cp_fluent_merge(p, ea->pred)) return;   /* #85 merge absorbs */
        long ka, kb;
        if (expr_fold(p, ea->expr_root, &ka) && expr_fold(p, eb->expr_root, &kb)
            && ka == kb)
            return;                            /* identical constants agree */
        conflict = true;
        assign = true;
    } else if (ea->value != INTERN_NONE && eb->value != INTERN_NONE) {
        conflict = true;                       /* provably-equal values checked
                                                * below, under the unifier */
    } else if (ea->value == INTERN_NONE && eb->value == INTERN_NONE) {
        conflict = ea->neg != eb->neg;         /* p vs ~p */
    } else {
        return;
    }
    if (!conflict) return;

    cp_subst u = { .n = 0 };
    if (!cp_unify_args(&u, ea, (var_bind *)a->vars, a->nvars,
                           eb, (var_bind *)b->vars, b->nvars))
        return;                                /* can never collide */
    if (!na && ea->value != INTERN_NONE) {
        /* MV pair: provably the same value = the identical effect */
        bool va = cp_is_var((var_bind *)a->vars, a->nvars, ea->value);
        bool vb = cp_is_var((var_bind *)b->vars, b->nvars, eb->value);
        if (!va && !vb && ea->value == eb->value) return;
        if ((va || vb) &&
            cp_args_equal(&u, (var_bind *)a->vars, a->nvars, ea->value,
                              (var_bind *)b->vars, b->nvars, eb->value))
            return;
    }
    if (cp_groups_exclusive(p, &u, a->conds, a->nconds, a->ng,
                            (var_bind *)a->vars, a->nvars,
                            b->conds, b->nconds, b->ng,
                            (var_bind *)b->vars, b->nvars))
        return;
    if (cp_excl_covers_pair(p, &u, a, b))
        return;                            /* #159: a declared group rejects
                                            * the co-submission at step time */
    cp_err_step(p, a, b, assign);
}

/* SELF collision: one writer template, two bindings landing on one ground
 * atom — possible exactly when a scope variable is absent from the effect's
 * argument list (a `for each` var: within one firing; an action var: a host
 * may pass several instances of the action in one step). Contests only when
 * the assigned content can differ between the two bindings. */
static void cp_check_writer_self(parser *p, const cp_writer *w)
{
    ast_atom *e = w->eff;
    bool covered[3 * MAX_ARGS] = { false };
    bool missing = false;
    uint32_t missing_var = 0;
    for (int i = 0; i < w->nvars; i++) {
        for (int k = 0; k < e->nargs; k++)
            if (e->args[k].name == w->vars[i].name) covered[i] = true;
        if (!covered[i] && !missing) { missing = true; missing_var = w->vars[i].name; }
    }
    if (!missing) return;
    if (cp_excl_covers_self(p, w, covered))
        return;                            /* #159: colliding instances share
                                            * a declared group key */
    const char *an = intern_name(p->syms, e->pred);
    if (e->is_num_effect) {
        if (e->numop != WORLD_OP_ASSIGN || cp_fluent_merge(p, e->pred)) return;
        if (!cp_expr_varies(p, e->expr_root, (var_bind *)w->vars, w->nvars,
                            covered, 0))
            return;                            /* same value from every binding */
        serr(p, w->line, w->col,
             "%s '%s' can fire more than once in one step (bindings differing "
             "on '%s') and assign (`:=`) '%s' a value that varies with the "
             "binding — a contested step (§5.8); include '%s' in the "
             "target's arguments, make the value independent of it, or "
             "declare the action self-`exclusive` (#159/#160)",
             w->is_ramif ? "ramification" : "action", w->owner,
             intern_name(p->syms, missing_var), an,
             intern_name(p->syms, missing_var));
    } else if (e->value != INTERN_NONE) {
        int vi = var_index((var_bind *)w->vars, w->nvars, e->value);
        if (vi < 0 || covered[vi]) return;     /* constant, or binding-tied */
        serr(p, w->line, w->col,
             "%s '%s' can fire more than once in one step (bindings differing "
             "on '%s') and set '%s' to a value that varies with the binding — "
             "a contested step (§5.8); include '%s' in the target's "
             "arguments, or declare the action self-`exclusive` (#159/#160)",
             w->is_ramif ? "ramification" : "action", w->owner,
             intern_name(p->syms, e->value), an,
             intern_name(p->syms, e->value));
    }
    /* boolean: one template concludes one polarity — never self-conflicts */
}

static void cp_collect_writer(parser *p, ast_action *a, ast_binder *bnd,
                              binder_item *item, ast_atom *eff, cp_writer *w)
{
    (void)p;
    w->eff = eff;
    w->owner = a->name;
    w->act = a;
    w->is_ramif = a->is_ramif;
    w->nvars = 0;
    for (int k = 0; k < a->nvars; k++) w->vars[w->nvars++] = a->vars[k];
    if (bnd)
        for (int k = 0; k < bnd->nvars && w->nvars < 3 * MAX_ARGS; k++)
            w->vars[w->nvars++] = bnd->vars[k];
    w->ng = 0;
    w->conds[w->ng] = a->requires; w->nconds[w->ng++] = a->nreq;
    if (bnd)  { w->conds[w->ng] = bnd->where;  w->nconds[w->ng++] = bnd->nwhere; }
    if (item) { w->conds[w->ng] = item->when;  w->nconds[w->ng++] = item->nwhen; }
    w->line = eff->line;
    w->col = eff->col;
}

/* Judgment side: complementary concluding rules (strict/defeasible; kind and
 * value machinery excluded — the kind stratum solves and errors at build). */
static bool cp_rule_concludes(parser *p, ast_rule *r)
{
    if (r->kind == DL_DEFEATER) return false;
    if (r->head.is_valuedef || r->head.is_kinddef) return false;
    if (rule_is_kind(p, r)) return false;
    if (r->head.value != INTERN_NONE || r->head.is_guard ||
        r->head.is_expr_guard)
        return false;
    return true;
}

/* Is some rule concluding the same literal as `side` ordered (via p->sups —
 * hand-written or band-desugared) above `opp`? Team defeat's static shadow. */
static bool cp_team_covers(parser *p, ast_rule *side, ast_rule *opp)
{
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *t = &p->rules[i];
        if (!cp_rule_concludes(p, t) || t->kind == DL_STRICT) continue;
        if (t->head.pred != side->head.pred || t->head.neg != side->head.neg)
            continue;
        for (int e = 0; e < p->nsups; e++)
            if (strcmp(p->sups[e].a, t->label) == 0 &&
                strcmp(p->sups[e].b, opp->label) == 0)
                return true;
    }
    return false;
}

static void cp_check_rule_pair(parser *p, ast_rule *ra, ast_rule *rb)
{
    if (ra->head.pred != rb->head.pred || ra->head.neg == rb->head.neg)
        return;
    cp_subst u = { .n = 0 };
    if (!cp_unify_args(&u, &ra->head, ra->vars, ra->nvars,
                           &rb->head, rb->vars, rb->nvars))
        return;                                /* heads never collide */
    /* only the BODIES exclude co-firing; `unless` guards defeat, they do
     * not gate applicability */
    ast_atom *ba[1] = { ra->body };
    int      nba[1] = { ra->nbody };
    ast_atom *bb[1] = { rb->body };
    int      nbb[1] = { rb->nbody };
    if (cp_groups_exclusive(p, &u, ba, nba, 1, ra->vars, ra->nvars,
                            bb, nbb, 1, rb->vars, rb->nvars))
        return;
    const char *pn = intern_name(p->syms, ra->head.pred);
    if (ra->kind == DL_STRICT && rb->kind == DL_STRICT) {
        warn(p, rb->line, rb->col,
             "strict rules '%s' and '%s' conclude complementary '%s' — "
             "superiority never orders the strict layer, so a state where "
             "both fire proves a definite contradiction; make their bodies "
             "exclusive, or weaken one to `=>` (#98)",
             ra->label, rb->label, pn);
        return;
    }
    if (ra->kind == DL_STRICT || rb->kind == DL_STRICT)
        return;                                /* strict wins: decided */
    if (cp_team_covers(p, ra, rb) || cp_team_covers(p, rb, ra))
        return;                                /* ordered (directly or by a
                                                * teammate / band edge) */
    warn(p, rb->line, rb->col,
         "rules '%s' and '%s' conclude complementary '%s' and nothing orders "
         "them — whenever both apply, team defeat reads BOTH sides REFUTED "
         "(§13); add '%s > %s' (or the reverse), a band edge, or exclusive "
         "bodies (#98)",
         ra->label, rb->label, pn, ra->label, rb->label);
}

static void check_conflictable_pairs(parser *p)
{
    /* step side: collect every effect writer, then all pairs + selfs */
    int nw = 0;
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        nw += a->neff;
        for (int bi = 0; bi < a->nbind; bi++)
            nw += p->binders[a->bind_ix[bi]].nitems;
    }
    if (nw > 0) {
        cp_writer *ws = calloc((size_t)nw, sizeof *ws);
        int n = 0;
        for (int i = 0; i < p->nactions; i++) {
            ast_action *a = &p->actions[i];
            for (int b = 0; b < a->neff; b++)
                cp_collect_writer(p, a, NULL, NULL, &a->effects[b], &ws[n++]);
            for (int bi = 0; bi < a->nbind; bi++) {
                ast_binder *bnd = &p->binders[a->bind_ix[bi]];
                for (int it = 0; it < bnd->nitems; it++)
                    cp_collect_writer(p, a, bnd, &bnd->items[it],
                                      &bnd->items[it].eff, &ws[n++]);
            }
        }
        for (int i = 0; i < n; i++) {
            cp_check_writer_self(p, &ws[i]);
            for (int j = i + 1; j < n; j++)
                cp_check_writer_pair(p, &ws[i], &ws[j]);
        }
        free(ws);
    }
    /* judgment side */
    for (int i = 0; i < p->nrules; i++) {
        if (!cp_rule_concludes(p, &p->rules[i])) continue;
        for (int j = i + 1; j < p->nrules; j++) {
            if (!cp_rule_concludes(p, &p->rules[j])) continue;
            cp_check_rule_pair(p, &p->rules[i], &p->rules[j]);
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

/* What sort is this atom over? A DECLARED predicate carries its schema in
 * `argsort`; a judgment has no declaration, so its argument sorts are the
 * signature inferred from the rules that conclude it (#205, `headsort`) —
 * reading `argsort` there gets whatever memset left, which is sort index 0 and
 * a real sort, so eligibility would turn on declaration order (#220). Every
 * lane gate asks the question here: when sort union lands, this equality
 * becomes admission ("does the lane sort satisfy the atom's"), in one place. */
static int pred_arg_sort(const pred_info *pi, int k)
{
    bool declared = pi->is_fluent || pi->is_provider || pi->is_emit ||
                    pi->is_value || pi->is_kindpred;
    return declared ? pi->argsort[k] : pi->headsort[k];
}

/* An argument that names an entity rather than binding a variable (#226). A
 * constant is a FIXED index into the ground map, known at compile time, in a
 * builder that is already substituting a different entity per role — it carries
 * no soundness content, so it disqualifies nothing. What it does do is make one
 * predicate reachable at several patterns (`holding(X, key1)` beside
 * `holding(X, key2)`), which is why a family-local is keyed by its whole
 * pattern (#235) and not by its predicate. */
enum { ROLE_CONST = -1 };

/* `roles[k]`/`consts[k]` describe argument k: a variable role (0 = the lane
 * axis), or ROLE_CONST plus the entity the source named. */
static bool lane_atom_ok(parser *p, const ast_atom *a, int S, uint32_t var,
                         bool is_head, int *roles, uint32_t *consts)
{
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect || a->is_expr_guard)
        return false;                              /* MV / numeric / expr guard: out */
    if (a->is_defined)
        return false;                              /* `defined v(…)` (#116): its
                                                    * marker-disjunction rules
                                                    * live in the N=1 family */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num)
        return false;
    if (pi->is_provider && is_head)
        return false;                              /* read-only: never concluded */
    if (pi->arity == 0)
        return !is_head;                           /* globals: broadcast body only */
    if (pi->arity != a->nargs)
        return false;
    bool binds = false;
    for (int k = 0; k < a->nargs; k++) {
        if (a->args[k].name == var) {
            if (pred_arg_sort(pi, k) != S)
                return false;
            roles[k] = 0;
            consts[k] = INTERN_NONE;
            binds = true;
        } else if (entity_pos(p, pred_arg_sort(pi, k), a->args[k].name) >= 0) {
            roles[k] = ROLE_CONST;                 /* a named entity of that sort */
            consts[k] = a->args[k].name;
        } else {
            return false;                          /* another variable, or not an
                                                    * entity of the declared sort */
        }
    }
    /* A head has to vary along the lane axis: one that names only constants
     * concludes the same atom from every lane, which is the arity-0 case and is
     * refused for the same reason. */
    return binds || !is_head;
}

/* A rule can lane iff it is single-variable and every atom — body, head, and any
 * `unless` guard — is over that one sort with each argument either the variable
 * or a named entity, or is an arity-0 global input. An `unless` guard lowers to
 * a defeater `guard ~> ~head` (§6), emitted as its own schema rule, so its atoms
 * must lane too. */
static bool rule_eligible(parser *p, ast_rule *r)
{
    if (r->nvars != 1)
        return false;
    int S = r->vars[0].sort;
    uint32_t var = r->vars[0].name;
    int roles[MAX_ARGS];
    uint32_t consts[MAX_ARGS];
    if (!lane_atom_ok(p, &r->head, S, var, true, roles, consts))
        return false;
    for (int b = 0; b < r->nbody; b++)
        if (!lane_atom_ok(p, &r->body[b], S, var, false, roles, consts))
            return false;
    for (int g = 0; g < r->nguard; g++)
        if (!lane_atom_ok(p, &r->guard[g], S, var, false, roles, consts))
            return false;
    return true;
}

static int pred_idx(parser *p, uint32_t pred)
{
    pred_info *pi = find_pred(p, pred);
    return pi ? (int)(pi - p->preds) : -1;
}

/* The family-local table, shared by both lane emitters. A local is a predicate
 * AT AN ARGUMENT PATTERN, never a predicate: `near(X, Y)` and `near(Y, X)` read
 * two different columns of one relation, and so do `holding(X, key1)` and
 * `holding(X, key2)`. Keying by predicate alone made the second read the first's
 * ground map (#235). `vname` is the display spelling of each slot — a variable's
 * name, or the entity a constant named — kept so a trace can say WHICH column. */
typedef struct {
    uint32_t pred[MAX_PREDS];
    uint8_t  kind[MAX_PREDS];
    int      nargs[MAX_PREDS];
    int      role[MAX_PREDS][MAX_ARGS];
    uint32_t konst[MAX_PREDS][MAX_ARGS];
    uint32_t vname[MAX_PREDS][MAX_ARGS];
    int      n;
} lane_locals;

/* Find or add the local for one atom's pattern; -1 if the table is full. */
static int lane_local_of(parser *p, lane_locals *L, const ast_rule *r,
                         uint32_t pred, int nargs, const int *roles,
                         const uint32_t *consts)
{
    for (int j = 0; j < L->n; j++) {
        if (L->pred[j] != pred || L->nargs[j] != nargs) continue;
        bool same = true;
        for (int m = 0; m < nargs && same; m++)
            same = L->role[j][m] == roles[m] &&
                   (roles[m] != ROLE_CONST || L->konst[j][m] == consts[m]);
        if (same) return j;
    }
    if (L->n >= MAX_PREDS) return -1;
    int j = L->n++;
    pred_info *pi = find_pred(p, pred);
    L->pred[j] = pred;
    L->kind[j] = pi->is_fluent   ? WORLD_LANE_FLUENT
               : pi->is_provider ? WORLD_LANE_PROVIDER
                                 : WORLD_LANE_DERIVED;
    L->nargs[j] = nargs;
    for (int m = 0; m < nargs; m++) {
        L->role[j][m] = roles[m];
        L->konst[j][m] = roles[m] == ROLE_CONST ? consts[m] : INTERN_NONE;
        L->vname[j][m] = roles[m] == ROLE_CONST ? consts[m]
                                                : r->vars[roles[m]].name;
    }
    return j;
}

/* Local `j`'s ground atom for one cell: role 0 takes the lane entity, role v>0
 * the iterated entity for var v (`vidx`, NULL for a single-variable family), and
 * ROLE_CONST the entity the source named. */
static uint32_t lane_cell_atom(parser *p, const lane_locals *L, int j,
                               const ast_rule *r, int Sl, int e, const int *vidx)
{
    if (L->nargs[j] == 0)
        return L->pred[j];                         /* arity-0 global: broadcast */
    uint32_t args[MAX_ARGS];
    for (int m = 0; m < L->nargs[j]; m++) {
        int rho = L->role[j][m];
        args[m] = rho == ROLE_CONST ? L->konst[j][m]
                : rho == 0          ? domain_at(p, Sl, e)
                                    : domain_at(p, r->vars[rho].sort, vidx[rho]);
    }
    return ground_pred(p, L->pred[j], args, L->nargs[j]);
}

/* A predicate at ONE pattern keeps its bare name, so every trace written before
 * locals split by pattern reads exactly as it did. A predicate at two has to say
 * which — `near(X,Y)` against `near(Y,X)` — or the trace shows one atom
 * concluding and failing at once. */
static void lane_local_name(parser *p, const lane_locals *L, int j,
                            char *buf, size_t cap)
{
    int same = 0;
    for (int b = 0; b < L->n; b++)
        if (L->pred[b] == L->pred[j]) same++;
    if (same == 1 || L->nargs[j] == 0) {
        snprintf(buf, cap, "%s", intern_name(p->syms, L->pred[j]));
        return;
    }
    int o = snprintf(buf, cap, "%s(", intern_name(p->syms, L->pred[j]));
    for (int m = 0; m < L->nargs[j] && o > 0 && o < (int)cap; m++)
        o += snprintf(buf + o, cap - (size_t)o, "%s%s", m ? "," : "",
                      intern_name(p->syms, L->vname[j][m]));
    if (o > 0 && o < (int)cap)
        snprintf(buf + o, cap - (size_t)o, ")");
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
            if (r->head.is_valuedef || r->head.is_kinddef) continue;   /* #82 */
            if (rule_is_kind(p, r)) continue;                          /* #125 */
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
        if (r->head.is_valuedef || r->head.is_kinddef) continue;       /* #82 */
        if (rule_is_kind(p, r)) continue;                              /* #125 */
        int hp = pred_idx(p, r->head.pred);
        if (hp < 0 || taint[hp] || r->nvars != 1 || r->vars[0].sort != S)
            continue;
        laned[nlaned++] = i;
    }
    if (nlaned == 0)
        return;

    /* Family-local atoms, one per (predicate, argument pattern). Two rules using
     * one predicate with different constants — `holding(X, key1)` beside
     * `holding(X, key2)` — read two columns, and must not share a local. */
    lane_locals L;
    L.n = 0;
    int local_head[MAX_RULES], local_body[MAX_RULES][MAX_BODY];
    int local_guard[MAX_RULES][MAX_BODY];
    for (int li = 0; li < nlaned; li++) {
        ast_rule *r = &p->rules[laned[li]];
        int roles[MAX_ARGS];
        uint32_t consts[MAX_ARGS];
        const ast_atom *atoms[1 + 2 * MAX_BODY];
        int slot[1 + 2 * MAX_BODY], na = 0;
        atoms[na++] = &r->head;
        for (int b = 0; b < r->nbody; b++)  atoms[na++] = &r->body[b];
        for (int g = 0; g < r->nguard; g++) atoms[na++] = &r->guard[g];
        for (int k = 0; k < na; k++) {
            /* re-derive the pattern the eligibility gate already accepted */
            if (!lane_atom_ok(p, atoms[k], S, r->vars[0].name, k == 0,
                              roles, consts))
                return;
            int j = lane_local_of(p, &L, r, atoms[k]->pred, atoms[k]->nargs,
                                  roles, consts);
            if (j < 0)
                return;                            /* local table full: no family */
            slot[k] = j;
        }
        local_head[li] = slot[0];
        for (int b = 0; b < r->nbody; b++)  local_body[li][b] = slot[1 + b];
        for (int g = 0; g < r->nguard; g++) local_guard[li][g] = slot[1 + r->nbody + g];
    }
    int npred = L.n;

    dlcol *f = dlcol_new(npred, nent);
    for (int a = 0; a < npred; a++) {
        char nbuf[MAX_NAME * 2 + 8];
        lane_local_name(p, &L, a, nbuf, sizeof nbuf);
        dlcol_set_atom_name(f, (uint32_t)a, nbuf);
    }

    int schema_id[MAX_RULES];                      /* rule index -> schema id */
    for (int i = 0; i < p->nrules; i++) schema_id[i] = -1;
    for (int li = 0; li < nlaned; li++) {
        ast_rule *r = &p->rules[laned[li]];
        int hl = local_head[li];
        dl_lit head = { (uint32_t)hl, r->head.neg };
        dl_lit body[MAX_BODY];
        for (int b = 0; b < r->nbody; b++)
            body[b] = (dl_lit){ (uint32_t)local_body[li][b], r->body[b].neg };
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
            for (int g = 0; g < r->nguard; g++)
                guard[g] = (dl_lit){ (uint32_t)local_guard[li][g], r->guard[g].neg };
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

    /* (local, lane) -> named ground atom, for facts + the differential check. A
     * global (arity 0) broadcasts the same atom to every lane, and so does an
     * atom whose arguments are all constants. */
    uint32_t *ground = malloc((size_t)npred * (size_t)nent * sizeof *ground);
    for (int a = 0; a < npred; a++)
        for (int e = 0; e < nent; e++)
            ground[(size_t)a * nent + e] = lane_cell_atom(p, &L, a, NULL, S, e, NULL);
    world_add_lane_family(p->w, f, npred, nent, 1, ground, L.kind);
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
                         int nvars, bool is_head, int *roles, uint32_t *consts)
{
    if (a->value != INTERN_NONE || a->is_guard || a->is_num_effect || a->is_expr_guard)
        return false;
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || pi->is_mv || pi->is_num || pi->arity != a->nargs)
        return false;
    if (pi->is_provider && is_head)
        return false;                              /* read-only: never concluded */
    /* a derived body/guard pred is allowed: it imports (§5.5); a provider one is
     * allowed too and becomes a host-filled column (#233) */
    bool binds = false;
    for (int k = 0; k < a->nargs; k++) {
        int rho = -1;
        for (int v = 0; v < nvars; v++)
            if (a->args[k].name == vars[v].name) { rho = v; break; }
        if (rho >= 0) {
            roles[k] = rho;                        /* a variable of this rule */
            consts[k] = INTERN_NONE;
            binds = true;
        } else if (entity_pos(p, pred_arg_sort(pi, k), a->args[k].name) >= 0) {
            roles[k] = ROLE_CONST;                 /* a named entity (#226) */
            consts[k] = a->args[k].name;
        } else {
            return false;                          /* not a variable of this rule,
                                                    * and not an entity of the
                                                    * declared argument sort */
        }
    }
    /* as in the single-variable gate: a head of constants alone concludes one
     * atom from every cell, so there is nothing to read out of the family */
    return binds || !is_head;
}

/* Does predicate `from`'s derivation transitively READ predicate `to`?
 * Template-level DFS down the judgment dependency graph (rules concluding the
 * current pred, either polarity, into their body/guard preds). */
static bool pred_feeds_dfs(parser *p, uint32_t from, uint32_t to, bool *seen)
{
    if (from == to) return true;
    int fi = pred_idx(p, from);
    if (fi < 0 || seen[fi]) return false;
    seen[fi] = true;
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *r = &p->rules[i];
        if (r->head.is_valuedef || r->head.is_kinddef || rule_is_kind(p, r))
            continue;
        if (r->head.pred != from) continue;
        for (int b = 0; b < r->nbody; b++)
            if (!r->body[b].is_member && !r->body[b].is_expr_guard &&
                pred_feeds_dfs(p, r->body[b].pred, to, seen))
                return true;
        for (int g = 0; g < r->nguard; g++)
            if (!r->guard[g].is_member && !r->guard[g].is_expr_guard &&
                pred_feeds_dfs(p, r->guard[g].pred, to, seen))
                return true;
    }
    return false;
}

static bool pred_feeds(parser *p, uint32_t from, uint32_t to)
{
    bool seen[MAX_PREDS] = { false };
    return pred_feeds_dfs(p, from, to, seen);
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

    /* Routing soundness (#109, and a latent bug it exposed): this ONE-RULE
     * family claims world_query routing for its head atoms, so it must BE the
     * head pred's whole proof cone — no other rule may conclude or attack the
     * pred (the rule's own `unless` defeater is in-family), and the rule must
     * not be recursive: a per-iteration slice cannot see sibling iterations'
     * conclusions, so a recursive relation would refute its own transitive
     * pairs. Recursive rules stay on the N=1 family (their lanes/matcher
     * story is the #44 derived-body widening); multi-rule preds await a
     * family that carries the full cone. */
    for (int i = 0; i < p->nrules; i++) {
        ast_rule *o = &p->rules[i];
        if (o == r) continue;
        if (o->head.is_valuedef || o->head.is_kinddef || rule_is_kind(p, o))
            continue;
        if (o->head.pred == r->head.pred)
            return;
    }
    for (int b = 0; b < r->nbody; b++)
        if (!r->body[b].is_member && !r->body[b].is_expr_guard &&
            find_pred(p, r->body[b].pred) &&
            pred_feeds(p, r->body[b].pred, r->head.pred))
            return;
    for (int g = 0; g < r->nguard; g++)
        if (!r->guard[g].is_member && !r->guard[g].is_expr_guard &&
            find_pred(p, r->guard[g].pred) &&
            pred_feeds(p, r->guard[g].pred, r->head.pred))
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
    uint32_t constslot[1 + 2 * MAX_BODY][MAX_ARGS];
    int nat = 0;
    ats[nat] = &r->head;
    if (!join_atom_ok(p, &r->head, r->vars, r->nvars, true, roleslot[nat],
                      constslot[nat])) return;
    nat++;
    for (int b = 0; b < r->nbody; b++) {
        ats[nat] = &r->body[b];
        if (!join_atom_ok(p, &r->body[b], r->vars, r->nvars, false, roleslot[nat],
                          constslot[nat])) return;
        nat++;
    }
    for (int g = 0; g < r->nguard; g++) {
        ats[nat] = &r->guard[g];
        if (!join_atom_ok(p, &r->guard[g], r->vars, r->nvars, false, roleslot[nat],
                          constslot[nat])) return;
        nat++;
    }

    /* Family-local atoms, one per (predicate, argument pattern) — see
     * lane_locals: near(X,Y) and near(Y,X) are two columns, and so are
     * holding(X, key1) and holding(X, key2). */
    lane_locals L;
    L.n = 0;
    int local_of[1 + 2 * MAX_BODY];
    for (int k = 0; k < nat; k++) {
        int j = lane_local_of(p, &L, r, ats[k]->pred, ats[k]->nargs,
                              roleslot[k], constslot[k]);
        if (j < 0)
            return;                                /* local table full: no family */
        local_of[k] = j;
    }
    int npred = L.n;

    /* classify locals: the head (local_of[0]) is concluded here; a body/guard
     * pred that is neither a base fluent nor a provider is DERIVED elsewhere and
     * imported (its verdict injected per cell at solve time). */
    for (int j = 0; j < npred; j++)
        if (L.kind[j] == WORLD_LANE_DERIVED && j != local_of[0])
            L.kind[j] = WORLD_LANE_IMPORT;

    dlcol *f = dlcol_new(npred, nent);
    for (int a = 0; a < npred; a++) {
        char nbuf[MAX_NAME * 2 + 8];
        lane_local_name(p, &L, a, nbuf, sizeof nbuf);
        dlcol_set_atom_name(f, (uint32_t)a, nbuf);
    }

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
     * args, for each iterated role v the entity picked out of var v's domain by
     * the iteration `it` (decoded mixed-radix over the non-lane sorts), and for
     * a constant slot the entity the source named. */
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
            for (int e = 0; e < nent; e++)
                ground[((size_t)a * niter + it) * nent + e] =
                    lane_cell_atom(p, &L, a, r, Sl, e, vidx);
        }
    world_add_lane_family(p->w, f, npred, nent, (int)niter, ground, L.kind);
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
static const char *lane_cmp_spelling(world_cmp op)
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

/* A numeric LANDMARK guard on the lane var (#242) — `hp(X) >= 1`. Already an
 * ordinary boolean atom on the N=1 path (world_add_guard computes it from the
 * value store), so per lane it is one bit and the family can carry it as a
 * read-only column. Condition position only: a guard is derived, never written.
 * An expression guard (#130) is not this — its operands are not a single stored
 * number — and a primed guard is the #87 stratified case, which bails earlier. */
static bool step_guard_ok(parser *p, const ast_atom *a, int S, uint32_t var)
{
    if (!a->is_guard || a->is_expr_guard || a->primed || a->neg) return false;
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || !pi->is_fluent || !pi->is_num || pi->is_mv || pi->is_cell)
        return false;
    if (pi->arity != 1 || pred_arg_sort(pi, 0) != S) return false;
    return a->nargs == 1 && a->args[0].name == var;
}

/* A derived JUDGMENT read by the transition (#261) — arity-1 over the lane
 * sort, concluded by rules rather than stored. The judgment layer settles
 * before a step solves, so the verdict is queried per lane and injected
 * (WORLD_STEP_IMPORT), the step side's twin of the §5.5 judgment import.
 *
 * Condition position only: a judgment is derived, so writing one would be the
 * write-back I1 forbids — and the effect side already refuses it elsewhere.
 * A PRIMED read is the #87 stratification case and must keep bailing. */
static bool step_import_ok(parser *p, const ast_atom *a, int S, uint32_t var)
{
    if (a->is_guard || a->is_expr_guard || a->primed || a->is_member) return false;
    if (a->value != INTERN_NONE || a->is_defined) return false;
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || !pi->is_head) return false;             /* not concluded anywhere */
    if (pi->is_fluent || pi->is_provider || pi->is_num ||
        pi->is_mv || pi->is_cell || pi->is_value || pi->is_kindpred) return false;
    if (pi->arity != 1 || pred_arg_sort(pi, 0) != S) return false;
    return a->nargs == 1 && a->args[0].name == var;
}

static bool step_atom_ok(parser *p, const ast_atom *a, int S, uint32_t var,
                         bool is_effect)
{
    if (a->is_guard || a->is_num_effect || a->value != INTERN_NONE)
        return false;                              /* numeric guard / MV: out */
    pred_info *pi = find_pred(p, a->pred);
    if (!pi || !pi->is_fluent || pi->is_mv || pi->is_num)
        return false;                              /* a burst cue (#11) lands here
                                                    * too: no emit columns on the
                                                    * lanes, so the family bails
                                                    * and the world steps N=1 */
    if (pi->arity == 0)
        return !is_effect;                         /* global: read-only broadcast */
    if (pi->arity != 1 || pred_arg_sort(pi, 0) != S)
        return false;
    return a->nargs == 1 && a->args[0].name == var;
}

/* A verdict is 0 or 1, so an RHS whose only non-folding leaves are test() reads
 * takes at most 2^k values — all known at compile time (#165). Each such leaf
 * must already be a lane bit-column: a boolean fluent, arity-1 over S, read on
 * the action's own var. Then the commit is a table lookup indexed by the lane's
 * own verdict bits, and no expression VM runs per entity.
 *
 * Leaf identity is (pred, neg): `test(p(X))` and `test(~p(X))` are distinct
 * reads, and NOT complements — a literal can be neither proved nor refuted. */
#define MAX_LANE_TESTS 6                 /* 64 table entries; beyond this, N=1 */
typedef struct { uint32_t pred; bool neg; } lane_test;

/* Gather the distinct laneable test() leaves of `e`; false if any leaf is
 * something a constant table cannot stand in for (a numeric read, a roll, a
 * host call, `prior`, or a test that is not a lane column). */
static bool collect_lane_tests(parser *p, int e, int S, uint32_t var,
                               lane_test *ts, int *nts)
{
    ex_node *n = &p->exprs[e];
    switch (n->kind) {
    case EX_CONST: case EX_ENT: return true;
    case EX_LOAD: case EX_ROLL: case EX_CALL: case EX_PRIOR:
        return false;
    case EX_TEST: {
        pred_info *ti = find_pred(p, n->pred);
        if (!ti || !ti->is_fluent || ti->is_mv || ti->is_num) return false;
        if (ti->arity != 1 || pred_arg_sort(ti, 0) != S) return false;
        if (n->nargs != 1 || n->args[0].name != var) return false;
        bool neg = n->konst != 0;
        for (int i = 0; i < *nts; i++)
            if (ts[i].pred == n->pred && ts[i].neg == neg) return true;
        if (*nts >= MAX_LANE_TESTS) return false;
        ts[*nts].pred = n->pred; ts[*nts].neg = neg; (*nts)++;
        return true;
    }
    case EX_NEG: return collect_lane_tests(p, n->lhs, S, var, ts, nts);
    default:
        return collect_lane_tests(p, n->lhs, S, var, ts, nts)
            && collect_lane_tests(p, n->rhs, S, var, ts, nts);
    }
}

/* expr_fold under an assignment of the collected tests: bit i of `mask` is the
 * verdict of ts[i]. Deliberately delegates every operator to expr_fold's own
 * arithmetic by folding children first, so the table cannot drift from what the
 * N=1 effect VM computes — that identity is what makes world_step_lanes_check
 * agree by construction rather than by luck. */
static bool fold_under_tests(parser *p, int e, const lane_test *ts, int nts,
                             unsigned mask, long *out)
{
    ex_node *n = &p->exprs[e];
    long a, b;
    if (n->kind == EX_TEST) {
        bool neg = n->konst != 0;
        for (int i = 0; i < nts; i++)
            if (ts[i].pred == n->pred && ts[i].neg == neg) {
                *out = (mask >> i) & 1u;
                return true;
            }
        return false;
    }
    if (n->kind == EX_CONST) { *out = n->konst; return true; }
    if (n->kind == EX_NEG) {
        if (!fold_under_tests(p, n->lhs, ts, nts, mask, &a)) return false;
        *out = -a; return true;
    }
    if (n->kind == EX_LOAD || n->kind == EX_ROLL || n->kind == EX_CALL
        || n->kind == EX_PRIOR)
        return false;
    if (!fold_under_tests(p, n->lhs, ts, nts, mask, &a)
        || !fold_under_tests(p, n->rhs, ts, nts, mask, &b))
        return false;
    switch (n->kind) {
    case EX_ADD: *out = a + b; return true;
    case EX_SUB: *out = a - b; return true;
    case EX_MUL: *out = a * b; return true;
    case EX_DIV:                            /* floored, matching EXPR_DIV exactly */
        if (b == 0) return false;           /* the VM defines x/0 = 0: stays dynamic */
        *out = a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
        return true;
    case EX_MIN: *out = a < b ? a : b; return true;
    case EX_MAX: *out = a > b ? a : b; return true;
    default: return false;
    }
}

/* S1: a numeric effect laneable iff it writes a numeric fluent arity-1 over S on
 * the action's own var and its RHS is a constant *given the verdicts it reads*.
 * `ts`/`*nts` receive the distinct test columns (0 for a plain constant RHS) and
 * `table` the 2^nts folded values, indexed by those verdicts as bits. */
static bool num_eff_ok(parser *p, const ast_atom *e, int S, uint32_t var,
                       lane_test *ts, int *nts, long *table)
{
    pred_info *pi = find_pred(p, e->pred);
    if (!pi || !pi->is_fluent || !pi->is_num) return false;
    if (pi->arity != 1 || pred_arg_sort(pi, 0) != S) return false;
    if (e->nargs != 1 || e->args[0].name != var) return false;
    *nts = 0;
    if (!collect_lane_tests(p, e->expr_root, S, var, ts, nts)) return false;
    for (unsigned m = 0; m < (1u << *nts); m++)
        if (!fold_under_tests(p, e->expr_root, ts, *nts, m, &table[m]))
            return false;
    return true;
}

#define MAX_LANE_NUMEFF 512

static void emit_step_lanes(parser *p)
{
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
        if (S < 0) S = pred_arg_sort(pi, 0);
        else if (pred_arg_sort(pi, 0) != S) return;   /* multi-sort: bail */
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
    /* distinct numeric landmark guards read anywhere (#242): `hp(X) >= 1` is one
     * read-only bit per lane, computed from the value store at fact-load. Keyed
     * by (pred, cmp, threshold) — the same fluent at two thresholds is two
     * columns, the same way two arguments are two columns (#235). */
    struct { uint32_t pred; world_cmp cmp; long thr; } gd[MAX_PREDS];
    int ngd = 0;
    /* distinct derived judgments the transition reads (#261): one read-only
     * column each, filled from the settled judgment layer at fact-load */
    uint32_t imp[MAX_PREDS];
    int nimp = 0;
    bool act_has_num[MAX_ACTIONS], act_is_binder[MAX_ACTIONS];
    for (int i = 0; i < p->nactions; i++) { act_has_num[i] = false; act_is_binder[i] = false; }
    int neff_act[MAX_LANE_NUMEFF], neff_schema[MAX_LANE_NUMEFF], neff_op[MAX_LANE_NUMEFF];
    long neff_konst[MAX_LANE_NUMEFF];
    /* #165: a test()-reading RHS is a table of 2^k constants over k verdict
     * columns. `static` because the table array is 256KB — too much stack — and
     * this pass has many early returns, so a malloc would need unwinding at each.
     * Safe: emit_step_lanes is a single non-recursive compiler pass and every
     * entry is written before it is read. */
    static int neff_ntest[MAX_LANE_NUMEFF];
    static lane_test neff_test[MAX_LANE_NUMEFF][MAX_LANE_TESTS];
    static long neff_table[MAX_LANE_NUMEFF][1 << MAX_LANE_TESTS];
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
             * the lane axis and the cast is a broadcast trigger. One binder over
             * S, no caster-side requires/effects, boolean where/when guards over
             * the target (arity-1 over S), and constant-RHS numeric effects on it.
             * The cast is either per-caster (`fireball(C)`, one cast atom per
             * caster) or CASTERLESS (`sweep:`, #240) — a setup/broadcast action
             * whose single arity-0 atom drives the same bcast local. */
            if (a->neff != 0 || a->nreq != 0 || a->nvars > 1 || a->nbind != 1)
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
                lane_test bts[MAX_LANE_TESTS];
                int bnts = 0;
                long btab[1 << MAX_LANE_TESTS];
                if (!item->eff.is_num_effect
                    || !num_eff_ok(p, &item->eff, S, tv, bts, &bnts, btab)
                    || bnts != 0)                /* binder casts: constant RHS only */
                    return;
                k = btab[0];
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
            if (step_import_ok(p, &a->requires[b], S, var)) {      /* #261 column */
                uint32_t ip = a->requires[b].pred;
                int found = -1;
                for (int j = 0; j < nimp; j++) if (imp[j] == ip) { found = j; break; }
                if (found < 0) { if (nimp >= MAX_PREDS) return; imp[nimp++] = ip; }
                continue;
            }
            if (step_guard_ok(p, &a->requires[b], S, var)) {       /* #242 column */
                const ast_atom *g = &a->requires[b];
                int found = -1;
                for (int j = 0; j < ngd; j++)
                    if (gd[j].pred == g->pred && gd[j].cmp == g->cmp &&
                        gd[j].thr == g->threshold) { found = j; break; }
                if (found < 0) {
                    if (ngd >= MAX_PREDS) return;
                    gd[ngd].pred = g->pred; gd[ngd].cmp = g->cmp;
                    gd[ngd].thr = g->threshold; ngd++;
                }
                continue;
            }
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
                lane_test ts[MAX_LANE_TESTS];
                int nts = 0;
                long table[1 << MAX_LANE_TESTS];
                if (!num_eff_ok(p, e, S, var, ts, &nts, table))
                    return;                                  /* not laneable -> N=1 */
                if (nne >= MAX_LANE_NUMEFF) return;
                int sc = -1;
                for (int j = 0; j < nnp; j++)
                    if (p->preds[numpred[j]].pred == e->pred) { sc = j; break; }
                if (sc < 0) return;
                neff_act[nne] = i; neff_schema[nne] = sc;
                neff_op[nne] = (int)e->numop; neff_konst[nne] = table[0];
                /* the verdict columns this RHS reads, and its 2^k folded values */
                neff_ntest[nne] = nts;
                for (int j = 0; j < nts; j++) neff_test[nne][j] = ts[j];
                for (unsigned m = 0; m < (1u << nts); m++)
                    neff_table[nne][m] = table[m];
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
    int nloc = 2 * nf + ng + na + nmark + nbcast + nbitem + ngd + nimp;
    dlcol *f = dlcol_new(nloc, nent);
    int cur_local[MAX_PREDS], pri_local[MAX_PREDS], glob_local[MAX_PREDS];
    int gd_local[MAX_PREDS], imp_local[MAX_PREDS];
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
    /* imported judgments (#261): read-only per-lane columns whose bits come
     * from the settled judgment layer, not from any rule in this family. */
    for (int j = 0; j < nimp; j++) {
        imp_local[j] = n; kind[n] = WORLD_STEP_IMPORT;
        dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, imp[j]));
        n++;
    }
    /* numeric landmark guards (#242): read-only per-lane columns filled from the
     * value store. Named as the author wrote them, so the trace reads the same
     * on both paths. */
    for (int j = 0; j < ngd; j++) {
        gd_local[j] = n; kind[n] = WORLD_STEP_GUARD;
        snprintf(nbuf, sizeof nbuf, "%s %s %ld", intern_name(p->syms, gd[j].pred),
                 lane_cmp_spelling(gd[j].cmp), gd[j].thr);
        dlcol_set_atom_name(f, (uint32_t)n, nbuf);
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
            if (step_import_ok(p, &a->requires[b], S, a->vars[0].name)) {
                int ij = -1;                           /* #261: an import column */
                for (int j = 0; j < nimp; j++) if (imp[j] == rp) { ij = j; break; }
                body[bi++] = (dl_lit){ (uint32_t)imp_local[ij], a->requires[b].neg };
                continue;
            }
            if (a->requires[b].is_guard) {             /* #242: a guard column */
                int gj = -1;
                for (int j = 0; j < ngd; j++)
                    if (gd[j].pred == rp && gd[j].cmp == a->requires[b].cmp &&
                        gd[j].thr == a->requires[b].threshold) { gj = j; break; }
                body[bi++] = (dl_lit){ (uint32_t)gd_local[gj], false };
                continue;
            }
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
    for (int j = 0; j < nimp; j++)
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            ground[(size_t)imp_local[j] * nent + e] = ground_pred(p, imp[j], &ent, 1);
        }
    /* a guard column's ground atom is the N=1 path's guard literal, so a trace
     * naming it reads identically whichever path produced it (#242) */
    for (int j = 0; j < ngd; j++)
        for (int e = 0; e < nent; e++) {
            uint32_t ent = domain_at(p, S, e);
            uint32_t num = ground_pred(p, gd[j].pred, &ent, 1);
            snprintf(nbuf, sizeof nbuf, "%s %s %ld", intern_name(p->syms, num),
                     lane_cmp_spelling(gd[j].cmp), gd[j].thr);
            ground[(size_t)gd_local[j] * nent + e] = intern_id(p->syms, nbuf);
        }
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
        /* #165: per spec, the verdict columns its RHS reads and its 2^k table —
         * the table is pointed at in place (neff_table, or the constant itself
         * for a binder item) rather than copied into a second 512KB buffer */
        int sc_ntest[2 * MAX_LANE_NUMEFF];
        uint32_t sc_tloc[2 * MAX_LANE_NUMEFF][MAX_LANE_TESTS];
        uint8_t sc_tneg[2 * MAX_LANE_NUMEFF][MAX_LANE_TESTS];
        const long *sc_table[2 * MAX_LANE_NUMEFF];
        for (int k = 0; k < nne; k++) {
            sc_schema[nspec] = neff_schema[k]; sc_op[nspec] = neff_op[k];
            sc_konst[nspec] = neff_konst[k];
            effmark[nspec] = (uint32_t)marker_local[neff_act[k]];
            sc_ntest[nspec] = neff_ntest[k];
            for (int j = 0; j < neff_ntest[k]; j++) {
                int fi = -1;                        /* the tested pred's CUR column */
                for (int q = 0; q < nf; q++)
                    if (p->preds[fpred[q]].pred == neff_test[k][j].pred) { fi = q; break; }
                if (fi < 0) { free(numcell); return; }   /* not a lane column: N=1 */
                sc_tloc[nspec][j] = (uint32_t)cur_local[fi];
                sc_tneg[nspec][j] = neff_test[k][j].neg ? 1 : 0;
            }
            sc_table[nspec] = neff_table[k];
            nspec++;
        }
        for (int k = 0; k < nbitem; k++) {
            sc_schema[nspec] = bitem_schema[k]; sc_op[nspec] = bitem_op[k];
            sc_konst[nspec] = bitem_konst[k];
            effmark[nspec] = (uint32_t)bmarker[k];
            sc_ntest[nspec] = 0; sc_table[nspec] = &bitem_konst[k];
            nspec++;
        }
        world_step_lane_set_numeric(p->w, nnp, numcell, nspec,
                                    sc_schema, sc_op, sc_konst, effmark,
                                    sc_ntest, &sc_tloc[0][0], &sc_tneg[0][0],
                                    sc_table, MAX_LANE_TESTS);

        /* Receipt provenance (#88). A laned effect has no ground instance name
         * — one schema rule runs for all lanes — so a contribution's identity
         * is the authored rule plus (its variable, this lane's entity). The
         * lane entities are the sort's own order, the same order `ground` was
         * filled in, so a receipt row on the routed path carries exactly what
         * the N=1 row's binding does. */
        const char *sc_name[2 * MAX_LANE_NUMEFF];
        uint32_t sc_pred[2 * MAX_LANE_NUMEFF], sc_var[2 * MAX_LANE_NUMEFF];
        int ns = 0;
        for (int k = 0; k < nne; k++, ns++) {
            ast_action *a = &p->actions[neff_act[k]];
            sc_name[ns] = a->name;
            sc_pred[ns] = intern_id(p->syms, a->name);
            sc_var[ns]  = a->nvars > 0 ? a->vars[0].name : INTERN_NONE;
        }
        for (int k = 0; k < nbitem; k++, ns++) {
            ast_action *a = &p->actions[bitem_act[k]];
            ast_binder *bnd = &p->binders[a->bind_ix[0]];
            sc_name[ns] = a->name;
            sc_pred[ns] = intern_id(p->syms, a->name);
            sc_var[ns]  = bnd->nvars > 0 ? bnd->vars[0].name : INTERN_NONE;
                                          /* the TARGET var: a binder's lane
                                           * axis is the bound one, not the cast's */
        }
        uint32_t *lane_ents = malloc((size_t)(nent ? nent : 1) * sizeof *lane_ents);
        for (int e = 0; e < nent; e++) lane_ents[e] = domain_at(p, S, e);
        world_step_lane_set_prov(p->w, lane_ents, nent, sc_name, sc_pred, sc_var,
                                 ns);
        free(lane_ents);
        free(numcell);
    }

    /* register broadcast cast atoms: every ground `action(caster)` -> its BCAST
     * local, so the discrete cast fans out over the target lanes. */
    if (nbcast > 0) {
        int total = 0;
        for (int i = 0; i < p->nactions; i++)
            if (act_is_binder[i])
                total += p->actions[i].nvars == 1
                       ? domain_size(p, p->actions[i].vars[0].sort)
                       : 1;                        /* casterless: one arity-0 atom */
        uint32_t *catom = malloc((size_t)(total ? total : 1) * sizeof *catom);
        int *clocal = malloc((size_t)(total ? total : 1) * sizeof *clocal);
        int ncast = 0;
        for (int i = 0; i < p->nactions; i++) if (act_is_binder[i]) {
            uint32_t nameatom = intern_id(p->syms, p->actions[i].name);
            if (p->actions[i].nvars == 0) {   /* casterless cast: the bare atom */
                catom[ncast] = nameatom;
                clocal[ncast] = bcast_local[i];
                ncast++;
                continue;
            }
            int Sc = p->actions[i].vars[0].sort, kc = domain_size(p, Sc);
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

    /* register the numeric guard columns: per (guard, lane) the ground numeric
     * atom that lane compares, resolved to a value-store index once (#242). */
    if (ngd > 0) {
        size_t nc = (size_t)ngd * (size_t)(nent ? nent : 1);
        uint32_t *gloc = malloc((size_t)ngd * sizeof *gloc);
        uint32_t *gcell = malloc(nc * sizeof *gcell);
        int *gop = malloc((size_t)ngd * sizeof *gop);
        long *gthr = malloc((size_t)ngd * sizeof *gthr);
        for (int j = 0; j < ngd; j++) {
            gloc[j] = (uint32_t)gd_local[j];
            gop[j]  = (int)gd[j].cmp;
            gthr[j] = gd[j].thr;
            for (int e = 0; e < nent; e++) {
                uint32_t ent = domain_at(p, S, e);
                gcell[(size_t)j * nent + e] = ground_pred(p, gd[j].pred, &ent, 1);
            }
        }
        world_step_lane_set_guards(p->w, ngd, gloc, gcell, gop, gthr);
        free(gloc); free(gcell); free(gop); free(gthr);
    }
    free(ground);
    free(kind);
}

/* #121 slice 2: per-value step lane families for a `split` world, plus the
 * coverage marks that carve the N=1 residue. Unlike emit_step_lanes this is
 * PER-RULE, not all-or-nothing: each action/ramification is classified —
 * lane-eligible (per-actor boolean over one sort, split guards acting as the
 * value selector), or residue (numerics, binders, MV effects incl. the split
 * fluent's own writers, multi-var, provider/expr guards — everything the N=1
 * path already handles). One family is emitted per split value, holding that
 * value's covered rules with their split guards DROPPED (statically true
 * there); world_step solves it first and the residue on N=1 (mixed routing).
 *
 * Soundness bail: if a residue rule writes an arity-1-over-S boolean fluent,
 * one fluent's writers would straddle the two halves (the lane solve could
 * not see the residue's contribution) — no families are built and the world
 * steps pure N=1, which is always correct. Numerics and binder effects ride
 * the residue pipeline; another (non-split) MV fluent bails like today. */
static void emit_step_lanes_split(parser *p)
{
    if (p->has_pguards)
        return;

    pred_info *spi = find_pred(p, p->split_pred);
    if (!spi || spi->nvalues < 2 || spi->nvalues > 31)
        return;

    /* the lane sort S and its boolean per-actor fluents; anything else is
     * residue material, not a bail — except a second MV fluent (like today) */
    int S = -1, fpred[MAX_PREDS], nf = 0;
    for (int i = 0; i < p->npreds; i++) {
        pred_info *pi = &p->preds[i];
        if (!pi->is_fluent || pi->pred == p->split_pred)
            continue;
        if (pi->is_mv)
            return;                                /* a non-split MV: not laned yet */
        if (pi->arity != 1 || pi->is_num || pi->is_cell)
            continue;                              /* global / numeric / n-ary: residue */
        if (S < 0) S = pred_arg_sort(pi, 0);
        if (pred_arg_sort(pi, 0) != S)
            continue;                              /* other-sort fluent: residue */
        fpred[nf++] = i;
    }
    if (nf == 0 || S < 0)
        return;
    int nent = domain_size(p, S);
    if (nent == 0)
        return;

    /* classify: -4 residue, -3 dead (contradictory selectors),
     * -2 lane in every value, >= 0 lane in that value only */
    int8_t cls[MAX_ACTIONS];
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        int sel = -2;
        cls[i] = -4;
        if (a->nbind > 0 || a->nvars != 1 || a->vars[0].sort != S)
            continue;
        uint32_t var = a->vars[0].name;
        bool ok = true;
        for (int b = 0; b < a->nreq && ok; b++) {
            ast_atom *at = &a->requires[b];
            if (at->is_member) { ok = false; break; }
            if (at->pred == p->split_pred) {
                if (at->value == INTERN_NONE || at->neg || at->primed) {
                    ok = false;                    /* odd split read: residue */
                    break;
                }
                int v = -1;
                for (int k = 0; k < spi->nvalues; k++)
                    if (spi->values[k] == at->value) { v = k; break; }
                if (v < 0) { ok = false; break; }
                if (sel == -2)      sel = v;
                else if (sel != v)  sel = -3;      /* two values: never fires */
                continue;
            }
            if (!step_atom_ok(p, at, S, var, false))
                ok = false;
        }
        for (int b = 0; b < a->neff && ok; b++) {
            ast_atom *e = &a->effects[b];
            if (e->is_num_effect || e->value != INTERN_NONE ||
                !step_atom_ok(p, e, S, var, true))
                ok = false;
        }
        if (ok)
            cls[i] = (int8_t)sel;
    }

    /* soundness: no residue writer of a lane fluent (see header) */
    int nlane = 0;
    for (int i = 0; i < p->nactions; i++) {
        if (cls[i] == -2 || cls[i] >= 0) { nlane++; continue; }
        if (cls[i] == -3)
            continue;                              /* never fires anywhere */
        ast_action *a = &p->actions[i];
        for (int b = 0; b < a->neff; b++) {
            ast_atom *e = &a->effects[b];
            if (e->is_num_effect || e->value != INTERN_NONE)
                continue;
            pred_info *pi = find_pred(p, e->pred);
            if (pi && pi->is_fluent && !pi->is_mv && !pi->is_num &&
                pi->arity == 1 && pi->argsort[0] == S)
                return;                            /* straddled writer: pure N=1 */
        }
        for (int bi = 0; bi < a->nbind; bi++) {
            ast_binder *bnd = &p->binders[a->bind_ix[bi]];
            for (int it = 0; it < bnd->nitems; it++) {
                ast_atom *e = &bnd->items[it].eff;
                if (e->is_num_effect || e->value != INTERN_NONE)
                    continue;
                pred_info *pi = find_pred(p, e->pred);
                if (pi && pi->is_fluent && !pi->is_mv && !pi->is_num &&
                    pi->arity == 1 && pi->argsort[0] == S)
                    return;
            }
        }
    }
    if (nlane == 0)
        return;

    /* one family per split value: covered = selector-matches or unguarded.
     * Each family carries ONLY the per-actor fluents its covered rules touch
     * (read, primed-read, or write) — the per-value write-set narrowing on
     * the lane side; an untouched fluent has no columns, no inertia, and
     * commits by copy through the residue's flw logic. */
    char nbuf[MAX_GROUND + 2];
    for (int v = 0; v < spi->nvalues; v++) {
        uint32_t apred[MAX_ACTIONS], glob[MAX_PREDS];
        int fpred_v[MAX_PREDS], nfv = 0;
        int na = 0, ng = 0, ncov = 0;
        for (int i = 0; i < p->nactions; i++) {
            if (!(cls[i] == -2 || cls[i] == v))
                continue;
            ncov++;
            ast_action *a = &p->actions[i];
            if (!a->is_ramif) {
                uint32_t tr = intern_id(p->syms, a->name);
                int found = -1;
                for (int j = 0; j < na; j++) if (apred[j] == tr) { found = j; break; }
                if (found < 0) { if (na >= MAX_ACTIONS) return; apred[na++] = tr; }
            }
            for (int b = 0; b < a->nreq + a->neff; b++) {
                ast_atom *at = b < a->nreq ? &a->requires[b]
                                           : &a->effects[b - a->nreq];
                if (at->pred == p->split_pred)
                    continue;                      /* erased: statically true here */
                pred_info *pi = find_pred(p, at->pred);
                if (pi->arity == 0) {
                    int found = -1;
                    for (int j = 0; j < ng; j++)
                        if (glob[j] == at->pred) { found = j; break; }
                    if (found < 0) { if (ng >= MAX_PREDS) return; glob[ng++] = at->pred; }
                    continue;
                }
                int fi = step_fidx(p, fpred, nf, at->pred);
                int found = -1;
                for (int j = 0; j < nfv; j++)
                    if (fpred_v[j] == fpred[fi]) { found = j; break; }
                if (found < 0) fpred_v[nfv++] = fpred[fi];
            }
        }
        if (ncov == 0)
            continue;                              /* this value steps pure N=1 */

        int nloc = 2 * nfv + ng + na;
        dlcol *f = dlcol_new(nloc, nent);
        int cur_local[MAX_PREDS], pri_local[MAX_PREDS], glob_local[MAX_PREDS];
        int inertia_pos[MAX_PREDS], inertia_neg[MAX_PREDS], act_local[MAX_ACTIONS];
        uint8_t *kind = malloc((size_t)nloc * sizeof *kind);
        int n = 0;
        for (int i = 0; i < nfv; i++) {
            uint32_t P = p->preds[fpred_v[i]].pred;
            cur_local[i] = n; kind[n] = WORLD_STEP_CUR;
            dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, P));
            n++;
            pri_local[i] = n; kind[n] = WORLD_STEP_PRIMED;
            snprintf(nbuf, sizeof nbuf, "%s'", intern_name(p->syms, P));
            dlcol_set_atom_name(f, (uint32_t)n, nbuf);
            n++;
        }
        for (int j = 0; j < ng; j++) {
            glob_local[j] = n; kind[n] = WORLD_STEP_CUR;
            dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, glob[j]));
            n++;
        }
        for (int j = 0; j < na; j++) {
            act_local[j] = n; kind[n] = WORLD_STEP_ACTION;
            dlcol_set_atom_name(f, (uint32_t)n, intern_name(p->syms, apred[j]));
            n++;
        }

        char rbuf[MAX_NAME + 16];
        for (int i = 0; i < nfv; i++) {
            const char *fname = intern_name(p->syms, p->preds[fpred_v[i]].pred);
            snprintf(rbuf, sizeof rbuf, "inertia on %s", fname);
            dl_lit cur = { (uint32_t)cur_local[i], false },
                   pri = { (uint32_t)pri_local[i], false };
            inertia_pos[i] = dlcol_add_rule(f, rbuf, DL_DEFEASIBLE, pri, &cur, 1);
            dl_lit ncur = dl_complement(cur), npri = dl_complement(pri);
            inertia_neg[i] = dlcol_add_rule(f, rbuf, DL_DEFEASIBLE, npri, &ncur, 1);
        }

        for (int i = 0; i < p->nactions; i++) {
            if (!(cls[i] == -2 || cls[i] == v))
                continue;
            ast_action *a = &p->actions[i];
            dl_lit body[MAX_BODY + 1];
            int bi = 0;
            for (int b = 0; b < a->nreq; b++) {
                ast_atom *at = &a->requires[b];
                if (at->pred == p->split_pred)
                    continue;                      /* the value selector, erased */
                int loc;
                if (find_pred(p, at->pred)->arity == 0) {
                    int gj = -1;
                    for (int j = 0; j < ng; j++) if (glob[j] == at->pred) { gj = j; break; }
                    loc = glob_local[gj];
                } else {
                    int fi = step_fidx(p, fpred_v, nfv, at->pred);
                    loc = at->primed ? pri_local[fi] : cur_local[fi];
                }
                body[bi++] = (dl_lit){ (uint32_t)loc, at->neg };
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
                int fi = step_fidx(p, fpred_v, nfv, a->effects[b].pred);
                dl_lit head = { (uint32_t)pri_local[fi], a->effects[b].neg };
                char cname[MAX_NAME + 8];
                snprintf(cname, sizeof cname, "%s/%s%s", a->name,
                         a->effects[b].neg ? "~" : "",
                         intern_name(p->syms, a->effects[b].pred));
                int rid = dlcol_add_rule(f, cname, DL_DEFEASIBLE, head, body, bi);
                dlcol_set_prov(f, rid, pbuf);
                dlcol_add_sup(f, rid, a->effects[b].neg ? inertia_pos[fi]
                                                        : inertia_neg[fi]);
            }
        }

        uint32_t *ground = malloc((size_t)nloc * nent * sizeof *ground);
        for (int i = 0; i < nfv; i++) {
            uint32_t P = p->preds[fpred_v[i]].pred;
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
                ground[(size_t)glob_local[j] * nent + e] = glob[j];
        for (int j = 0; j < na; j++)
            for (int e = 0; e < nent; e++) {
                uint32_t ent = domain_at(p, S, e);
                ground[(size_t)act_local[j] * nent + e] =
                    ground_pred(p, apred[j], &ent, 1);
            }

        world_add_step_lane_family(p->w, f, nloc, nent, ground, kind);
        world_step_lane_bind_value(p->w, v);
        free(ground);
        free(kind);
    }

    /* coverage marks: the residue schema omits these srules per value */
    uint32_t all = (1u << spi->nvalues) - 1;
    for (int i = 0; i < p->nactions; i++) {
        if (!(cls[i] == -2 || cls[i] >= 0))
            continue;
        uint32_t mask = cls[i] == -2 ? all : (1u << cls[i]);
        for (int h = p->actions[i].srule_lo; h < p->actions[i].srule_hi; h++)
            world_step_rule_set_lane_cover(p->w, h, mask);
    }
}

static void build_lane_families(parser *p)
{
    if (p->nactions > 0) {         /* a step world: lane the transition (first cut) */
        if (p->split_pred)
            emit_step_lanes_split(p);   /* #121: per-value families + N=1 residue */
        else
            emit_step_lanes(p);    /* bails unless nrules==0 + homogeneous over S */
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

/* ---- the §6.3 interface artifact ------------------------------------------
 *
 * The declared vocabulary as DATA: entities and their sorts, state with its
 * domains, providers and functions, derived values, emissions, judgment heads,
 * action signatures, and the protocol declarations a client has to honour
 * (`exclusive` groups, the `split` fluent). It is the compile-time twin of
 * world.h's runtime contract, and the extension point every client checks
 * against — the typed JS binding is the first backend, and any future front end
 * fact-checks against this same artifact rather than reaching into the compiler.
 *
 * JSON because the consumers are out-of-process and out-of-language. The one
 * thing a reader cannot infer and must not guess is how a ground atom is
 * spelled, so the artifact states it: `pred(e1,e2)`, bare at arity 0, and a
 * multi-valued fluent's value atoms as `pred(args)=value`. Get that wrong and a
 * client interns a fresh always-false atom — exactly the silent failure this
 * whole surface exists to kill. */

typedef struct { char *b; size_t n, cap; } sbuf;

static void sb_raw(sbuf *s, const char *t)
{
    size_t l = strlen(t);
    if (s->n + l + 1 > s->cap) {
        while (s->n + l + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 1024;
        s->b = realloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, t, l);
    s->n += l;
    s->b[s->n] = '\0';
}

static void sb_int(sbuf *s, long v)
{
    char t[32];
    snprintf(t, sizeof t, "%ld", v);
    sb_raw(s, t);
}

/* A JSON string. The .story identifier grammar is ASCII words, but a source
 * name is a path, so escape properly rather than assuming. */
static void sb_str(sbuf *s, const char *t)
{
    sb_raw(s, "\"");
    char esc[8];
    for (const unsigned char *c = (const unsigned char *)t; *c; c++) {
        if (*c == '"' || *c == '\\') { esc[0] = '\\'; esc[1] = (char)*c; esc[2] = 0; sb_raw(s, esc); }
        else if (*c == '\n') sb_raw(s, "\\n");
        else if (*c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *c); sb_raw(s, esc); }
        else { esc[0] = (char)*c; esc[1] = 0; sb_raw(s, esc); }
    }
    sb_raw(s, "\"");
}

static void sb_kv(sbuf *s, const char *k, const char *v)
{
    sb_str(s, k); sb_raw(s, ": "); sb_str(s, v);
}

/* `["actor", "item"]` for a pred_info's argument sorts. INT_SORT prints as
 * "int"; the #124 value meta-sort never reaches the artifact (kind predicates
 * are build-time vocabulary and are omitted entirely). */
static void sb_argsorts(sbuf *s, parser *p, const pred_info *pi)
{
    sb_raw(s, "[");
    for (int k = 0; k < pi->arity; k++) {
        if (k) sb_raw(s, ", ");
        sb_str(s, pi->argsort[k] == INT_SORT ? "int" : sort_name(p, pi->argsort[k]));
    }
    sb_raw(s, "]");
}

/* Argument sorts for a JUDGMENT head — the signature `infer_head_sorts` (#205)
 * settled from the rules that conclude it, since a head is not a declaration.
 * A slot no rule pinned down (arity known, sorts not) stays "?". */
static void sb_head_argsorts(sbuf *s, parser *p, const pred_info *pi)
{
    sb_raw(s, "[");
    for (int k = 0; k < pi->arity; k++) {
        if (k) sb_raw(s, ", ");
        sb_str(s, arg_sort_name(p, pi->headsort[k]));
    }
    sb_raw(s, "]");
}

/* The ast_fluent behind a declared predicate, for its domain details. */
static const ast_fluent *iface_fluent(parser *p, uint32_t pred)
{
    for (int i = 0; i < p->nfluents; i++)
        if (p->fluents[i].pred == pred) return &p->fluents[i];
    return NULL;
}

static void sb_state_entry(sbuf *s, parser *p, const pred_info *pi)
{
    const ast_fluent *f = iface_fluent(p, pi->pred);
    sb_raw(s, "{ ");
    sb_kv(s, "name", intern_name(p->syms, pi->pred));
    sb_raw(s, ", \"args\": ");
    sb_argsorts(s, p, pi);
    if (f && f->is_cell) {
        sb_raw(s, ", "); sb_kv(s, "type", "cell");
        sb_raw(s, ", "); sb_kv(s, "domain", intern_name(p->syms, f->val_sort));
    } else if (f && f->is_num) {
        sb_raw(s, ", "); sb_kv(s, "type", "int");
        if (f->has_range) {
            /* a dynamic bound is an expression, not a number: say so rather
             * than exporting a constant that is only sometimes the bound */
            if (f->rmin_expr < 0) { sb_raw(s, ", \"min\": "); sb_int(s, f->rmin); }
            if (f->rmax_expr < 0) { sb_raw(s, ", \"max\": "); sb_int(s, f->rmax); }
            if (f->rmin_expr >= 0 || f->rmax_expr >= 0)
                sb_raw(s, ", \"dynamic_bounds\": true");
        }
    } else if (f && f->is_mv) {
        sb_raw(s, ", "); sb_kv(s, "type", "enum");
        sb_raw(s, ", \"values\": [");
        for (int v = 0; v < f->nvalues; v++) {
            if (v) sb_raw(s, ", ");
            sb_str(s, intern_name(p->syms, f->values[v]));
        }
        sb_raw(s, "]");
        if (f->is_split) sb_raw(s, ", \"split\": true");
    } else {
        sb_raw(s, ", "); sb_kv(s, "type", "bool");
    }
    sb_raw(s, " }");
}

/* Emit the interface artifact for the parsed file. The caller owns the string. */
static char *harvest_iface(parser *p)
{
    sbuf s = { NULL, 0, 0 };
    sb_raw(&s, "{\n  \"artifact\": \"infeasible.interface\",\n  \"version\": 1,\n  ");
    sb_kv(&s, "story", p->srcname ? p->srcname : "<story>");
    if (p->has_scene) { sb_raw(&s, ",\n  "); sb_kv(&s, "scene", p->scene_name); }

    /* how a ground atom is spelled — the contract a client cannot guess */
    sb_raw(&s, ",\n  \"ground\": { ");
    sb_kv(&s, "atom", "pred(arg1,arg2)");
    sb_raw(&s, ", "); sb_kv(&s, "nullary", "pred");
    sb_raw(&s, ", "); sb_kv(&s, "value", "pred(args)=value");
    sb_raw(&s, ", "); sb_kv(&s, "separator", ",");
    sb_raw(&s, " }");

    sb_raw(&s, ",\n  \"sorts\": [");
    int sfirst = 1;
    for (int i = 0; i < p->nsorts; i++) {
        if (p->sorts[i].is_enum) continue;            /* listed under "enums" */
        sb_raw(&s, sfirst ? "\n    " : ",\n    "); sfirst = 0;
        sb_raw(&s, "{ ");
        sb_kv(&s, "name", p->sorts[i].name);
        sb_raw(&s, ", ");
        /* A COVER is published as its own kind, with its members (#231). It
         * must not read as a plain sort: a client that builds "entity -> its
         * sort" from these lists would find every covered entity under two
         * names and answer whichever it saw last. Members are named so a
         * client can still resolve the cover, and the entity list is emitted
         * as usual so `w.lit.*` can spell atoms over cover-typed predicates. */
        sb_kv(&s, "kind", p->sorts[i].is_domain ? "domain"
                          : p->sorts[i].is_union ? "union" : "sort");
        if (p->sorts[i].is_union) {
            sb_raw(&s, ", \"members\": [");
            for (int m = 0; m < p->sorts[i].nmem; m++) {
                if (m) sb_raw(&s, ", ");
                sb_str(&s, p->sorts[p->sorts[i].mem[m]].name);
            }
            sb_raw(&s, "]");
        }
        if (!p->sorts[i].is_domain) {
            sb_raw(&s, ", \"entities\": [");
            for (int e = 0; e < p->domain_n[i]; e++) {
                if (e) sb_raw(&s, ", ");
                sb_str(&s, intern_name(p->syms, p->domain_ents[i][e]));
            }
            sb_raw(&s, "]");
        }
        sb_raw(&s, " }");
    }
    sb_raw(&s, sfirst ? "]" : "\n  ]");

    sb_raw(&s, ",\n  \"enums\": [");
    for (int i = 0; i < p->nenums; i++) {
        sb_raw(&s, i ? ",\n    " : "\n    ");
        sb_raw(&s, "{ ");
        sb_kv(&s, "name", p->enums[i].name);
        sb_raw(&s, ", \"values\": [");
        for (int v = 0; v < p->enums[i].nvalues; v++) {
            if (v) sb_raw(&s, ", ");
            sb_str(&s, intern_name(p->syms, p->enums[i].values[v]));
        }
        sb_raw(&s, "] }");
    }
    sb_raw(&s, p->nenums ? "\n  ]" : "]");

    /* the predicate registry, classified — one pass, so a predicate cannot be
     * in two sections or missing from all of them */
    const char *sections[] = { "state", "providers", "values", "emits", "judgments" };
    for (int sec = 0; sec < 5; sec++) {
        sb_raw(&s, ",\n  \"");
        sb_raw(&s, sections[sec]);
        sb_raw(&s, "\": [");
        int first = 1;
        for (int i = 0; i < p->npreds; i++) {
            pred_info *pi = &p->preds[i];
            if (pi->is_kindpred) continue;            /* build-time vocabulary */
            int want = pi->is_fluent ? 0 : pi->is_provider ? 1 : pi->is_value ? 2
                     : pi->is_emit ? 3 : pi->is_head ? 4 : -1;
            if (want != sec) continue;
            sb_raw(&s, first ? "\n    " : ",\n    "); first = 0;
            if (sec == 0) { sb_state_entry(&s, p, pi); continue; }
            sb_raw(&s, "{ ");
            sb_kv(&s, "name", intern_name(p->syms, pi->pred));
            sb_raw(&s, ", \"args\": ");
            if (sec == 4) sb_head_argsorts(&s, p, pi);
            else          sb_argsorts(&s, p, pi);
            if (sec == 2) { sb_raw(&s, ", "); sb_kv(&s, "type", "int"); }
            sb_raw(&s, " }");
        }
        sb_raw(&s, first ? "]" : "\n  ]");
    }

    sb_raw(&s, ",\n  \"functions\": [");
    for (int i = 0; i < p->nfunctions; i++) {
        ast_function *fn = &p->functions[i];
        sb_raw(&s, i ? ",\n    " : "\n    ");
        sb_raw(&s, "{ ");
        sb_kv(&s, "name", intern_name(p->syms, fn->name));
        sb_raw(&s, ", \"args\": [");
        for (int k = 0; k < fn->nargs; k++) {
            if (k) sb_raw(&s, ", ");
            sb_str(&s, fn->argsort[k] ? intern_name(p->syms, fn->argsort[k]) : "int");
        }
        sb_raw(&s, "], ");
        sb_kv(&s, "returns", fn->ret ? intern_name(p->syms, fn->ret) : "int");
        sb_raw(&s, " }");
    }
    sb_raw(&s, p->nfunctions ? "\n  ]" : "]");

    /* actions carry their PARAMETER names as well as sorts: a generated
     * constructor wants `unlock(who)`, not `unlock(arg1)` */
    sb_raw(&s, ",\n  \"actions\": [");
    int first = 1;
    for (int i = 0; i < p->nactions; i++) {
        ast_action *a = &p->actions[i];
        if (a->is_ramif) continue;                    /* no trigger: not callable */
        sb_raw(&s, first ? "\n    " : ",\n    "); first = 0;
        sb_raw(&s, "{ ");
        sb_kv(&s, "name", a->name);
        sb_raw(&s, ", \"params\": [");
        for (int k = 0; k < a->nvars; k++) {
            if (k) sb_raw(&s, ", ");
            sb_raw(&s, "{ ");
            sb_kv(&s, "name", intern_name(p->syms, a->vars[k].name));
            sb_raw(&s, ", ");
            sb_kv(&s, "sort", sort_name(p, a->vars[k].sort));
            sb_raw(&s, " }");
        }
        sb_raw(&s, "] }");
    }
    sb_raw(&s, first ? "]" : "\n  ]");

    /* #159: the protocol a bound host must enforce at construction (§6.3's
     * builder), keyed exactly as world_step checks it. A `_` position never
     * constrains; named positions matched by name form the key. */
    sb_raw(&s, ",\n  \"exclusive\": [");
    for (int i = 0; i < p->nexcls; i++) {
        ast_excl *g = &p->excls[i];
        sb_raw(&s, i ? ",\n    " : "\n    ");
        sb_raw(&s, "{ \"members\": [");
        for (int m = 0; m < g->nmem; m++) {
            if (m) sb_raw(&s, ", ");
            sb_raw(&s, "{ ");
            sb_kv(&s, "action", g->mem[m].action);
            sb_raw(&s, ", \"key\": [");
            for (int k = 0; k < g->mem[m].nargs; k++) {
                if (k) sb_raw(&s, ", ");
                if (g->mem[m].vars[k])
                    sb_str(&s, intern_name(p->syms, g->mem[m].vars[k]));
                else
                    sb_raw(&s, "null");               /* `_`: never constrains */
            }
            sb_raw(&s, "] }");
        }
        sb_raw(&s, "] }");
    }
    sb_raw(&s, p->nexcls ? "\n  ]" : "]");
    sb_raw(&s, "\n}\n");
    return s.b ? s.b : strdup("{}\n");
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
    for (int i = 0; i < p->nemits; i++) {          /* burst cues (#11) */
        const char *nm = intern_name(p->syms, p->emits[i].pred);
        build_fluent_detail(det, sizeof det, p, &p->emits[i], "emit");
        sm_add_sym(m, nm, STORY_SYM_EMIT, p->emits[i].line, p->emits[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->emits[i].line, p->emits[i].col);
    }
    for (int i = 0; i < p->nkindpreds; i++) {      /* kind predicates (#124/#126):
                                                    * the detail says BUILD-TIME
                                                    * out loud (the LSP hover) */
        const char *nm = intern_name(p->syms, p->kindpreds[i].pred);
        build_fluent_detail(det, sizeof det, p, &p->kindpreds[i], "kind");
        size_t dl_ = strlen(det);
        snprintf(det + dl_, sizeof det - dl_, " — build-time");
        sm_add_sym(m, nm, STORY_SYM_VALUE, p->kindpreds[i].line,
                   p->kindpreds[i].col, det);
        sm_add_ref(m, nm, STORY_OCC_DECL, p->kindpreds[i].line,
                   p->kindpreds[i].col);
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
/* Defined beside story_compile_matcher, below; forward-declared so the #59
 * auto-routing tail can install it. The materialize/schema thunks are already
 * defined above. */
static void matcher_reground_thunk(void *ctx, world *w);

/* #59: the world disposes a compiler-installed matcher at world_free. */
static void matcher_free_thunk(void *ctx) { story_matcher_free((story_matcher *)ctx); }

static world *compile_impl(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, bool matched,
                           story_model **out, story_matcher **mret,
                           const char *kwhy_query, FILE *kwhy_out,
                           char **iface_out)
{
    parser *p = calloc(1, sizeof *p);
    p->kwhy_query = kwhy_query;        /* #125 build-time why hook */
    p->kwhy_out = kwhy_out;
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
        case TK_IDENT:
            if (ident_is(p->cur, "fact"))           parse_fact(p);  /* #124 */
            else if (ident_is(p->cur, "emit"))      parse_emit(p);  /* #11 */
            else if (ident_is(p->cur, "exclusive")) parse_exclusive(p); /* #159 */
            else                                    parse_sup(p);
            break;
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
            check_conflictable_pairs(p);          /* #98: contested-step + null
                                                   * conflicts, at compile time */
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
                /* #59: the cardinality ceiling is a ROUTING threshold, not a
                 * stop. A rule whose cross product blows the cap but whose every
                 * variable is generator-bound is exactly the shape the tick-time
                 * matcher exists for — its instances are Nᵏ only on paper, while
                 * the live extension is sparse. Route it there instead of
                 * refusing the program; an over-cap rule the matcher CANNOT take
                 * still errors in ground_rule, and says why.
                 *
                 * The matcher is created on demand, so a story that never trips
                 * the cap pays nothing and behaves exactly as before. */
                for (int i = 0; i < p->nrules; i++) {
                    ast_rule *r = &p->rules[i];
                    if (p->ground_matched && rule_matchable(p, r)) {
                        ground_rule_matched(p, r);
                    } else if (rule_over_cap(p, r) && rule_matchable(p, r)) {
                        if (!m) {
                            m = calloc(1, sizeof *m);
                            m->syms = syms;
                            m->w = p->w;
                        }
                        matcher_capture(m, p, r, rule_island(p, r));
                        warn(p, r->line, r->col,
                             "rule '%s' exceeds %d eager instances; grounding it "
                             "at tick time against the live facts instead "
                             "(§8.1 routing) — its cost now tracks matches, not "
                             "the sort cross product",
                             r->label, MAX_INSTANCES);
                    } else {
                        ground_rule(p, r);
                    }
                }
                if (m) {                       /* the matched region starts here */
                    world_matched_checkpoint(p->w);
                    matcher_capture_schema(m, p);
                }
            }
            for (int i = 0; i < p->nactions; i++) ground_action(p, &p->actions[i]);
            for (int i = 0; i < p->nsups; i++)    ground_sup(p, &p->sups[i]);
            ground_exclusives(p);                 /* #159 exclusivity groups */
            register_value_reads(p);              /* #82 values, readable */
            {   /* #109 (§5.2): an attacked support cycle is a located error —
                 * the Datalog completion is only sound where defeat cannot
                 * reach, so the compiler refuses the overlap outright */
                char cerr[512]; const char *cprov = NULL;
                if (world_attacked_cycle(p->w, cerr, sizeof cerr, &cprov)) {
                    int cline = 0;
                    if (cprov) {
                        const char *cl = strrchr(cprov, ':');
                        if (cl) cline = atoi(cl + 1);
                    }
                    serr(p, cline ? cline : 1, 1, "%s", cerr);
                }
            }
            check_orphans(p);
            /* Skip lanes in tick-time matcher mode: matchable rules aren't ground
             * into the world (only captured), so a judgment lane family would
             * shadow the re-materialized layer with stale, un-re-ground results.
             * lane↔matcher routing is a later slice (#28 router). */
            /* `m` covers the #59 auto-routed case too: either way the matched
             * rules are captured, not in the world, so a judgment lane family
             * would shadow the re-materialized layer with stale results. */
            if (p->nerrors == 0 && !mret && !m) build_lane_families(p);   /* the DoD thesis, 2a */
        }
    }

    if (p->nerrors == 0) {
        result = p->w;
        if (mret) {
            *mret = m;                     /* caller asked for it, caller frees it */
        } else if (m) {
            /* #59: the compiler installed this matcher on its own. The caller
             * asked for a plain world and never learns it exists, so the world
             * takes the hooks AND the lifetime. */
            world_set_reground_fn(p->w, matcher_reground_thunk, m);
            world_set_materialize_fn(p->w, matcher_materialize_thunk, m);
            world_set_schema_fn(p->w, matcher_schema_thunk, m);
            world_own_reground_ctx(p->w, matcher_free_thunk);
        }
    }
    else { world_free(p->w); story_matcher_free(m); if (mret) *mret = NULL; }

    /* Harvest the span model before the parser tables are torn down —
     * best-effort, so navigation works even on a file that failed to compile. */
    if (out) *out = harvest_model(p);
    /* The §6.3 interface artifact, on the same terms: the vocabulary a client
     * checks against, harvested from the tables the checks themselves used. */
    if (iface_out) *iface_out = p->nerrors == 0 ? harvest_iface(p) : NULL;

    if (p->kres) dl_result_free(p->kres);              /* #125 kind stratum */
    if (p->kth)  dl_theory_free(p->kth);
    free(p->katoms);
    free(p->katom_rule);
    for (int i = 0; i < p->nrules; i++) free(p->rules[i].insts);
    free(p->rules);
    free(p->actions);
    free(p->binders);
    free(p->exprs);
    free(p->inits);
    free(p->hreads);
    free(p->ents);
    free(p->ent_of);
    free(p->ent_pos);
    free(p->vmark_of);
    free(p->vdefd_of);
    for (int s = 0; s < p->nsorts; s++) free(p->domain_ents[s]);
    if (p->fidx) factindex_free(p->fidx);
    free(p);
    return result;
}

world *story_compile(const char *src, const char *srcname, intern *syms,
                     story_diags *diags)
{
    return compile_impl(src, srcname, syms, diags, false, NULL, NULL, NULL, NULL,
                        NULL);
}

world *story_compile_kinds_why(const char *src, const char *srcname,
                               intern *syms, story_diags *diags,
                               const char *query, FILE *out)
{
    return compile_impl(src, srcname, syms, diags, false, NULL, NULL,
                        query, out, NULL);
}

world *story_compile_model(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, story_model **out)
{
    return compile_impl(src, srcname, syms, diags, false, out, NULL, NULL, NULL,
                        NULL);
}

/* Same grammar and world, but ground rules in the join-matcher kernel via the
 * fact-store extension index (§5.2 item 4, #28) where eligible. Verdicts and
 * why-traces are identical to story_compile (pinned by test_matcher). */
world *story_compile_iface(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, char **iface_out)
{
    if (iface_out) *iface_out = NULL;
    return compile_impl(src, srcname, syms, diags, false, NULL, NULL, NULL, NULL,
                        iface_out);
}

world *story_compile_matched(const char *src, const char *srcname, intern *syms,
                             story_diags *diags)
{
    return compile_impl(src, srcname, syms, diags, true, NULL, NULL, NULL, NULL,
                        NULL);
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
    world *w = compile_impl(src, srcname, syms, diags, true, NULL, &m, NULL, NULL,
                            NULL);
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
