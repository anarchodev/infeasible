#include "core/arena.h"

#include <stdlib.h>
#include <string.h>

struct arena_block {
    arena_block *next;
    size_t cap;
    size_t used;
    /* data follows */
};

#define BLOCK_DATA(b) ((unsigned char *)(b) + sizeof(arena_block))
#define DEFAULT_BLOCK (64 * 1024)

void arena_init(arena *a)
{
    a->head = NULL;
}

static arena_block *new_block(size_t min)
{
    size_t cap = DEFAULT_BLOCK;
    if (min > cap)
        cap = min;
    arena_block *b = malloc(sizeof(arena_block) + cap);
    b->next = NULL;
    b->cap = cap;
    b->used = 0;
    return b;
}

void *arena_alloc(arena *a, size_t n)
{
    n = (n + 15u) & ~(size_t)15u;
    arena_block *b = a->head;
    if (!b || b->used + n > b->cap) {
        arena_block *nb = new_block(n);
        nb->next = a->head;
        a->head = nb;
        b = nb;
    }
    void *p = BLOCK_DATA(b) + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strdup(arena *a, const char *s)
{
    size_t len = strlen(s) + 1;
    char *p = arena_alloc(a, len);
    memcpy(p, s, len);
    return p;
}

void arena_release(arena *a)
{
    arena_block *b = a->head;
    while (b) {
        arena_block *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
