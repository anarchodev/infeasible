#ifndef INFEASIBLE_CORE_GROW_H
#define INFEASIBLE_CORE_GROW_H

#include <stdlib.h>

/* GROW(arr, n, cap): ensure the dynamic array `arr` has room for one more
 * element beyond its current count `n`, doubling capacity `cap` (from 16) when
 * full. Type-generic — element size comes from `sizeof *(arr)` — and writes
 * back through `arr` and `cap`, which is why this is a macro rather than a
 * function. Callers must include <stdlib.h> transitively (this header does). */
#define GROW(arr, n, cap) \
    do { \
        if ((n) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 16; \
            (arr) = realloc((arr), (size_t)(cap) * sizeof *(arr)); \
        } \
    } while (0)

#endif /* INFEASIBLE_CORE_GROW_H */
