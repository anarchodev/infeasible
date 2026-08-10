/* tests/test_secondclient.c — the SECOND CLIENT (DESIGN.md §4.2, §11 M2).
 *
 * The layering claim this repo makes is that clients use zero private APIs:
 * everything a client does — build a world, query it, offer actions, propose
 * them, render the result — goes through the public surface every client gets.
 * There is no blessed client to check that against, so the claim is carried
 * entirely by having a *second* one, in tests, the way golden tests carry
 * semantics. §11 calls it a hard deliverable for exactly that reason.
 *
 * A second client is only evidence if it is genuinely a second one, so this is
 * as unlike `web/carts/cellar.mjs` as a client of the same world can be:
 *
 *   different LANGUAGE      C against world_*, not JS against the §6.3 binding
 *   different PRESENTATION  a text frame, not Canvas2D
 *   different ARCHITECTURE  REACTIVE, not polled. The cart re-asks the world
 *                           everything it draws, every frame. This one asks
 *                           once, at startup, and after that its picture is
 *                           maintained ONLY from what a step reports — the
 *                           subscription edges and the numeric changeset. It
 *                           never re-reads a fluent it already knows about.
 *
 * That last difference is what gives the test teeth beyond "it compiles". A
 * reactive client's cached picture must equal a freshly polled one at every
 * single tick, and this file asserts exactly that (`view_eq`) after every step.
 * If the subscription channel ever misses an edge, or reports one a step did
 * not cause, the two pictures separate and this fails — which is the only
 * check anywhere that the reactive channel is COMPLETE rather than merely
 * correct about what it does report.
 *
 * The two clients meet on a shared fixture: `examples/cellar_play.log`, a save
 * in the only form this engine has one (§12 — engine-hash, game-hash, action
 * log). `web/platform_check.mjs` plays the cart by clicking and asserts the log
 * it produced is that file; this replays that file and asserts it lands in the
 * same world, offering the same commands. Neither client can be quietly
 * special.
 *
 * Note what is NOT included below: no dl_col.h, no factindex.h, no reaching
 * into `world` internals. The include list is the test.
 */

#include "core/intern.h"
#include "lang/story.h"
#include "logic/dl.h"
#include "state/world.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

#ifndef STORY_DIR
#define STORY_DIR "examples"
#endif

/* ---- the vocabulary this client knows about ------------------------------
 *
 * Hand-written, because that is the point: a C client has no generated
 * binding, so it spells ground atoms the way the interface artifact says to
 * (`pred(a,b)`, `pred(a)=value`) and takes the risk the artifact exists to
 * remove. `test_iface` pins that spelling; here it is being *used*. */

static const char *ACTORS[] = { "hero", "guard" };
static const char *ITEMS[]  = { "rusty_key", "torch", "antidote" };
static const char *ROOMS[]  = { "cellar", "hall", "vault" };
static const char *DOORV[]  = { "locked", "closed", "open" };
enum { NACT = 2, NITEM = 3, NROOM = 3, NDOOR = 3 };

/* Everything this client puts on screen, and nothing else. */
typedef struct {
    int  at[NACT];                    /* room index, -1 = nowhere known yet */
    long hp[NACT];
    int  door;
    bool weak[NACT], dark[NACT], down[NACT];
    bool can_enter[NACT], can_unlock[NACT], can_force[NACT];
    bool hold[NACT][NITEM];
    bool onfloor[NITEM][NROOM];
} view;

/* ---- subscriptions -------------------------------------------------------
 *
 * One handle per literal the view is a function of. A base fact (`holding`,
 * `on_floor`, the `at(·)=v` family) and a derived judgment (`weakened`,
 * `can_force_door`) are subscribed with the identical call, which is the
 * property that lets an author refactor one into the other without touching
 * this file. */

enum sub_kind { S_AT, S_DOOR, S_WEAK, S_DARK, S_DOWN,
                S_ENTER, S_UNLOCK, S_FORCE, S_HOLD, S_FLOOR };

typedef struct { int sub, kind, a, b; } sub_rec;
enum { MAXSUBS = 64 };

typedef struct {
    world   *w;
    intern  *syms;
    sub_rec  recs[MAXSUBS];
    int      n;
    view     v;
} client;

static uint32_t atom(client *c, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return intern_id(c->syms, buf);
}

static void watch(client *c, uint32_t a, int kind, int i, int j)
{
    if (c->n >= MAXSUBS) return;
    c->recs[c->n].sub  = world_subscribe(c->w, dl_pos(a));
    c->recs[c->n].kind = kind;
    c->recs[c->n].a    = i;
    c->recs[c->n].b    = j;
    c->n++;
}

static void apply(view *v, const sub_rec *r, dl_verdict to)
{
    bool on = (to == DL_PROVED);
    switch (r->kind) {
    /* A multi-valued family reports both halves of a move; only the value
     * that became true names the new one. */
    case S_AT:     if (on) v->at[r->a] = r->b;   break;
    case S_DOOR:   if (on) v->door     = r->b;   break;
    case S_WEAK:   v->weak[r->a]       = on;     break;
    case S_DARK:   v->dark[r->a]       = on;     break;
    case S_DOWN:   v->down[r->a]       = on;     break;
    case S_ENTER:  v->can_enter[r->a]  = on;     break;
    case S_UNLOCK: v->can_unlock[r->a] = on;     break;
    case S_FORCE:  v->can_force[r->a]  = on;     break;
    case S_HOLD:   v->hold[r->a][r->b] = on;     break;
    case S_FLOOR:  v->onfloor[r->a][r->b] = on;  break;
    }
}

static void subscribe_all(client *c)
{
    for (int i = 0; i < NACT; i++) {
        for (int r = 0; r < NROOM; r++)
            watch(c, atom(c, "at(%s)=%s", ACTORS[i], ROOMS[r]), S_AT, i, r);
        watch(c, atom(c, "weakened(%s)", ACTORS[i]),       S_WEAK,   i, 0);
        watch(c, atom(c, "in_dark(%s)", ACTORS[i]),        S_DARK,   i, 0);
        watch(c, atom(c, "down(%s)", ACTORS[i]),           S_DOWN,   i, 0);
        watch(c, atom(c, "can_enter_vault(%s)", ACTORS[i]), S_ENTER,  i, 0);
        watch(c, atom(c, "can_unlock_door(%s)", ACTORS[i]), S_UNLOCK, i, 0);
        watch(c, atom(c, "can_force_door(%s)", ACTORS[i]),  S_FORCE,  i, 0);
        for (int t = 0; t < NITEM; t++)
            watch(c, atom(c, "holding(%s,%s)", ACTORS[i], ITEMS[t]), S_HOLD, i, t);
    }
    for (int d = 0; d < NDOOR; d++)
        watch(c, atom(c, "door=%s", DOORV[d]), S_DOOR, 0, d);
    for (int t = 0; t < NITEM; t++)
        for (int r = 0; r < NROOM; r++)
            watch(c, atom(c, "on_floor(%s,%s)", ITEMS[t], ROOMS[r]), S_FLOOR, t, r);
}

/* The one poll this client performs: the opening frame. Every later frame is
 * built from what a step reported. */
static void paint_initial(client *c)
{
    for (int i = 0; i < c->n; i++)
        apply(&c->v, &c->recs[i], world_sub_verdict(c->w, c->recs[i].sub));
    for (int i = 0; i < NACT; i++)
        c->v.hp[i] = world_get_num(c->w, atom(c, "hp(%s)", ACTORS[i]));
}

/* ---- the honesty check ---------------------------------------------------
 *
 * What the client WOULD see if it threw its picture away and asked again. The
 * reactive picture must equal this after every tick. */
static void poll_view(client *c, view *out)
{
    memset(out, 0, sizeof *out);
    out->at[0] = out->at[1] = -1;
    out->door = -1;
    for (int i = 0; i < c->n; i++) {
        const sub_rec *r = &c->recs[i];
        apply(out, r, world_sub_verdict(c->w, r->sub));
    }
    for (int i = 0; i < NACT; i++)
        out->hp[i] = world_get_num(c->w, atom(c, "hp(%s)", ACTORS[i]));
}

static bool view_eq(const view *a, const view *b)
{
    return memcmp(a, b, sizeof *a) == 0;
}

/* ---- the client's own reading of the world ------------------------------ */

/* The commands this client offers, from the VIEW alone — no query goes out.
 * The same list the cart builds, in the same order, so two clients disagreeing
 * about what a player may do is a test failure rather than a bug report. */
static int offer(const view *v, int who, char out[][24], bool *ok, int max)
{
    int n = 0;
    #define ADD(lbl, enabled) \
        do { if (n < max) { snprintf(out[n], 24, "%s", (lbl)); ok[n] = (enabled); n++; } } while (0)

    if (v->at[who] == 0) ADD("GO TO HALL", true);
    if (v->at[who] == 1) {
        ADD("GO TO CELLAR", true);
        ADD("ENTER VAULT",  v->can_enter[who]);
        ADD("UNLOCK DOOR",  v->can_unlock[who]);
        ADD("FORCE DOOR",   v->can_force[who]);
    }
    if (v->at[who] == 2) ADD("LEAVE VAULT", true);

    for (int t = 0; t < NITEM; t++) {
        char lbl[24];
        if (v->at[who] >= 0 && v->onfloor[t][v->at[who]]) {
            snprintf(lbl, sizeof lbl, "TAKE %s", ITEMS[t]);
            ADD(lbl, true);
        } else if (v->hold[who][t]) {
            snprintf(lbl, sizeof lbl, "DROP %s", ITEMS[t]);
            ADD(lbl, true);
        }
    }
    #undef ADD
    return n;
}

/* Presentation, over the same public surface: a text frame. Proof that a
 * renderer needs nothing the query surface does not already give. */
static void render(const view *v, char *buf, size_t cap)
{
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "  door: %s\n",
                          v->door >= 0 ? DOORV[v->door] : "?");
    for (int r = 0; r < NROOM; r++) {
        o += (size_t)snprintf(buf + o, cap - o, "  %-7s", ROOMS[r]);
        for (int i = 0; i < NACT; i++) {
            if (v->at[i] != r) continue;
            o += (size_t)snprintf(buf + o, cap - o, " %s(%ld)%s%s", ACTORS[i], v->hp[i],
                                  v->weak[i] ? " weak" : "", v->dark[i] ? " dark" : "");
            for (int t = 0; t < NITEM; t++)
                if (v->hold[i][t])
                    o += (size_t)snprintf(buf + o, cap - o, " [%s]", ITEMS[t]);
        }
        for (int t = 0; t < NITEM; t++)
            if (v->onfloor[t][r])
                o += (size_t)snprintf(buf + o, cap - o, " <%s>", ITEMS[t]);
        o += (size_t)snprintf(buf + o, cap - o, "\n");
    }
}

/* ---- the save: a story name and one line of orders per tick -------------- */

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = '\0';
    fclose(f);
    return s;
}

int main(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/cellar_play.story", STORY_DIR);
    char *src = slurp(path);
    CHECK(src != NULL);
    snprintf(path, sizeof path, "%s/cellar_play.log", STORY_DIR);
    char *log = slurp(path);
    CHECK(log != NULL);

    intern *syms = intern_new();
    story_diag di[32];
    story_diags dg = { di, 32, 0, 0 };
    world *w = story_compile(src, "cellar_play.story", syms, &dg);
    if (!w) fprintf(stderr, "  compile: %s\n", dg.count ? di[0].msg : "?");
    CHECK(w != NULL);

    client c = { .w = w, .syms = syms, .n = 0 };
    memset(&c.v, 0, sizeof c.v);
    c.v.at[0] = c.v.at[1] = -1;
    c.v.door = -1;
    subscribe_all(&c);
    paint_initial(&c);

    /* the opening frame, and the commands it implies */
    CHECK(c.v.at[0] == 0 && c.v.at[1] == 1);        /* hero cellar, guard hall */
    CHECK(c.v.door == 0);                            /* locked */
    CHECK(c.v.hp[0] == 12 && c.v.hp[1] == 12);
    CHECK(c.v.weak[0] && !c.v.weak[1]);              /* the hero is poisoned */
    CHECK(c.v.dark[0]);                              /* and has no torch */

    {
        char lbl[8][24]; bool ok[8];
        int n = offer(&c.v, 0, lbl, ok, 8);
        CHECK(n == 3);
        CHECK(strcmp(lbl[0], "GO TO HALL") == 0);
        CHECK(strcmp(lbl[1], "TAKE rusty_key") == 0);
        CHECK(strcmp(lbl[2], "TAKE torch") == 0);    /* the cart's list, verbatim */
    }

    /* ---- replay ---------------------------------------------------------- */

    int ticks = 0, saw_footstep = 0, saw_pickup = 0, saw_heave = 0,
        saw_sip = 0, saw_clunk = 0;
    bool checked_force = false, checked_receipt = false;
    char err[128];

    /* Walk the lines by hand: the inner strtok over one line's orders would
     * clobber an outer strtok's state. */
    for (char *line = log, *next; line && *line; line = next) {
        char *nl = strchr(line, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == '\0') continue;
        if (strncmp(line, "story ", 6) == 0) {
            /* a log replayed against the wrong world is the classic desync;
             * the save names its story so that is a refusal, not a mystery */
            CHECK(strstr(line, "cellar_play.story") != NULL);
            continue;
        }

        /* Before the tick that forces the door: the two clients' shared claim
         * is that the guard can and the hero cannot, for the same door. */
        if (strncmp(line, "force_door", 10) == 0) {
            CHECK(!c.v.can_force[0] && c.v.can_force[1]);
            checked_force = true;
        }

        uint32_t acts[8];
        int na = 0;
        for (char *t = strtok(line, " "); t && na < 8; t = strtok(NULL, " "))
            acts[na++] = intern_id(syms, t);
        CHECK(na > 0);
        CHECK(world_step(w, acts, na, err, sizeof err) == 0);
        ticks++;

        /* THE REACTIVE UPDATE — the whole client, in nine lines. Nothing here
         * asks the world anything; it is told. */
        int ne;
        const world_sub_edge *edges = world_sub_edges(w, &ne);
        for (int i = 0; i < ne; i++)
            for (int r = 0; r < c.n; r++)
                if (c.recs[r].sub == edges[i].sub) apply(&c.v, &c.recs[r], edges[i].to);
        int nd;
        const world_num_delta *nums = world_num_deltas(w, &nd);
        for (int i = 0; i < nd; i++)
            for (int a = 0; a < NACT; a++)
                if (nums[i].atom == atom(&c, "hp(%s)", ACTORS[a])) c.v.hp[a] = nums[i].to;

        /* ...and the invariant that makes the above trustworthy. */
        view polled;
        poll_view(&c, &polled);
        if (!view_eq(&c.v, &polled)) {
            char a[512] = {0}, b[512] = {0};
            render(&c.v, a, sizeof a);
            render(&polled, b, sizeof b);
            fprintf(stderr, "reactive view diverged at tick %d\n--- told:\n%s--- asked:\n%s",
                    ticks, a, b);
        }
        CHECK(view_eq(&c.v, &polled));

        /* the transient stream: cues the renderer would fire and then forget */
        int nem;
        const uint32_t *em = world_emits(w, &nem);
        for (int i = 0; i < nem; i++) {
            const char *name = intern_name(syms, em[i]);
            if (strncmp(name, "footstep", 8) == 0) saw_footstep++;
            if (strncmp(name, "pickup", 6) == 0)   saw_pickup++;
            if (strncmp(name, "heave", 5) == 0)    saw_heave++;
            if (strncmp(name, "sip", 3) == 0)      saw_sip++;
            if (strcmp(name, "clunk") == 0)        saw_clunk++;
        }

        /* the numeric receipt, on the one tick that spends anything */
        if (saw_heave && !checked_receipt) {
            world_receipt r;
            CHECK(world_num_receipt(w, atom(&c, "hp(guard)"), &r));
            CHECK(r.base == 12 && r.applied == 10);
            CHECK(r.n >= 1 && strcmp(intern_name(syms, r.items[0].pred), "force_door") == 0);
            checked_receipt = true;
        }
    }

    /* ---- the same world the other client reached ------------------------- */

    CHECK(ticks == 10);
    CHECK(checked_force && checked_receipt);
    CHECK(c.v.at[0] == 2);                    /* the hero is in the vault */
    CHECK(c.v.door == 2);                     /* open */
    CHECK(c.v.hp[1] == 10);                   /* the guard paid for it */
    CHECK(!c.v.weak[0]);                      /* the antidote worked */
    CHECK(!c.v.dark[0]);                      /* the torch is lit */
    CHECK(c.v.hold[0][1] && c.v.hold[0][0] && c.v.hold[0][2]);
    /* four moves, three pickups, one lock, one shoulder, one swallow — the
     * renderer's whole soundtrack, and none of it is state */
    CHECK(saw_footstep == 4 && saw_pickup == 3);
    CHECK(saw_heave == 1 && saw_sip == 1 && saw_clunk == 1);

    /* A step that never happened reports nothing — the client must not repaint
     * on a refusal. */
    {
        uint32_t bogus = intern_id(syms, "force_door(hero)");
        CHECK(world_step(w, &bogus, 1, err, sizeof err) == 0);   /* guard-failed, not refused */
        int ne2;
        world_sub_edges(w, &ne2);
        CHECK(ne2 == 0);
        view polled;
        poll_view(&c, &polled);
        CHECK(view_eq(&c.v, &polled));
    }

    char frame[512] = { 0 };
    render(&c.v, frame, sizeof frame);
    printf("%s", frame);

    world_free(w);
    intern_free(syms);
    free(src);
    free(log);
    printf("test_secondclient: all passed\n");
    return 0;
}
