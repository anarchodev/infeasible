#ifndef INF_CORE_ARENA_H
#define INF_CORE_ARENA_H

#include <stddef.h>

/* Bump allocator with chained blocks. Individual frees are not supported;
 * release the whole arena at once. */

typedef struct arena_block arena_block;

typedef struct arena {
    arena_block *head;
} arena;

void  arena_init(arena *a);
void *arena_alloc(arena *a, size_t n);          /* 16-byte aligned, zeroed */
char *arena_strdup(arena *a, const char *s);
void  arena_release(arena *a);

#endif
