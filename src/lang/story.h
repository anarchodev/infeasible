#ifndef INF_LANG_STORY_H
#define INF_LANG_STORY_H

#include <stddef.h>

#include "core/intern.h"
#include "state/world.h"
#include "lang/story_model.h"

/* Front half of the .story compiler (DESIGN.md §11 M1). Two passes over an
 * arena AST (§10): recursive-descent parse, then a semantic + build-time
 * grounding pass that emits ground rules into the `world_*` API.
 *
 * Grounder over the propositional first slice: typed variables (`X : actor`)
 * and predicate atoms (`holding(actor,item)`), grounded up front over declared
 * finite sorts (DESIGN.md §5.2 item 2 — typed vars bound every domain; the
 * tick-time join matcher is M3, §11). Multi-valued fluents (`: { … }`, §5.7)
 * and numeric fluents (`: int [ in lo..hi ]`, comparison guards, and the write
 * side — `:=`/`+=`/`-=` effects over an expression VM, §5.8) are handled too.
 * Ramifications are a `rule … causes …` (a bare `causes`, no arrow): a step
 * rule with no action trigger, firing in any step whose state matches (§5.4).
 * Its body is current-state by default; a postfix `'` (`~alive(X)'`) reads the
 * next state, so an indirect effect cascades in the same step. The prime is
 * legal only in a ramification body and only on a boolean or multi-valued
 * fluent read — a primed numeric guard or judgment (`hp' <= 0`, `dead'`) is the
 * §5.8 stratification case, rejected with a located error.
 * Set-quantified effect binders (§13): a `causes` body may contain, alongside
 * plain effects, `for each T: sort [where <guard>]: <effect> [when <cond>]` (a
 * single effect, or a `{ … }` block of comma-separated `effect when <cond>`
 * items). The bound var(s) extend the enclosing action's binding at ground
 * time, so one guarded step-rule is emitted per (cast × target × item), sharing
 * the action's trigger; the `where`/`when` conjuncts lower to step conditions,
 * resolving the affected set at tick time — an AoE/multi-target effect over a
 * host-marked or state-guarded set (Fireball, Faerie Fire). `limit` (bounded
 * quantification) and provider-answered guards are a later slice. `enum` names
 * a value domain reusable as a fluent type (`f(a) : school`), erasing to the
 * same multi-valued machinery as an inline `: { … }`.
 * Still out: MV judgment-rule heads / negative MV effects (they need the §5.7
 * family reification), each rejected with a located error.
 * The variable-free fragment is the degenerate arity-0 case: cellar_prop.story
 * still compiles to the same world.
 *
 * Grammar handled by this slice:
 *
 *   file    := decl*
 *   decl    := sort | enum | entity | state | function | value | init | rule | action | sup
 *   sort    := 'sort'   ( IDENT | '(' IDENT (','? IDENT)* ')' )
 *   enum    := 'enum' IDENT '{' IDENT (',' IDENT)* '}'  -- named value domain (§13);
 *                                          -- also legal as an argument sort and a
 *                                          -- rule-var type (#96): values ground like
 *                                          -- a sort's entities but are not entities
 *   entity  := 'entity' ( ebind | '(' ebind* ')' )
 *   ebind   := IDENT (',' IDENT)* ':' IDENT            -- names : sort
 *   state   := 'state'  ( fdecl | '(' fdecl* ')' )
 *   fdecl   := IDENT [ '(' IDENT (',' IDENT)* ')' ] [ ftype ]
 *   ftype   := ':' 'int' [ 'in' INT '..' INT ] [ 'merge' ('min'|'max') ]
 *                                          -- numeric + clamp range; `merge` (#85):
 *                                          -- the ASSIGN-class algebra — firing `:=`s
 *                                          -- take the extreme instead of contesting
 *                                          -- (deltas still sum on top, clamp outermost)
 *            | ':' '{' IDENT (',' IDENT)* '}'           -- inline multi-valued domain
 *            | ':' IDENT                                -- a declared `enum` domain
 *   function:= 'function' IDENT '(' vtype (',' vtype)* ')' ':' vtype -- host fn (§5.6)
 *   value   := 'value' ( vdecl | '(' vdecl* ')' )       -- engine-derived value (#82)
 *   vdecl   := IDENT [ '(' IDENT* ')' ] ':' 'int'        -- defined by a rule, inlined
 *                                                        -- at reads; roll sites key on
 *                                                        -- the definition, so every
 *                                                        -- reader shares the draw
 *   vtype   := IDENT | 'int'                            -- a sort/domain, or int
 *   init    := 'init'   ( iatom | '(' iatom* ')' )      -- ground
 *   iatom   := atom | IDENT '=' (IDENT | INT)           -- MV / numeric init
 *   rule    := 'rule' IDENT [ params ] ':' conj                 -- judgment
 *                ( OP atom [ 'unless' conj ]
 *                | 'causes' effects )                           -- ramification
 *            | 'rule' IDENT [ params ] ':' '=>' IDENT '(' args ')' '=' expr
 *                                          -- value definition (#82): body must be
 *                                          -- empty in this slice (one total
 *                                          -- definition per value; layering later)
 *   action  := 'action' IDENT [ params ] ':' [ 'requires' conj ] 'causes' effects
 *   effects := effitem ( '&' effitem )*                         -- effect body
 *   effitem := eatom | binder                                   -- plain or set-quantified
 *   binder  := 'for' 'each' vbind (',' vbind)* [ 'where' conj ] ':'  -- §13
 *                ( bindeff | '{' bindeff (',' bindeff)* '}' )
 *   bindeff := eatom [ 'when' conj ]                            -- `when` in blocks only
 *   params  := '(' vbind (',' vbind)* ')'
 *   vbind   := IDENT ':' IDENT                          -- var : sort
 *   OP      := '->' | '=>' | '~>'
 *   sup     := IDENT '>' IDENT                          -- label > label
 *   conj    := eatom ( '&' eatom )*
 *   eatom   := atom [ cmp INT | '=' IDENT | numop expr ] -- guard / MV / effect
 *            | IDENT ['not'] 'in' '{' IDENT (',' IDENT)* '}'  -- membership (#95):
 *                                          -- a static grounding filter over a var's
 *                                          -- finite domain; never in the fixpoint
 *   cmp     := '<=' | '<' | '>=' | '>' | '='
 *   numop   := ':=' | '+=' | '-='                        -- numeric effect (§5.8);
 *                                          -- a `+=`/`-=` RHS may end `'as' IDENT`
 *                                          -- (#83): the delta accumulates in that
 *                                          -- enum value's damage-type bucket and the
 *                                          -- commit consults resistant/vulnerable/
 *                                          -- immune(<subject>, <type>) judgments
 *                                          -- after summation, before the clamp (#84)
 *   expr    := term (('+'|'-') term)*                    -- effect RHS, int-only
 *   term    := factor (('*'|'/') factor)*   -- '/' floors (rounds toward -inf, §5.8)
 *   factor  := '-' factor | INT
 *            | ('min'|'max'|'divup') '(' expr ',' expr ')'  -- divup = ceiling div
 *            | IDENT '(' expr (',' expr)* ')'           -- fn-provider call (§5.6)
 *            | IDENT [ '(' arg (',' arg)* ')' ] | '(' expr ')'
 *   atom    := [ '~' ] IDENT [ '(' arg (',' arg)* ')' ] [ "'" ]
 *   arg     := IDENT                                    -- a var or an entity
 *   -- postfix "'" = next-state; ramification bodies only (§5.4)
 *
 * Atoms are interned into `syms`; ground atoms intern as "pred(e1,e2)" (the
 * bare name at arity 0), so a host querying the equivalent ground atom sees
 * the same id. Conclusions (rule heads) need no declaration. The returned
 * world borrows `syms`, which must outlive it.
 *
 * Diagnostics (errors and warnings) collect into a caller-supplied sink; the
 * parser recovers at declaration boundaries (panic mode, §10) so one bad
 * declaration does not mask the rest of the file. Returns the compiled world
 * when no error-severity diagnostic was produced, or NULL if any error was —
 * warnings (e.g. orphan/typo detection, §6.1) never fail the compile.
 *
 * `items`/`cap` are caller-owned; `count` is the total number of diagnostics
 * produced (may exceed `cap`, in which case only the first `cap` are stored)
 * and `nerrors` how many of them were errors. Pass NULL for `diags` to
 * compile without collecting messages (the return value still reflects
 * success). Any prior `count`/`nerrors` are reset on entry. */

typedef enum { STORY_ERROR, STORY_WARNING } story_severity;

typedef struct {
    story_severity sev;
    int            line, col;
    char           msg[192];
} story_diag;

typedef struct {
    story_diag *items;
    int         cap;
    int         count;
    int         nerrors;
} story_diags;

/* `srcname` is the source file name (e.g. "cellar.story"), used only to render
 * provenance in why-traces (§6.3) as `srcname:line`; NULL falls back to
 * "<story>". It is borrowed — the caller must keep it alive as long as the
 * returned world (provenance strings copy from it at compile time). */
world *story_compile(const char *src, const char *srcname, intern *syms,
                     story_diags *diags);

/* Same result as story_compile, but grounds eligible rules via the semi-naïve
 * join matcher over the fact-store extension index (§5.2 item 4, #28) instead
 * of the eager sort cross product. Query verdicts and why-traces are identical
 * (test_matcher pins the equivalence); it grounds only body-satisfying rule
 * instances rather than every sort^k possibility. */
world *story_compile_matched(const char *src, const char *srcname, intern *syms,
                             story_diags *diags);

/* Same compile, but also harvest the symbol/occurrence model (story_model.h)
 * from the parser tables into `*out` — the source-span index navigation, hover,
 * and the interface artifact read (§6.1 item 7, §6.3). The world is returned as
 * usual (NULL on error); the model is populated best-effort even when the
 * compile fails, so an editor navigates a broken file. Pass NULL for `out` to
 * skip it (then this is exactly story_compile). The caller owns `*out` and
 * frees it with story_model_free; unlike the world it does not borrow `syms`
 * (names are copied), so it may outlive both `syms` and `src`. */
world *story_compile_model(const char *src, const char *srcname, intern *syms,
                           story_diags *diags, story_model **out);

/* Tick-time matcher (#28, the runtime half). Compiles like story_compile_matched
 * but RETAINS a compact plan of the matchable rules, so the matched judgment
 * layer can be re-grounded against the world's *current* facts each tick — the
 * matcher scans the live fact-store extension (world_fact_index), so cost tracks
 * actual matches, not the sort cross product, even after world_step changes the
 * state. The static (non-matchable) rules are ground once, as usual.
 *
 * `*out` receives the compiled world (NULL on compile error). The returned handle
 * owns only the plan; the world is owned by the caller (world_free it). Verdicts
 * and why-traces stay identical to the eager path (test_ticktime pins it across a
 * state change). Prototype: NOT yet routed through world_step — the host drives
 * re-grounding, mirroring how the lanes were validated before adoption. */
typedef struct story_matcher story_matcher;
story_matcher *story_compile_matcher(const char *src, const char *srcname,
                                     intern *syms, story_diags *diags, world **out);
/* Re-materialize the matched judgment layer against the world's current facts:
 * drop the previous matched rules, refresh the extension index, re-run the join. */
void   story_matcher_reground(story_matcher *m);
world *story_matcher_world(const story_matcher *m);
/* Fact-tuples the last reground walked (#46 selectivity instrumentation). */
long   story_matcher_last_probes(const story_matcher *m);
void   story_matcher_free(story_matcher *m);

#endif
