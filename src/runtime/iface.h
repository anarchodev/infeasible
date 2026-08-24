#ifndef INF_RUNTIME_IFACE_H
#define INF_RUNTIME_IFACE_H

#include <stdbool.h>
#include <stddef.h>

/* The §6.3 interface artifact, read back (DESIGN.md §6.3, §12).
 *
 * `story_compile_iface` publishes the declared vocabulary — sorts and their
 * entities, enums and their values, judgments and their argument sorts, values,
 * actions and their parameters — as JSON. A client that wants to draw a world
 * it has never heard of needs exactly that: to enumerate a judgment you must
 * know which domains to cross, and to read a value you must know it exists.
 *
 * So this is the artifact as a lookup table, and it is what makes a renderer
 * game-blind: `src/runtime/scene.c` knows a blessed VOCABULARY (panel, caption,
 * shows, …) and asks the artifact what to cross it over. Point it at another
 * story and it draws another game with no edit.
 *
 * The spelling of a ground atom is the artifact's own (`pred(a,b)`, `pred` for
 * a nullary), which is the whole reason the artifact exists: a client that
 * spells an atom its own way names something the engine does not have, and the
 * failure is silent. */

typedef struct iface iface;

/* Parse the artifact JSON. Returns NULL with `err` filled on malformed input
 * or a version this build does not know. */
iface *iface_parse(const char *json, char *err, size_t errsz);
void   iface_free(iface *f);

/* A DOMAIN is a sort (holding entities) or an enum (holding values) — the two
 * things an argument position can range over. `iface_domain` finds either by
 * name; a declared cover (`sort thing union actor, item`, #231) is a domain
 * too, listing its members' entities under its own name. */
int         iface_domain_size(const iface *f, const char *name);
const char *iface_domain_item(const iface *f, const char *name, int i);
bool        iface_is_union(const iface *f, const char *name);

/* An entity's BASE sort — never a cover that merely admits it. A client that
 * answered "drawable" for a fighter could not tell a card from a person, and
 * the menu filter below depends on being able to. NULL for a value or an
 * unknown name. */
const char *iface_sort_of(const iface *f, const char *entity);

/* Position of a value in its enum, or -1. Enum ORDER is meaning: a `sprite`
 * member's position is its atlas index, and declaration order is already the
 * engine's tie-break (I4), so two clients cannot disagree about it. */
int iface_enum_index(const iface *f, const char *enum_name, const char *value);

/* A judgment's argument sorts — what to cross to enumerate it. Returns the
 * arity, or -1 if this story has no such judgment. */
int         iface_judgment_arity(const iface *f, const char *name);
const char *iface_judgment_arg(const iface *f, const char *name, int i);
/* Is this predicate a judgment at all? (A blocker that is one has a `why` to
 * print; a base fact is merely absent.) */
bool iface_is_judgment(const iface *f, const char *pred);

/* Does the story define this derived value? */
bool iface_has_value(const iface *f, const char *name);

const char *iface_story(const iface *f);

#endif
