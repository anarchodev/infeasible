/* Golden test for generator-capable providers (#254).
 *
 * The point and fill callbacks both TEST a tuple the engine already formed, so
 * a provider can never bind a variable — which is why a rule anchored only on
 * `near(X, Y)` grounds the sort cross product and prunes afterwards. The
 * generator form inverts the question: the host walks its own index once and
 * hands back the b for which `pred(a, ·)` holds.
 *
 * Two properties carry the whole thing.
 *
 * I4: the run's order is the HOST's — its buckets, its rebuild, its iteration
 * — and grounding order sets roll-site indices and lane assignment. So the
 * engine canonicalises before anyone sees a run, and the hosts below
 * deliberately misbehave: one enumerates backwards, one reports pairs twice.
 * Neither may be observable.
 *
 * Agreement: enumerating changes WHEN the host is asked, never what it
 * answers. world_providers_gen_check is the differential that pins it, the
 * same posture world_providers_check takes for the batched form and
 * world_lanes_check takes for layouts. */

#include "state/world.h"
#include "core/intern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

enum { N = 12 };
static intern  *SY;
static uint32_t ENT[N];          /* e0..e11 */
static uint32_t NEAR, LOS;

/* the truth both forms must agree on: |i - j| <= 2 and i != j */
static int idx_of(uint32_t e)
{
    for (int i = 0; i < N; i++) if (ENT[i] == e) return i;
    return -1;
}
static bool truth(uint32_t a, uint32_t b)
{
    int i = idx_of(a), j = idx_of(b);
    if (i < 0 || j < 0 || i == j) return false;
    int d = i - j;
    return d <= 2 && d >= -2;
}

static bool point_fn(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    (void)ctx; (void)pred;
    return nargs == 2 && truth(args[0], args[1]);
}

/* A host that enumerates BACKWARDS and reports the far neighbours twice — both
 * legal (its index is its own business) and both invisible after
 * canonicalisation. */
static int gen_misbehaving(void *ctx, uint32_t pred, uint32_t a,
                           uint32_t *out, int cap)
{
    (void)ctx; (void)pred;
    uint32_t buf[16]; int n = 0;
    for (int j = N - 1; j >= 0; j--)
        if (truth(a, ENT[j])) {
            buf[n++] = ENT[j];
            int d = idx_of(a) - j;
            if (d == 2 || d == -2) buf[n++] = ENT[j];      /* duplicate */
        }
    for (int k = 0; k < n && k < cap; k++) out[k] = buf[k];
    return n;                                    /* the TOTAL, may exceed cap */
}

static int gen_empty(void *ctx, uint32_t pred, uint32_t a, uint32_t *out, int cap)
{ (void)ctx; (void)pred; (void)a; (void)out; (void)cap; return 0; }

int main(void)
{
    SY = intern_new();
    world *w = world_new(SY);
    for (int i = 0; i < N; i++) {
        char b[16]; snprintf(b, sizeof b, "e%d", i);
        ENT[i] = intern_id(SY, b);
    }
    NEAR = intern_id(SY, "near");
    LOS  = intern_id(SY, "los");
    world_set_provider_fn(w, point_fn, NULL);

    /* the capability is DECLARED by registration, never guessed */
    CHECK(!world_provider_generates(w, NEAR));
    {
        uint32_t out[16];
        CHECK(world_provider_gen(w, NEAR, ENT[5], out, 16) == 0);
    }
    printf("  unregistered: generates=false, run empty\n");

    world_set_provider_gen_fn(w, NEAR, gen_misbehaving, NULL);
    CHECK(world_provider_generates(w, NEAR));
    /* per-PREDICATE: a host that can enumerate adjacency may have only a point
     * query for line of sight, and registering one must not claim the other */
    CHECK(!world_provider_generates(w, LOS));
    printf("  per-predicate: near generates, los does not\n");

    /* canonicalisation: ascending, deduplicated, whatever the host did */
    {
        uint32_t out[16];
        int n = world_provider_gen(w, NEAR, ENT[5], out, 16);
        CHECK(n == 4);                                  /* e3 e4 e6 e7 */
        CHECK(out[0] == ENT[3] && out[1] == ENT[4] &&
              out[2] == ENT[6] && out[3] == ENT[7]);
        printf("  canonical: backwards + duplicated host run -> %d ascending\n", n);
    }

    /* an edge entity has a short run; nothing wraps */
    {
        uint32_t out[16];
        int n = world_provider_gen(w, NEAR, ENT[0], out, 16);
        CHECK(n == 2 && out[0] == ENT[1] && out[1] == ENT[2]);
        printf("  edge lane: %d neighbours, no wrap\n", n);
    }

    /* cap short of the run: fills what fits, returns the TRUE total, so a
     * caller can size a buffer by asking with cap 0 first */
    {
        uint32_t out[2] = { 0, 0 };
        int n = world_provider_gen(w, NEAR, ENT[5], out, 2);
        CHECK(n == 4);
        CHECK(out[0] == ENT[3] && out[1] == ENT[4]);
        CHECK(world_provider_gen(w, NEAR, ENT[5], NULL, 0) == 4);
        printf("  cap 2 of 4: returns 4, fills 2; cap 0 sizes\n");
    }

    /* determinism: the same question twice is the same answer, byte for byte */
    {
        uint32_t a[16], b[16];
        int n = world_provider_gen(w, NEAR, ENT[7], a, 16);
        int m = world_provider_gen(w, NEAR, ENT[7], b, 16);
        CHECK(n == m && memcmp(a, b, (size_t)n * sizeof *a) == 0);
        printf("  determinism: repeated run identical\n");
    }

    /* THE differential: enumerating changes when the host is asked, not what it
     * answers. Deliberately fed in a scrambled entity order, so a correct
     * generator cannot be made to look wrong by the caller's enumeration. */
    {
        bool ok = false;
        int checks = world_providers_gen_check(w, NEAR, ENT, N, &ok);
        CHECK(ok && checks == N * N);
        uint32_t shuffled[N];
        for (int i = 0; i < N; i++) shuffled[i] = ENT[(i * 7 + 5) % N];
        ok = false;
        CHECK(world_providers_gen_check(w, NEAR, shuffled, N, &ok) == N * N && ok);
        printf("  differential: %d comparisons agree, in-order and scrambled\n", checks);
    }

    /* a generator that disagrees with the point form must be CAUGHT — the
     * oracle is worthless if it only ever passes */
    {
        world_set_provider_gen_fn(w, NEAR, gen_empty, NULL);
        bool ok = true;
        world_providers_gen_check(w, NEAR, ENT, N, &ok);
        CHECK(!ok);
        world_set_provider_gen_fn(w, NEAR, gen_misbehaving, NULL);
        printf("  differential catches a generator that drops pairs\n");
    }

    /* an empty run is "nothing holds here", not "no generator" — the caller
     * distinguishes them with world_provider_generates, which is why an
     * unregistered predicate must not be readable as an empty answer */
    {
        world_set_provider_gen_fn(w, LOS, gen_empty, NULL);
        uint32_t out[4];
        CHECK(world_provider_generates(w, LOS));
        CHECK(world_provider_gen(w, LOS, ENT[3], out, 4) == 0);
        printf("  empty run != absent generator\n");
    }

    /* with no point callback there is nothing to compare against, and the
     * oracle says so rather than passing vacuously */
    {
        world *bare = world_new(SY);
        world_set_provider_gen_fn(bare, NEAR, gen_misbehaving, NULL);
        bool ok = false;
        CHECK(world_providers_gen_check(bare, NEAR, ENT, N, &ok) == 0);
        world_free(bare);
        printf("  no point callback: 0 comparisons\n");
    }

    world_free(w);
    intern_free(SY);
    printf("test_provgen: all passed\n");
    return 0;
}
