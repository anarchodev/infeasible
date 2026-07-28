#ifndef INF_LANG_STORY_MODEL_H
#define INF_LANG_STORY_MODEL_H

/* The .story symbol / occurrence model — a first-class compiler output, the
 * single source of truth for anything that needs source spans over the
 * vocabulary: the language server's navigation (§6.1 item 7), hover, the §6.3
 * interface artifact (which exports atoms/heads/actions), and the §6.1
 * dependency-cone queries. It is produced by the SAME parse that emits
 * diagnostics — never a second walk of the source (that would drift).
 *
 * Every AST node already carries `line, col`; the model harvests them from the
 * parser's own tables (sorts, entities, fluents, providers, functions, rules,
 * actions, sups, inits) into a flat, tier-neutral form. `lang` owns it and it
 * depends on nothing above `lang` — the LSP is one consumer, not privileged.
 *
 * Positions are 1-based (line, col) as the lexer reports them, with `len` the
 * identifier width in columns; consumers remap to their own convention (the
 * LSP subtracts one and, eventually, recodes columns as UTF-16 units).
 *
 * Best-effort on broken input: it is harvested from whatever the parser
 * accumulated, so an editor still navigates a file that fails to compile. */

typedef enum {
    STORY_SYM_SORT,      /* `sort s`                          */
    STORY_SYM_DOMAIN,    /* `domain d` — an opaque value sort */
    STORY_SYM_ENTITY,    /* `entity e : s`                    */
    STORY_SYM_FLUENT,    /* `state f(...)` — base state       */
    STORY_SYM_PROVIDER,  /* `provider r(...)` — host relation */
    STORY_SYM_FUNCTION,  /* `function g(...) : t` — host fn   */
    STORY_SYM_ENUM,      /* `enum e { ... }`                  */
    STORY_SYM_ACTION,    /* `action a`                        */
    STORY_SYM_RULE       /* a labelled `rule L: ...`          */
} story_sym_kind;

/* Where an identifier occurrence sits, so navigation can distinguish "declared
 * here" / "concluded here" / "used here" without re-deriving grammar. */
typedef enum {
    STORY_OCC_DECL,    /* the declaring occurrence of a name        */
    STORY_OCC_HEAD,    /* an atom concluded by a rule (a rule head) */
    STORY_OCC_BODY,    /* used as a condition/guard/requires/init   */
    STORY_OCC_EFFECT,  /* written by an action or ramification      */
    STORY_OCC_ARG      /* an argument (entity/var/value) position   */
} story_occ_role;

typedef struct {
    const char    *name;   /* NUL-terminated, model-owned */
    story_sym_kind kind;
    int            line, col, len;
    const char    *detail; /* a compact signature the concept word can't carry
                            * in the closed LSP SymbolKind enum — e.g.
                            * "provider(actor, actor)", "fluent : int in 0..40",
                            * "function(cell, int) : cell". Model-owned, never
                            * NULL (empty string when there's nothing to add). */
} story_symbol;

typedef struct {
    const char    *name;   /* NUL-terminated, model-owned */
    story_occ_role role;
    int            line, col, len;
    bool           neg;    /* the `~` on this atom — a HEAD with neg=true is an
                            * attacker of its pred, not a concluder of it */
    int            rule;   /* index into story_model_rules of the owning rule,
                            * or -1 (decls, args, actions, inits, sup operands) */
} story_occ;

/* One judgment rule, indexed by story_occ.rule. `label` is the author's rule
 * label, or "" for an anonymous rule (identify it by its span instead). This
 * is what the dependency/attacker cone (§6.1 item 7, §9) is built over: the
 * head/body occurrences of a rule share its index. */
typedef struct {
    const char *label;     /* NUL-terminated, model-owned; "" if anonymous */
    int         line, col;
} story_rule;

typedef struct story_model story_model;

const story_symbol *story_model_symbols(const story_model *m, int *n);
const story_occ    *story_model_occs(const story_model *m, int *n);
const story_rule   *story_model_rules(const story_model *m, int *n);
void                story_model_free(story_model *m);

#endif
