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
 * Returns a freshly built world on success, or NULL with a `line:col: message`
 * written into `err` (fail-fast; panic-mode recovery is a follow-up). The
 * returned world borrows `syms`, which must outlive it. */

world *story_compile(const char *src, intern *syms, char *err, size_t errsz);

#endif
