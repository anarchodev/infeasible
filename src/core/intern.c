#include "core/intern.h"
#include "core/arena.h"

#include <stdlib.h>
#include <string.h>

struct intern {
    arena a;
    char **names;       /* id -> name */
    uint32_t count;
    uint32_t names_cap;
    uint32_t *slots;    /* hash table storing id + 1, 0 = empty */
    uint32_t nslots;    /* power of two */
};

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

static void rehash(intern *t, uint32_t nslots)
{
    uint32_t *slots = calloc(nslots, sizeof *slots);
    for (uint32_t id = 0; id < t->count; id++) {
        uint32_t i = fnv1a(t->names[id]) & (nslots - 1);
        while (slots[i])
            i = (i + 1) & (nslots - 1);
        slots[i] = id + 1;
    }
    free(t->slots);
    t->slots = slots;
    t->nslots = nslots;
}

intern *intern_new(void)
{
    intern *t = calloc(1, sizeof *t);
    arena_init(&t->a);
    t->names_cap = 256;
    t->names = malloc(t->names_cap * sizeof *t->names);
    t->nslots = 512;
    t->slots = calloc(t->nslots, sizeof *t->slots);
    intern_id(t, "<none>");   /* reserve id 0 */
    return t;
}

uint32_t intern_id(intern *t, const char *name)
{
    uint32_t i = fnv1a(name) & (t->nslots - 1);
    while (t->slots[i]) {
        uint32_t id = t->slots[i] - 1;
        if (strcmp(t->names[id], name) == 0)
            return id;
        i = (i + 1) & (t->nslots - 1);
    }
    uint32_t id = t->count++;
    if (t->count > t->names_cap) {
        t->names_cap *= 2;
        t->names = realloc(t->names, t->names_cap * sizeof *t->names);
    }
    t->names[id] = arena_strdup(&t->a, name);
    t->slots[i] = id + 1;
    if (t->count * 10 > t->nslots * 7)
        rehash(t, t->nslots * 2);
    return id;
}

const char *intern_name(const intern *t, uint32_t id)
{
    return id < t->count ? t->names[id] : "<bad-atom>";
}

uint32_t intern_count(const intern *t)
{
    return t->count;
}

void intern_free(intern *t)
{
    arena_release(&t->a);
    free(t->names);
    free(t->slots);
    free(t);
}
