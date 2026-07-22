#ifndef INF_LANG_STORY_H
#define INF_LANG_STORY_H

#include <stddef.h>

#include "core/intern.h"
#include "state/world.h"

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
 * Still out: MV judgment-rule heads / negative MV effects (they need the §5.7
 * family reification), each rejected with a located error.
 * The variable-free fragment is the degenerate arity-0 case: cellar_prop.story
 * still compiles to the same world.
 *
 * Grammar handled by this slice:
 *
 *   file    := decl*
 *   decl    := sort | entity | state | init | rule | action | sup
 *   sort    := 'sort'   ( IDENT | '(' IDENT (','? IDENT)* ')' )
 *   entity  := 'entity' ( ebind | '(' ebind* ')' )
 *   ebind   := IDENT (',' IDENT)* ':' IDENT            -- names : sort
 *   state   := 'state'  ( fdecl | '(' fdecl* ')' )
 *   fdecl   := IDENT [ '(' IDENT (',' IDENT)* ')' ] [ ftype ]
 *   ftype   := ':' 'int' [ 'in' INT '..' INT ]         -- numeric + clamp range
 *            | ':' '{' IDENT (',' IDENT)* '}'           -- multi-valued domain
 *   init    := 'init'   ( iatom | '(' iatom* ')' )      -- ground
 *   iatom   := atom | IDENT '=' (IDENT | INT)           -- MV / numeric init
 *   rule    := 'rule' IDENT [ params ] ':' conj                 -- judgment
 *                ( OP atom [ 'unless' conj ]
 *                | 'causes' conj )                              -- ramification
 *   action  := 'action' IDENT [ params ] ':' [ 'requires' conj ] 'causes' conj
 *   params  := '(' vbind (',' vbind)* ')'
 *   vbind   := IDENT ':' IDENT                          -- var : sort
 *   OP      := '->' | '=>' | '~>'
 *   sup     := IDENT '>' IDENT                          -- label > label
 *   conj    := eatom ( '&' eatom )*
 *   eatom   := atom [ cmp INT | '=' IDENT | numop expr ] -- guard / MV / effect
 *   cmp     := '<=' | '<' | '>=' | '>' | '='
 *   numop   := ':=' | '+=' | '-='                        -- numeric effect (§5.8)
 *   expr    := term (('+'|'-') term)*                    -- effect RHS, int-only
 *   term    := factor ('*' factor)*
 *   factor  := '-' factor | INT | ('min'|'max') '(' expr ',' expr ')'
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

#endif
