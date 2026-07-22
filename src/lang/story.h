#ifndef INF_LANG_STORY_H
#define INF_LANG_STORY_H

#include <stddef.h>

#include "core/intern.h"
#include "state/world.h"

/* Front half of the .story compiler (DESIGN.md §11 M1), first slice: the
 * variable-free / boolean-fluent fragment that maps directly onto the current
 * engine API. Enough to build the propositional cellar (examples/cellar_prop
 * .story) that tests/test_world.c builds by hand — no typed variables, no
 * multi-valued domains, no numeric guards, no ramifications yet (those need
 * the M1 grounder and the §5.7/5.8 fluent compilation).
 *
 * Grammar handled by this slice:
 *
 *   file    := decl*
 *   decl    := state | init | rule | action | sup
 *   state   := 'state' ( '(' IDENT* ')' | IDENT )      -- boolean fluents
 *   init    := 'init'  ( '(' IDENT* ')' | IDENT )      -- set those fluents true
 *   rule    := 'rule' IDENT ':' conj OP lit [ 'unless' conj ]
 *   OP      := '->' | '=>' | '~>'
 *   action  := 'action' IDENT ':' [ 'requires' conj ] 'causes' conj
 *   sup     := IDENT '>' IDENT                          -- label > label
 *   conj    := lit ( '&' lit )*
 *   lit     := [ '~' ] IDENT
 *
 * Atoms are interned into `syms`; conclusions (rule heads) need no declaration.
 * The returned world borrows `syms`, which must outlive it.
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

world *story_compile(const char *src, intern *syms, story_diags *diags);

#endif
