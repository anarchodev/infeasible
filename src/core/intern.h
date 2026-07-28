#ifndef INF_CORE_INTERN_H
#define INF_CORE_INTERN_H

#include <stdint.h>

/* String interning: name <-> dense uint32 atom id.
 * Id 0 is reserved for the empty sentinel "<none>". */

typedef struct intern intern;

#define INTERN_NONE 0u

intern     *intern_new(void);
uint32_t    intern_id(intern *t, const char *name);   /* creates if absent */
const char *intern_name(const intern *t, uint32_t id);
uint32_t    intern_count(const intern *t);
void        intern_free(intern *t);

/* Lookup WITHOUT creating: the id of the length-bounded name s[0..len), or
 * INTERN_NONE if it was never interned. Lets a caller probe substrings of a
 * composite name (a ground atom's pred / arg tokens, #92) without polluting
 * the table. */
uint32_t    intern_find_n(const intern *t, const char *s, uint32_t len);

#endif
