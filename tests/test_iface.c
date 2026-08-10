/* Golden test for the §6.3 INTERFACE ARTIFACT — the declared vocabulary a
 * client checks against.
 *
 * The artifact exists because the intern table's failure mode is silence: a
 * host that spells a ground atom wrong gets a fresh, always-false atom and a
 * REFUTED verdict forever. So the artifact publishes the vocabulary AND the
 * ground-atom spelling, and the typed binding is generated from it.
 *
 * Which makes the load-bearing assertion here a ROUND TRIP, not a field-by-field
 * comparison: take the artifact, spell every ground atom the way it says to, and
 * check the engine actually has that atom. If the spelling contract and the
 * grounder ever drift, a generated client goes quietly always-false — exactly
 * the bug the artifact is supposed to end — so nothing else in this file matters
 * as much as that loop. */

#include "lang/story.h"
#include "state/world.h"
#include "core/intern.h"
#include "core/arena.h"
#include "lsp/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                     return 1; } } while (0)

static const char *SRC =
    "scene cellar\n"
    "sort actor, item\n"
    "domain cell\n"
    "enum school { evocation, abjuration }\n"
    "entity (\n"
    "  hero, guard : actor\n"
    "  torch, key  : item\n"
    ")\n"
    "state (\n"
    "  holding(actor, item)\n"
    "  hp(actor) : int in 0 .. 30\n"
    "  door : { locked, closed, open }\n"
    "  at(actor) : cell\n"
    "  poisoned(actor)\n"
    ")\n"
    "provider near(actor, actor)\n"
    "function step(cell, int) : cell\n"
    "value ac(actor) : int\n"
    "emit spark(actor)\n"
    "init ( door = locked  poisoned(hero)  hp(hero) = 10  hp(guard) = 12 )\n"
    "rule weak(X: actor): poisoned(X) => weakened(X)\n"
    "rule ac_base(X: actor): => ac(X) = 10\n"
    "action unlock(X: actor, K: item):\n"
    "  requires holding(X, K) & door = locked\n"
    "  causes   door = closed & spark(X)\n"
    "action shove(X: actor): requires door = closed causes door = open\n"
    "action pull(X: actor): requires door = closed causes door = locked\n"
    /* `door` is a global, so ANY two actors contest it: the group keys
     * nothing (`_`), admitting one of the pair per step whoever acts */
    "exclusive shove(_), pull(_)\n"
    "action bless(X: actor): causes ~poisoned(X)\n"
    "action curse(X: actor): causes poisoned(X)\n"
    /* per-actor state, so the key is the actor: blessing one and cursing
     * another in one step is fine */
    "exclusive bless(X), curse(X)\n";

/* The entity list of a named sort, from the artifact. */
static const json *entities_of(const json *iface, const char *sort)
{
    const json *sorts = json_get(iface, "sorts");
    for (size_t i = 0; i < json_arr_len(sorts); i++) {
        const json *s = json_arr_at(sorts, i);
        const char *n = json_str(json_get(s, "name"));
        if (n && strcmp(n, sort) == 0) return json_get(s, "entities");
    }
    return NULL;
}

/* Spell a ground term the way the artifact says to: `pred(a,b)`, bare at
 * arity 0. This is deliberately written from the DOCUMENTED convention rather
 * than by calling the compiler's own helper — a test that reuses the code under
 * test cannot catch it drifting from what it promised. */
static void ground(char *buf, size_t cap, const char *pred,
                   const char *const *args, int nargs)
{
    size_t off = (size_t)snprintf(buf, cap, "%s", pred);
    if (!nargs) return;
    off += (size_t)snprintf(buf + off, cap - off, "(");
    for (int i = 0; i < nargs; i++)
        off += (size_t)snprintf(buf + off, cap - off, "%s%s", i ? "," : "", args[i]);
    snprintf(buf + off, cap - off, ")");
}

/* Walk the cross product of a predicate's argument sorts, spelling each ground
 * instance and handing it to `visit`. Returns non-zero on the first failure. */
static int each_instance(const json *iface, const json *args, const char *pred,
                         int (*visit)(const char *term, void *ctx), void *ctx)
{
    int nargs = (int)json_arr_len(args);
    const char *pick[4];
    const json *doms[4];
    if (nargs > 4) return 0;
    for (int k = 0; k < nargs; k++) {
        doms[k] = entities_of(iface, json_str(json_arr_at(args, k)));
        if (!doms[k]) return 0;              /* a domain / int: not enumerable */
    }
    int idx[4] = { 0, 0, 0, 0 };
    for (;;) {
        for (int k = 0; k < nargs; k++)
            pick[k] = json_str(json_arr_at(doms[k], (size_t)idx[k]));
        char term[256];
        ground(term, sizeof term, pred, pick, nargs);
        int rc = visit(term, ctx);
        if (rc) return rc;
        int k = nargs - 1;
        while (k >= 0 && ++idx[k] >= (int)json_arr_len(doms[k])) { idx[k] = 0; k--; }
        if (k < 0 || nargs == 0) break;
    }
    return 0;
}

struct wctx { world *w; intern *sy; const char *kind; };

/* Every boolean state atom the artifact describes is a fluent the world has. */
static int visit_bool(const char *term, void *vctx)
{
    struct wctx *c = vctx;
    if (!world_has_fluent(c->w, intern_id(c->sy, term))) {
        fprintf(stderr, "FAIL %s: '%s' is in the artifact but not in the world\n",
                c->kind, term);
        return 1;
    }
    return 0;
}

/* Every action term the artifact describes was interned by the grounder —
 * probed WITHOUT interning, so a miss is a miss rather than a fresh atom. */
static int visit_interned(const char *term, void *vctx)
{
    struct wctx *c = vctx;
    if (intern_find_n(c->sy, term, (uint32_t)strlen(term)) == INTERN_NONE) {
        fprintf(stderr, "FAIL %s: '%s' is in the artifact but was never interned\n",
                c->kind, term);
        return 1;
    }
    return 0;
}

int main(void)
{
    intern *sy = intern_new();
    story_diag di[16];
    story_diags dg = { di, 16, 0, 0 };
    char *artifact = NULL;
    world *w = story_compile_iface(SRC, "iface.story", sy, &dg, &artifact);
    if (!w) { fprintf(stderr, "compile: %s\n", dg.count ? di[0].msg : "?"); return 1; }
    CHECK(artifact != NULL);

    arena a;
    arena_init(&a);
    const json *iface = json_parse(&a, artifact, strlen(artifact));
    CHECK(iface != NULL);                       /* it is valid JSON at all */
    CHECK(strcmp(json_str(json_get(iface, "artifact")), "infeasible.interface") == 0);
    CHECK(json_int(json_get(iface, "version"), 0) == 1);
    CHECK(strcmp(json_str(json_get(iface, "story")), "iface.story") == 0);
    CHECK(strcmp(json_str(json_get(iface, "scene")), "cellar") == 0);

    /* the spelling contract is published, not implied */
    const json *g = json_get(iface, "ground");
    CHECK(strcmp(json_str(json_get(g, "atom")), "pred(arg1,arg2)") == 0);
    CHECK(strcmp(json_str(json_get(g, "nullary")), "pred") == 0);
    CHECK(strcmp(json_str(json_get(g, "value")), "pred(args)=value") == 0);

    /* sorts carry their entities; a domain has none to carry (§5.6) */
    CHECK(json_arr_len(entities_of(iface, "actor")) == 2);
    CHECK(strcmp(json_str(json_arr_at(entities_of(iface, "actor"), 0)), "hero") == 0);
    CHECK(entities_of(iface, "cell") == NULL);
    {
        const json *sorts = json_get(iface, "sorts");
        bool sawcell = false;
        for (size_t i = 0; i < json_arr_len(sorts); i++) {
            const json *s = json_arr_at(sorts, i);
            if (strcmp(json_str(json_get(s, "name")), "cell") == 0) {
                sawcell = true;
                CHECK(strcmp(json_str(json_get(s, "kind")), "domain") == 0);
            }
        }
        CHECK(sawcell);
    }
    /* an enum is a value domain, listed apart from the entity sorts (#96) */
    {
        const json *e = json_arr_at(json_get(iface, "enums"), 0);
        CHECK(strcmp(json_str(json_get(e, "name")), "school") == 0);
        CHECK(json_arr_len(json_get(e, "values")) == 2);
    }

    /* every declared kind lands in exactly one section, with its shape */
    struct { const char *sec, *name; int nargs; } expect[] = {
        { "state", "holding", 2 }, { "state", "hp", 1 }, { "state", "door", 0 },
        { "state", "at", 1 },      { "state", "poisoned", 1 },
        { "providers", "near", 2 }, { "values", "ac", 1 },
        { "emits", "spark", 1 },    { "judgments", "weakened", 1 },
    };
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
        const json *sec = json_get(iface, expect[i].sec);
        bool found = false;
        for (size_t k = 0; k < json_arr_len(sec); k++) {
            const json *ent = json_arr_at(sec, k);
            if (strcmp(json_str(json_get(ent, "name")), expect[i].name) != 0) continue;
            found = true;
            CHECK((int)json_arr_len(json_get(ent, "args")) == expect[i].nargs);
        }
        if (!found) {
            fprintf(stderr, "FAIL: %s missing from \"%s\"\n",
                    expect[i].name, expect[i].sec);
            return 1;
        }
    }

    /* domains: the range a client clamps its UI to, and the values it renders */
    {
        const json *state = json_get(iface, "state");
        for (size_t i = 0; i < json_arr_len(state); i++) {
            const json *f = json_arr_at(state, i);
            const char *n = json_str(json_get(f, "name"));
            if (strcmp(n, "hp") == 0) {
                CHECK(strcmp(json_str(json_get(f, "type")), "int") == 0);
                CHECK(json_int(json_get(f, "min"), -1) == 0);
                CHECK(json_int(json_get(f, "max"), -1) == 30);
            } else if (strcmp(n, "door") == 0) {
                CHECK(strcmp(json_str(json_get(f, "type")), "enum") == 0);
                CHECK(json_arr_len(json_get(f, "values")) == 3);
            } else if (strcmp(n, "at") == 0) {
                CHECK(strcmp(json_str(json_get(f, "type")), "cell") == 0);
                CHECK(strcmp(json_str(json_get(f, "domain")), "cell") == 0);
            } else if (strcmp(n, "poisoned") == 0) {
                CHECK(strcmp(json_str(json_get(f, "type")), "bool") == 0);
            }
        }
    }

    /* actions publish parameter NAMES as well as sorts — a generated
     * constructor wants `unlock(who, what)`, not `unlock(arg1, arg2)` */
    {
        const json *acts = json_get(iface, "actions");
        const json *unlock = NULL;
        for (size_t i = 0; i < json_arr_len(acts); i++)
            if (strcmp(json_str(json_get(json_arr_at(acts, i), "name")), "unlock") == 0)
                unlock = json_arr_at(acts, i);
        CHECK(unlock != NULL);
        const json *ps = json_get(unlock, "params");
        CHECK(json_arr_len(ps) == 2);
        CHECK(strcmp(json_str(json_get(json_arr_at(ps, 0), "name")), "X") == 0);
        CHECK(strcmp(json_str(json_get(json_arr_at(ps, 0), "sort")), "actor") == 0);
        CHECK(strcmp(json_str(json_get(json_arr_at(ps, 1), "sort")), "item") == 0);
    }

    /* #159: the protocol the bound host enforces at construction. A named
     * position is the key; `_` is null and never constrains — the difference
     * between "one of these per actor" and "one of these per step". */
    {
        const json *ex = json_get(iface, "exclusive");
        CHECK(json_arr_len(ex) == 2);
        const json *g0 = json_get(json_arr_at(ex, 0), "members");
        CHECK(json_arr_len(g0) == 2);
        CHECK(strcmp(json_str(json_get(json_arr_at(g0, 0), "action")), "shove") == 0);
        CHECK(json_is(json_arr_at(json_get(json_arr_at(g0, 0), "key"), 0), JSON_NULL));
        const json *g1 = json_get(json_arr_at(ex, 1), "members");
        CHECK(strcmp(json_str(json_get(json_arr_at(g1, 0), "action")), "bless") == 0);
        CHECK(strcmp(json_str(json_arr_at(json_get(json_arr_at(g1, 0), "key"), 0)),
                     "X") == 0);
    }

    /* ---- THE ROUND TRIP: the artifact's spelling is the engine's ---------- */
    {
        struct wctx ctx = { w, sy, "state" };
        const json *state = json_get(iface, "state");
        for (size_t i = 0; i < json_arr_len(state); i++) {
            const json *f = json_arr_at(state, i);
            const char *n = json_str(json_get(f, "name"));
            const char *ty = json_str(json_get(f, "type"));
            const json *args = json_get(f, "args");
            if (strcmp(ty, "bool") == 0) {
                CHECK(each_instance(iface, args, n, visit_bool, &ctx) == 0);
            } else if (strcmp(ty, "enum") == 0) {
                /* a multi-valued fluent erases to one boolean atom per value,
                 * spelled `pred(args)=value` — the artifact says so, and the
                 * grounder had better agree */
                const json *vals = json_get(f, "values");
                for (size_t v = 0; v < json_arr_len(vals); v++) {
                    char term[256];
                    snprintf(term, sizeof term, "%s=%s", n, json_str(json_arr_at(vals, v)));
                    CHECK(world_has_fluent(w, intern_id(sy, term)));
                }
            }
        }
        ctx.kind = "actions";
        const json *acts = json_get(iface, "actions");
        for (size_t i = 0; i < json_arr_len(acts); i++) {
            const json *act = json_arr_at(acts, i);
            const json *ps = json_get(act, "params");
            /* params -> the sort list each_instance walks */
            char sortbuf[256];
            size_t off = (size_t)snprintf(sortbuf, sizeof sortbuf, "[");
            for (size_t k = 0; k < json_arr_len(ps); k++)
                off += (size_t)snprintf(sortbuf + off, sizeof sortbuf - off, "%s\"%s\"",
                                        k ? "," : "",
                                        json_str(json_get(json_arr_at(ps, k), "sort")));
            snprintf(sortbuf + off, sizeof sortbuf - off, "]");
            arena a2;
            arena_init(&a2);
            const json *sortlist = json_parse(&a2, sortbuf, strlen(sortbuf));
            int rc = each_instance(iface, sortlist,
                                   json_str(json_get(act, "name")),
                                   visit_interned, &ctx);
            arena_release(&a2);
            CHECK(rc == 0);
        }
    }

    /* a story that does not compile publishes no vocabulary */
    {
        intern *s2 = intern_new();
        story_diags d2 = { di, 16, 0, 0 };
        char *j2 = (char *)"sentinel";
        world *bad = story_compile_iface("sort actor\nrule r: nope => bad\n"
                                         "action a: causes undeclared\n",
                                         "bad.story", s2, &d2, &j2);
        CHECK(bad == NULL && j2 == NULL);
        intern_free(s2);
    }

    arena_release(&a);
    free(artifact);
    world_free(w);
    intern_free(sy);
    printf("test_iface: all passed\n");
    return 0;
}
