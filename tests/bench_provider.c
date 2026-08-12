/* bench_provider — the PROVIDER BOUNDARY, priced (#229).
 *
 * bench_slice reports the provider at ~90% of its tick, but bench_slice's
 * provider is a host phase that writes fact columns directly: it never crosses
 * `world_provider_fn` at all. That number is the cost of a spatial index, not
 * the cost of the interface. This file measures the interface — the one a
 * `.story` provider actually goes through — and separates the two:
 *
 *   1. WHAT THE BOUNDARY COSTS. A binary provider over N entities registers N^2
 *      ground atoms, and every solve asks the host about every one of them, one
 *      indirect call returning one bit. The "host floor" column is the same N^2
 *      answers computed in a tight loop with no engine in the middle, so the gap
 *      between it and the load is what the SHAPE costs rather than the work.
 *
 *   2. WHAT BATCHING BUYS. `world_set_provider_fill_fn` (#229) hands the host a
 *      RUN of atoms differing in one argument — the grounder's inner loop — so
 *      the host answers a row at a time and walks its index once per row.
 *
 * Two hosts, because the answer depends entirely on what a call costs:
 *
 *   flat   an O(1) distance test off a position array. The best case for the
 *          per-atom form, and the floor on what batching can buy: pure call
 *          overhead.
 *   index  a uniform-grid broadphase re-entered from the top per call — what a
 *          real `near`/`los` provider does (§5.6: the host owns the geometry).
 *          Per-atom this scans a neighbourhood per PAIR; batched, per SUBJECT.
 *
 * The third column of the story is the one neither host can fix: a rule reading
 * a provider is disqualified from the lane path (story.c's lane_atom_ok /
 * join_atom_ok), so all of this feeds an N=1 solve. Batching the boundary does
 * not change that, and the report says so.
 *
 * Deterministic (I4): positions are assigned by index, never randomly. Built -O2
 * regardless of build type; build Release for meaningful numbers. Not a test. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "logic/dl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* ---- the host: N actors on a grid, `near` answered two ways --------------- */

enum { GW = 64, R = 2 };            /* grid width; alert radius in cells */

typedef struct {
    int n;
    int *cx, *cy;                   /* positions, by actor index */
    int *by_atom; uint32_t natom;   /* entity atom id -> actor index, or -1 */
    int *bit_of;                    /* actor index -> its bit in the current run */
    int *cell_head, *nxt;           /* the uniform grid: intrusive lists */
    bool use_index;                 /* answer by scanning the grid, not by maths */
    long atom_calls, fill_calls, scans;
} host;

static int actor_of(const host *h, uint32_t ent)
{
    return ent < h->natom ? h->by_atom[ent] : -1;
}

static bool near_flat(const host *h, int i, int j)
{
    if (i < 0 || j < 0 || i == j) return false;
    int dx = h->cx[i] - h->cx[j], dy = h->cy[i] - h->cy[j];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= R && dy <= R;
}

/* The same relation, answered the way a broadphase answers it: walk the cells
 * around i and look for j. Deterministic; the scan is the point. */
static bool near_index(host *h, int i, int j)
{
    if (i < 0 || j < 0 || i == j) return false;
    for (int dy = -R; dy <= R; dy++) {
        int yy = h->cy[i] + dy;
        if (yy < 0 || yy >= GW) continue;
        for (int dx = -R; dx <= R; dx++) {
            int xx = h->cx[i] + dx;
            if (xx < 0 || xx >= GW) continue;
            for (int k = h->cell_head[yy * GW + xx]; k >= 0; k = h->nxt[k]) {
                h->scans++;
                if (k == j) return true;
            }
        }
    }
    return false;
}

static bool answer(host *h, int i, int j)
{
    return h->use_index ? near_index(h, i, j) : near_flat(h, i, j);
}

static bool prov(void *ctx, uint32_t pred, const uint32_t *args, int nargs)
{
    host *h = ctx;
    (void)pred;
    h->atom_calls++;
    return nargs == 2 && answer(h, actor_of(h, args[0]), actor_of(h, args[1]));
}

/* The batched form. The flat host just answers the run; the index host does what
 * the per-atom shape forbids — walk its neighbourhood ONCE for the subject and
 * set a bit per member found — which is the whole reason the run exists. */
static void fill(void *ctx, uint32_t pred, const uint32_t *args, int nargs,
                 int slot, const uint32_t *ents, int nents, uint64_t *out)
{
    host *h = ctx;
    (void)pred;
    h->fill_calls++;
    if (nargs != 2 || (slot != 0 && slot != 1)) return;
    int fixed = actor_of(h, args[slot == 0 ? 1 : 0]);

    if (h->use_index && slot == 1 && fixed >= 0) {
        /* ONE walk for the whole row: mark where each run member sits, scan the
         * subject's neighbourhood once, and set the bit of every member found.
         * This is the shape the per-atom callback cannot express. */
        for (int i = 0; i < nents; i++) {
            int a = actor_of(h, ents[i]);
            if (a >= 0) h->bit_of[a] = i;
        }
        for (int dy = -R; dy <= R; dy++) {
            int yy = h->cy[fixed] + dy;
            if (yy < 0 || yy >= GW) continue;
            for (int dx = -R; dx <= R; dx++) {
                int xx = h->cx[fixed] + dx;
                if (xx < 0 || xx >= GW) continue;
                for (int k = h->cell_head[yy * GW + xx]; k >= 0; k = h->nxt[k]) {
                    h->scans++;
                    int b = h->bit_of[k];
                    if (b >= 0 && k != fixed) out[b / 64] |= 1ull << (b % 64);
                }
            }
        }
        for (int i = 0; i < nents; i++) {
            int a = actor_of(h, ents[i]);
            if (a >= 0) h->bit_of[a] = -1;
        }
        return;
    }
    for (int i = 0; i < nents; i++) {
        int v = actor_of(h, ents[i]);
        if (answer(h, slot == 0 ? v : fixed, slot == 0 ? fixed : v))
            out[i / 64] |= 1ull << (i % 64);
    }
}

/* ---- the world ----------------------------------------------------------- */

/* `near` is a provider; the rule that reads it is the §5.6 shape. `awake` gives
 * the tick a base fact to move so the judgment family re-solves.
 *
 * The CONTROL declares the identical rule with `near` as a stored fluent — same
 * N^2 ground atoms, same N^2 ground rules, same N=1 solve, no host boundary
 * anywhere. Whatever the control costs is what this shape costs before the
 * provider is even reached, and it is the only way to read the columns above
 * honestly. */
static char *world_src(int n, bool as_provider, bool disqualify)
{
    size_t cap = 1u << 22;
    char *s = malloc(cap);
    size_t o = 0;
    o += (size_t)snprintf(s + o, cap - o, "sort actor\n%s\n"
                          "state ( awake(actor)  impossible(actor)%s )\nentity (",
                          as_provider ? "provider near(actor, actor)" : "",
                          as_provider ? "" : "  near(actor, actor)");
    for (int i = 0; i < n; i++)
        o += (size_t)snprintf(s + o, cap - o, "%sa%d", i ? ", " : " ", i);
    o += (size_t)snprintf(s + o, cap - o, " : actor )\n"
        "rule spot(X: actor, Y: actor): near(X, Y) & awake(Y) => threat(X, Y)\n");
    /* bench_join's disqualifier: `impossible` is declared and never set, so this
     * rule can never fire — but a second rule concluding the head takes the
     * family off the lane path, and it adds N ground rules against the shape's
     * N^2, so it prices the GATE rather than itself. */
    if (disqualify)
        o += (size_t)snprintf(s + o, cap - o,
            "rule never(X: actor): impossible(X) => threat(X, X)\n");
    return s;
}

/* The control's `near` extension, set to match what the flat host answers, so
 * the two worlds solve over the same facts and not just the same shape. */
static void fill_control(world *w, intern *sy, host *h, int n)
{
    char buf[48];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            if (near_flat(h, i, j)) {
                snprintf(buf, sizeof buf, "near(a%d,a%d)", i, j);
                world_set(w, intern_id(sy, buf), true);
            }
}

static void host_init(host *h, int n, intern *sy, bool use_index)
{
    memset(h, 0, sizeof *h);
    h->n = n;
    h->cx = malloc((size_t)n * sizeof *h->cx);
    h->cy = malloc((size_t)n * sizeof *h->cy);
    h->bit_of = malloc((size_t)n * sizeof *h->bit_of);
    h->nxt = malloc((size_t)n * sizeof *h->nxt);
    h->cell_head = malloc((size_t)GW * GW * sizeof *h->cell_head);
    h->use_index = use_index;
    h->natom = intern_count(sy);
    h->by_atom = malloc((size_t)h->natom * sizeof *h->by_atom);
    for (uint32_t a = 0; a < h->natom; a++) h->by_atom[a] = -1;

    char buf[32];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "a%d", i);
        uint32_t id = intern_id(sy, buf);
        if (id < h->natom) h->by_atom[id] = i;
        h->bit_of[i] = -1;
        /* a deterministic scatter: a few actors per cell at the sizes swept */
        h->cx[i] = (i * 7) % GW;
        h->cy[i] = (i * 13 / GW) % GW;
    }
    memset(h->cell_head, 0xff, (size_t)GW * GW * sizeof *h->cell_head);
    for (int i = 0; i < n; i++) {
        int c = h->cy[i] * GW + h->cx[i];
        h->nxt[i] = h->cell_head[c];
        h->cell_head[c] = i;
    }
}

static void host_free(host *h)
{
    free(h->cx); free(h->cy); free(h->by_atom); free(h->bit_of);
    free(h->nxt); free(h->cell_head);
}

/* One tick: a base fact moves, a conclusion is read — the whole provider table
 * is re-asked, because a provider is consulted fresh every solve (§5.6). */
static double tick_ms(world *w, intern *sy, int reps)
{
    uint32_t q = intern_id(sy, "threat(a0,a1)");
    uint32_t toggle = intern_id(sy, "awake(a1)");
    double t0 = now_ms();
    for (int k = 0; k < reps; k++) {
        world_set(w, toggle, k % 2 == 0);
        (void)world_query(w, dl_pos(q));
    }
    return (now_ms() - t0) / reps;
}

/* The same N^2 answers with no engine in the middle: the host's own work. */
static volatile long g_sink;

static double floor_ms(host *h, int reps)
{
    double t0 = now_ms();
    long sink = 0;
    for (int k = 0; k < reps; k++)
        for (int i = 0; i < h->n; i++)
            for (int j = 0; j < h->n; j++)
                sink += answer(h, i, j);
    g_sink = sink;                   /* the loop is the measurement; keep it */
    return (now_ms() - t0) / reps;
}

/* The control: the same rule with a stored `near`, timed the same way — once as
 * the compiler routes it (laned) and once forced onto N=1, which is where a
 * provider read puts it. `fams` reports which family count each got. */
static double control_ms(int n, host *h, int reps, bool disqualify, int *fams)
{
    char *src = world_src(n, false, disqualify);
    intern *sy = intern_new();
    story_diag di[8];
    story_diags dg = { di, 8, 0, 0 };
    world *w = story_compile(src, "control.story", sy, &dg);
    if (!w) {
        fprintf(stderr, "bench_provider: control compile failed: %s\n",
                dg.count ? di[0].msg : "?");
        exit(1);
    }
    fill_control(w, sy, h, n);
    if (fams) *fams = world_lane_family_count(w);
    double ms = tick_ms(w, sy, reps);
    world_free(w);
    intern_free(sy);
    free(src);
    return ms;
}

static void sweep(const int *ns, int nn, bool use_index, const char *label)
{
    printf("\n== %s ==\n", label);
    printf("  %-6s %-8s %-11s %-11s %-7s %-11s %-11s %-11s %s\n",
           "N", "atoms", "per-atom", "batched", "batch", "host floor",
           "fluent laned", "fluent N=1", "calls/tick");
    for (int i = 0; i < nn; i++) {
        int n = ns[i], reps = n <= 100 ? 50 : 10;
        char *src = world_src(n, true, false);
        double per_atom = 0, batched = 0, flr = 0, ctrl = 0, ctrl1 = 0;
        long calls_pa = 0, calls_b = 0, atoms = 0;
        for (int mode = 0; mode < 2; mode++) {
            intern *sy = intern_new();
            story_diag di[8];
            story_diags dg = { di, 8, 0, 0 };
            world *w = story_compile(src, "provider.story", sy, &dg);
            if (!w) {
                fprintf(stderr, "bench_provider: compile failed: %s\n",
                        dg.count ? di[0].msg : "?");
                exit(1);
            }
            host h;
            host_init(&h, n, sy, use_index);
            world_set_provider_fn(w, prov, &h);
            if (mode == 1) world_set_provider_fill_fn(w, fill, &h);

            bool ok = true;
            if (mode == 1 && world_providers_check(w, &ok) && !ok) {
                fprintf(stderr, "bench_provider: batched != per-atom at N=%d\n", n);
                exit(1);
            }
            h.atom_calls = h.fill_calls = 0;
            double ms = tick_ms(w, sy, reps);
            if (mode == 0) {
                per_atom = ms;
                calls_pa = h.atom_calls / reps;
                atoms = world_provider_atom_count(w);
                flr = floor_ms(&h, reps <= 10 ? 3 : 10);
                int cf = 0;
                ctrl = control_ms(n, &h, reps, false, &cf);
                ctrl1 = control_ms(n, &h, reps, true, NULL);
                if (cf == 0) {
                    fprintf(stderr, "bench_provider: the fluent control stopped "
                            "laning — this table's baseline is gone\n");
                    exit(1);
                }
            } else {
                batched = ms;
                calls_b = (h.atom_calls + h.fill_calls) / reps;
            }
            host_free(&h);
            world_free(w);
            intern_free(sy);
        }
        printf("  %-6d %-8ld %8.4f ms %8.4f ms %5.2fx %8.4f ms %8.4f ms %8.4f ms %ld -> %ld\n",
               n, atoms, per_atom, batched, per_atom / batched, flr, ctrl, ctrl1,
               calls_pa, calls_b);
        free(src);
    }
}

int main(int argc, char **argv)
{
    int nmax = argc > 1 ? atoi(argv[1]) : 400;
    int ns[] = { 50, 100, 200, 400, 800 };
    int nn = 0;
    while (nn < (int)(sizeof ns / sizeof ns[0]) && ns[nn] <= nmax) nn++;

    printf("bench_provider: the interface a .story provider goes through — "
           "N^2 ground atoms, one solve\n");
    sweep(ns, nn, false, "flat host: an O(1) distance test (pure call overhead)");
    sweep(ns, nn, true, "index host: a uniform-grid broadphase, re-entered per call");

    printf("\nReading, and it is not the reading this file was written to get:\n\n"
           "The BOUNDARY is not the cost. `host floor` is every one of those N^2\n"
           "answers computed with no engine in the middle, and it is under 1%% of\n"
           "the tick; batching collapses the call count by a factor of N and moves\n"
           "the tick by nothing (the index host's better ratio is its own scan\n"
           "work amortising, not the crossing). One indirect call per ground atom\n"
           "is an ugly interface, but it is not where the time goes.\n\n"
           "The last two columns say where. They are the SAME rule with `near` as\n"
           "a stored fluent — no host, no callback, no boundary at all — first as\n"
           "the compiler routes it, then forced onto N=1 by a never-firing second\n"
           "rule on the head. `fluent N=1` lands on the provider columns, and\n"
           "`fluent laned` is an order of magnitude under both. The whole cost of\n"
           "a provider is that READING one takes its rule off the lane path\n"
           "(story.c's lane_atom_ok / join_atom_ok reject a provider atom), so the\n"
           "shape that would have solved 64 entities to a word solves N^2 ground\n"
           "rules one at a time instead.\n\n"
           "So the batched fill (#229) is the right interface and the wrong lever:\n"
           "the lever is laning a provider read — a provider column is a bitset the\n"
           "host already knows how to fill, which is exactly what the fill callback\n"
           "hands it. That is the sequel, and bench_slice's 90%% is a third thing\n"
           "again: a host phase that never crosses this interface at all.\n");
    return 0;
}
