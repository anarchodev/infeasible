#include "runtime/runtime.h"

#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t act[RT_MAX_ACTIONS]; int n; } rt_orders;

struct rt {
    plat   *p;
    world  *w;
    intern *syms;
    rt_cart cart;
    char    story[256];

    uint64_t tick;
    char     rejected[192];
    bool     has_rejected;

    /* the action log — a save is (engine-hash, game-hash, THIS) */
    rt_orders *log;
    int        nlog, caplog;

    /* the replay source: the log decides and the cart's proposal is dropped */
    rt_orders *feed;
    int        nfeed, atfeed;
    bool       replaying;
};

rt *rt_open(plat *p, world *w, intern *syms, rt_cart cart, const char *story)
{
    rt *r = calloc(1, sizeof *r);
    r->p = p;
    r->w = w;
    r->syms = syms;
    r->cart = cart;
    snprintf(r->story, sizeof r->story, "%s", story ? story : "");
    for (int i = 0; i < cart.nsheets; i++)
        plat_define_sheet(p, cart.sheets[i], NULL);
    if (cart.init) cart.init(cart.ctx, r);
    return r;
}

void rt_close(rt *r)
{
    if (!r) return;
    free(r->log);
    free(r->feed);
    free(r);
}

world  *rt_world(rt *r)    { return r->w; }
plat   *rt_platform(rt *r) { return r->p; }
intern *rt_syms(rt *r)     { return r->syms; }
uint64_t rt_tick_count(const rt *r) { return r->tick; }
const char *rt_rejected(const rt *r) { return r->has_rejected ? r->rejected : NULL; }
bool rt_replaying(const rt *r) { return r->replaying && r->atfeed < r->nfeed; }

static void log_push(rt *r, const uint32_t *act, int n)
{
    if (r->nlog == r->caplog) {
        r->caplog = r->caplog ? r->caplog * 2 : 64;
        r->log = realloc(r->log, (size_t)r->caplog * sizeof *r->log);
    }
    rt_orders *o = &r->log[r->nlog++];
    o->n = n;
    for (int i = 0; i < n; i++) o->act[i] = act[i];
}

void rt_step(rt *r)
{
    plat_sample_input(r->p);
    r->has_rejected = false;

    uint32_t proposed[RT_MAX_ACTIONS];
    int n = 0;
    if (r->cart.tick) n = r->cart.tick(r->cart.ctx, r, proposed, RT_MAX_ACTIONS);
    if (n < 0) n = 0;
    if (n > RT_MAX_ACTIONS) n = RT_MAX_ACTIONS;

    /* The cart proposes; the source disposes. Live, those are the same thing;
     * replaying, the log's entry replaces the proposal entirely — which is
     * what makes a save loadable rather than merely recorded, and it is the
     * seat a network source takes for lockstep. */
    const uint32_t *act = proposed;
    if (r->replaying) {
        if (r->atfeed < r->nfeed) {
            act = r->feed[r->atfeed].act;
            n = r->feed[r->atfeed].n;
            r->atfeed++;
        } else {
            n = 0;
        }
    }

    if (n > 0) {
        char err[160];
        if (world_step(r->w, act, n, err, sizeof err) == 0) {
            log_push(r, act, n);
        } else {
            snprintf(r->rejected, sizeof r->rejected, "%s", err);
            r->has_rejected = true;
        }
    }
    r->tick++;
    if (r->cart.after) r->cart.after(r->cart.ctx, r);
}

void rt_render(rt *r)
{
    plat_begin_frame(r->p);
    if (r->cart.draw) r->cart.draw(r->cart.ctx, r);
    plat_present(r->p);
}

void rt_advance(rt *r, int nticks)
{
    for (int i = 0; i < nticks; i++) { rt_step(r); rt_render(r); }
}

int rt_save(const rt *r, FILE *out)
{
    if (!out) return -1;
    fprintf(out, "# a save, in the only form this engine has one: the action\n"
                 "# log (DESIGN.md §12). One line per tick; a blank line is a\n"
                 "# tick that changed nothing.\n\n");
    if (r->story[0]) fprintf(out, "story %s\n\n", r->story);
    for (int i = 0; i < r->nlog; i++) {
        for (int j = 0; j < r->log[i].n; j++)
            fprintf(out, "%s%s", j ? " " : "", intern_name(r->syms, r->log[i].act[j]));
        fputc('\n', out);
    }
    return r->nlog;
}

static void feed_push(rt *r, const uint32_t *act, int n)
{
    if (r->nfeed == r->atfeed && r->feed == NULL) r->atfeed = 0;
    r->feed = realloc(r->feed, (size_t)(r->nfeed + 1) * sizeof *r->feed);
    rt_orders *o = &r->feed[r->nfeed++];
    o->n = n;
    for (int i = 0; i < n; i++) o->act[i] = act[i];
}

int rt_load(rt *r, const char *text, char *err, size_t errsz)
{
    if (r->tick != 0 || r->nlog != 0) {
        if (err) snprintf(err, errsz,
                          "load into a fresh world: a log is a history from genesis");
        return -1;
    }
    char *copy = strdup(text ? text : "");
    for (char *line = copy, *next; line && *line; line = next) {
        char *nl = strchr(line, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || *line == '\0') continue;
        if (strncmp(line, "story ", 6) == 0) {
            /* a log replayed against the wrong world is the classic desync,
             * so the save names its story and this is a refusal, not a
             * mystery. Compared on the basename, since a save travels and an
             * absolute path does not. */
            const char *want = line + 6;
            const char *wb = strrchr(want, '/');
            const char *hb = strrchr(r->story, '/');
            wb = wb ? wb + 1 : want;
            hb = hb ? hb + 1 : r->story;
            if (r->story[0] && strcmp(wb, hb) != 0) {
                if (err) snprintf(err, errsz,
                                  "this save is from '%s' — its action log names "
                                  "atoms '%s' may not have", wb, hb);
                free(copy);
                return -1;
            }
            continue;
        }
        uint32_t act[RT_MAX_ACTIONS];
        int n = 0;
        for (char *t = strtok(line, " \t"); t && n < RT_MAX_ACTIONS;
             t = strtok(NULL, " \t"))
            act[n++] = intern_id(r->syms, t);
        feed_push(r, act, n);
    }
    free(copy);
    r->replaying = true;
    r->atfeed = 0;
    return r->nfeed;
}
