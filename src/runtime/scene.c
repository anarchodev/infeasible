#include "runtime/scene.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    S_MAXTERM = 128, S_MAXNAME = 64,
    S_MAXPANELS = 64, S_MAXCAPTIONS = 64, S_MAXSHADED = 32, S_MAXGAUGES = 16,
    S_MAXSLOTS = 64, S_MAXHELD = 64, S_MAXMENU = 32, S_MAXACTIONS = 512,
    S_MAXSHOWS = 128, S_MAXPAIRS = 256,
};

typedef struct { char a[S_MAXNAME], b[S_MAXNAME]; } pair;

typedef struct {
    char e[S_MAXNAME];
    char anchor[S_MAXNAME];
    int  x, y;                       /* where the last draw put it */
    bool drawn;
} occupant;

typedef struct {
    char   term[S_MAXTERM];
    char   label[S_MAXNAME];
    bool   ok;
    dl_lit blocker;
    bool   has_blocker;
    int    x, y, w, h;
} menu_row;

struct scene {
    world      *w;
    intern     *syms;
    const iface *f;
    plat       *p;

    pair     panels[S_MAXPANELS];      int npanels;
    pair     captions[S_MAXCAPTIONS];  int ncaptions;
    char     shaded[S_MAXSHADED][S_MAXNAME]; int nshaded;
    pair     gauges[S_MAXGAUGES];      int ngauges;
    pair     held[S_MAXHELD];          int nheld;      /* (item, holder) */
    pair     shows[S_MAXSHOWS];        int nshows;     /* (drawable, sprite) */
    occupant slots[S_MAXSLOTS];        int nslots;
    char     picked[S_MAXNAME];        bool has_picked;
    char     aimed[S_MAXNAME];         bool has_aimed;
    menu_row menu[S_MAXMENU];          int nmenu;
    char     ids[S_MAXMENU + S_MAXSLOTS][S_MAXTERM + 8];  int nids;
};

/* ---- names ------------------------------------------------------------------ */

void scene_say(const char *atom, char *out, size_t cap)
{
    if (!atom) { if (cap) out[0] = 0; return; }
    const char *s = atom;
    /* a one-letter vocabulary prefix (`w_`, `st_` is two — only single-letter
     * prefixes are stripped, matching how authors disambiguate enum members) */
    if (islower((unsigned char)s[0]) && s[1] == '_') s += 2;
    size_t k = 0;
    for (; s[k] && k + 1 < cap; k++) {
        char c = s[k];
        out[k] = (c == '_') ? ' ' : (char)toupper((unsigned char)c);
    }
    out[k] = 0;
}

/* `take_torch(hero,hall)` -> `take_torch`: the label a menu row shows. */
static void verb_of(const char *term, char *out, size_t cap)
{
    size_t k = 0;
    for (; term[k] && term[k] != '(' && k + 1 < cap; k++) out[k] = term[k];
    out[k] = 0;
}

/* A ground term's arguments — they are in its name, which is the spelling the
 * §6.3 artifact publishes. */
static int args_of(const char *term, char out[][S_MAXNAME], int cap)
{
    const char *open = strchr(term, '(');
    if (!open) return 0;
    int n = 0;
    const char *p = open + 1;
    while (*p && *p != ')' && n < cap) {
        size_t k = 0;
        while (*p && *p != ',' && *p != ')' && k + 1 < S_MAXNAME) out[n][k++] = *p++;
        out[n][k] = 0;
        n++;
        if (*p == ',') p++;
    }
    return n;
}

/* the predicate an atom names: `hp(guard)` -> `hp`, `at(hero)=hall` -> `at` */
static void pred_of(const char *atom, char *out, size_t cap)
{
    size_t k = 0;
    for (; atom[k] && atom[k] != '(' && atom[k] != '=' && k + 1 < cap; k++)
        out[k] = atom[k];
    out[k] = 0;
}

/* ---- asking the world -------------------------------------------------------- */

static bool proved_atom(scene *s, const char *name)
{
    return world_query(s->w, dl_pos(intern_id(s->syms, name))) == DL_PROVED;
}

static long value_of(scene *s, const char *name, long dflt)
{
    long v = 0;
    if (!world_get_value(s->w, intern_id(s->syms, name), &v)) return dflt;
    return v;
}

/* Enumerate a judgment's proved ground instances by crossing its argument
 * domains — which is what the §6.3 artifact is FOR: a client cannot ask about
 * a predicate it cannot spell, and it cannot spell one without knowing the
 * domains. Arity 1 and 2 cover the blessed vocabulary. */
static int enumerate(scene *s, const char *name, pair *out, int cap)
{
    int arity = iface_judgment_arity(s->f, name);
    if (arity < 1 || arity > 2) return 0;
    const char *d0 = iface_judgment_arg(s->f, name, 0);
    int n0 = iface_domain_size(s->f, d0), n = 0;
    char atom[S_MAXTERM];
    if (arity == 1) {
        for (int i = 0; i < n0 && n < cap; i++) {
            const char *a = iface_domain_item(s->f, d0, i);
            snprintf(atom, sizeof atom, "%s(%s)", name, a);
            if (!proved_atom(s, atom)) continue;
            snprintf(out[n].a, S_MAXNAME, "%s", a);
            out[n].b[0] = 0;
            n++;
        }
        return n;
    }
    const char *d1 = iface_judgment_arg(s->f, name, 1);
    int n1 = iface_domain_size(s->f, d1);
    for (int i = 0; i < n0 && n < cap; i++)
        for (int j = 0; j < n1 && n < cap; j++) {
            const char *a = iface_domain_item(s->f, d0, i);
            const char *b = iface_domain_item(s->f, d1, j);
            snprintf(atom, sizeof atom, "%s(%s,%s)", name, a, b);
            if (!proved_atom(s, atom)) continue;
            snprintf(out[n].a, S_MAXNAME, "%s", a);
            snprintf(out[n].b, S_MAXNAME, "%s", b);
            n++;
        }
    return n;
}

int scene_pairs(scene *s, const char *judgment, scene_pair *out, int cap)
{
    pair buf[S_MAXPAIRS];
    int n = enumerate(s, judgment, buf, S_MAXPAIRS);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].a, sizeof out[i].a, "%s", buf[i].a);
        snprintf(out[i].b, sizeof out[i].b, "%s", buf[i].b);
    }
    return n;
}

/* ---- the menu ---------------------------------------------------------------- */

/* A blocked row is worth SHOWING when its refusal is an argument rather than an
 * absence. One blocker means everything holds but one thing — the row is one
 * step away — and a blocker that is a JUDGMENT means there is a `why` worth
 * reading, where a base fact ("you are not there") is merely absent and has no
 * trace to print. Both halves come from the artifact, so no story declares
 * which of its guards are interesting. */
static bool offerable(scene *s, uint32_t action, dl_lit *out)
{
    dl_lit blockers[8];
    int n = world_action_blockers(s->w, action, blockers, 8);
    if (n != 1) return false;
    char pred[S_MAXNAME];
    pred_of(intern_name(s->syms, blockers[0].atom), pred, sizeof pred);
    if (!iface_is_judgment(s->f, pred)) return false;
    *out = blockers[0];
    return true;
}

const char *scene_pick_action(scene *s, const char *entity)
{
    static char term[S_MAXTERM];
    uint32_t acts[S_MAXACTIONS];
    int n = world_actions(s->w, acts, S_MAXACTIONS);
    for (int i = 0; i < n; i++) {
        const char *name = intern_name(s->syms, acts[i]);
        if (strchr(name, '(')) continue;                 /* nullary only */
        if (world_action_status_of(s->w, acts[i]) != WORLD_ACTION_APPLIES) continue;
        /* A NAMING CONVENTION standing in for a language feature, and it should
         * die when scopes arrive. Written out rather than inferred, because
         * inferring it ("any applicable action with one entity argument")
         * swallowed `go_hall(hero)` and hid half the cellar's menu. */
        static const char *const CLICK[] = { "pick_", "aim_" };
        for (int k = 0; k < 2; k++) {
            char want[S_MAXTERM];
            snprintf(want, sizeof want, "%s%s", CLICK[k], entity);
            if (strcmp(name, want) == 0) {
                snprintf(term, sizeof term, "%s", name);
                return term;
            }
        }
    }
    return NULL;
}

static bool is_clickable(const char *term)
{
    if (strchr(term, '(')) return false;
    return strncmp(term, "pick_", 5) == 0 || strncmp(term, "aim_", 4) == 0;
}

/* WHICH ROWS BELONG TO THIS MENU. The rule is about SORT, not position: hide a
 * row that names something of the SUBJECT'S OWN KIND which is neither the
 * subject nor the object. `go_hall(guard)` is the guard's business,
 * `strike(bolt_a, imp)` is aimed at someone you are not aiming at, and an
 * action naming nothing of that kind (`end_turn`) is nobody's and everybody's,
 * so it is always offered. */
static bool mine(scene *s, const char *term, const char *kin)
{
    if (!kin) return false;
    char argv[8][S_MAXNAME];
    int na = args_of(term, argv, 8);
    for (int i = 0; i < na; i++) {
        const char *sort = iface_sort_of(s->f, argv[i]);
        if (!sort || strcmp(sort, kin) != 0) continue;
        bool is_picked = s->has_picked && strcmp(argv[i], s->picked) == 0;
        bool is_aimed = s->has_aimed && strcmp(argv[i], s->aimed) == 0;
        if (!is_picked && !is_aimed) return false;
    }
    return true;
}

static void build_menu(scene *s)
{
    s->nmenu = 0;
    if (!s->has_picked) return;
    const char *kin = iface_sort_of(s->f, s->picked);

    uint32_t acts[S_MAXACTIONS];
    int n = world_actions(s->w, acts, S_MAXACTIONS);

    /* A verb grounds once per binding, so `drop_torch` is three ground actions
     * for three rooms and a flat list shows DROP TORCH three times. Every
     * APPLICABLE instance is a real choice and all of them are listed; a verb
     * with no applicable instance contributes at most ONE refused row, because
     * the reason you cannot drop the torch is not three different reasons. */
    char verbs[S_MAXMENU][S_MAXNAME];
    int nverbs = 0;
    for (int i = 0; i < n && nverbs < S_MAXMENU; i++) {
        const char *term = intern_name(s->syms, acts[i]);
        if (is_clickable(term) || !mine(s, term, kin)) continue;
        char v[S_MAXNAME];
        verb_of(term, v, sizeof v);
        bool seen = false;
        for (int k = 0; k < nverbs; k++) if (strcmp(verbs[k], v) == 0) seen = true;
        if (!seen) snprintf(verbs[nverbs++], S_MAXNAME, "%s", v);
    }

    for (int vi = 0; vi < nverbs && s->nmenu < S_MAXMENU; vi++) {
        int before = s->nmenu;
        for (int i = 0; i < n && s->nmenu < S_MAXMENU; i++) {
            const char *term = intern_name(s->syms, acts[i]);
            char v[S_MAXNAME];
            verb_of(term, v, sizeof v);
            if (strcmp(v, verbs[vi]) != 0) continue;
            if (is_clickable(term) || !mine(s, term, kin)) continue;
            if (world_action_status_of(s->w, acts[i]) != WORLD_ACTION_APPLIES) continue;
            menu_row *r = &s->menu[s->nmenu++];
            memset(r, 0, sizeof *r);
            snprintf(r->term, sizeof r->term, "%s", term);
            r->ok = true;
        }
        if (s->nmenu > before) continue;                  /* had applicable rows */
        for (int i = 0; i < n && s->nmenu < S_MAXMENU; i++) {
            const char *term = intern_name(s->syms, acts[i]);
            char v[S_MAXNAME];
            verb_of(term, v, sizeof v);
            if (strcmp(v, verbs[vi]) != 0) continue;
            if (is_clickable(term) || !mine(s, term, kin)) continue;
            dl_lit b;
            if (!offerable(s, acts[i], &b)) continue;
            menu_row *r = &s->menu[s->nmenu++];
            memset(r, 0, sizeof *r);
            snprintf(r->term, sizeof r->term, "%s", term);
            r->ok = false;
            r->blocker = b;
            r->has_blocker = true;
            break;                                        /* at most one */
        }
    }

    /* Where one verb DOES leave several rows, the label has to say which is
     * which — but only by what actually DIFFERS between them. Appending every
     * argument gives "STRIKE EDGE A GNOLL" when the target is the same in
     * both, and the noise is the part a player has to read past. */
    for (int i = 0; i < s->nmenu; i++) {
        menu_row *r = &s->menu[i];
        char v[S_MAXNAME];
        verb_of(r->term, v, sizeof v);
        char mine_args[8][S_MAXNAME];
        int na = args_of(r->term, mine_args, 8);
        char label[S_MAXNAME];
        scene_say(v, label, sizeof label);
        for (int k = 0; k < na; k++) {
            bool differs = false;
            for (int j = 0; j < s->nmenu; j++) {
                if (j == i) continue;
                char pv[S_MAXNAME];
                verb_of(s->menu[j].term, pv, sizeof pv);
                if (strcmp(pv, v) != 0) continue;
                char peer[8][S_MAXNAME];
                int pn = args_of(s->menu[j].term, peer, 8);
                if (k < pn && strcmp(peer[k], mine_args[k]) != 0) differs = true;
            }
            if (!differs) continue;
            char word[S_MAXNAME];
            scene_say(mine_args[k], word, sizeof word);
            size_t used = strlen(label);
            snprintf(label + used, sizeof label - used, " %s", word);
        }
        snprintf(r->label, sizeof r->label, "%s", label);
    }

    /* the menu's own geometry is the story's, like everything else here */
    int mx = (int)value_of(s, "ax(a_menu)", 8), my = (int)value_of(s, "ay(a_menu)", 244);
    int mw = (int)value_of(s, "aw(a_menu)", 176), mh = (int)value_of(s, "ah(a_menu)", 12);
    for (int i = 0; i < s->nmenu; i++) {
        s->menu[i].x = mx;
        s->menu[i].y = my + i * (mh + 3);
        s->menu[i].w = mw;
        s->menu[i].h = mh;
    }
}

/* ---- the model --------------------------------------------------------------- */

scene *scene_new(world *w, intern *syms, const iface *f, plat *p)
{
    scene *s = calloc(1, sizeof *s);
    s->w = w; s->syms = syms; s->f = f; s->p = p;
    return s;
}

void scene_free(scene *s) { free(s); }

void scene_rebuild(scene *s)
{
    s->npanels   = enumerate(s, "panel",    s->panels,   S_MAXPANELS);
    s->ncaptions = enumerate(s, "caption",  s->captions, S_MAXCAPTIONS);
    s->ngauges   = enumerate(s, "gauge",    s->gauges,   S_MAXGAUGES);
    s->nheld     = enumerate(s, "held",     s->held,     S_MAXHELD);
    s->nshows    = enumerate(s, "shows",    s->shows,    S_MAXSHOWS);

    pair tmp[S_MAXSHADED];
    int n = enumerate(s, "shaded", tmp, S_MAXSHADED);
    s->nshaded = n;
    for (int i = 0; i < n; i++) snprintf(s->shaded[i], S_MAXNAME, "%s", tmp[i].a);

    /* occupants of a region, in declaration order — the layout built-in the
     * story is spared: "which slot" is ordinal reasoning, and asking rules to
     * do it is the failure mode §12 names. */
    s->nslots = 0;
    pair occ[S_MAXSLOTS];
    int na = enumerate(s, "in_anchor", occ, S_MAXSLOTS);
    for (int i = 0; i < na && s->nslots < S_MAXSLOTS; i++) {
        occupant *o = &s->slots[s->nslots++];
        memset(o, 0, sizeof *o);
        snprintf(o->e, S_MAXNAME, "%s", occ[i].a);
        snprintf(o->anchor, S_MAXNAME, "%s", occ[i].b);
    }
    int np = enumerate(s, "prop_in", occ, S_MAXSLOTS);
    for (int i = 0; i < np && s->nslots < S_MAXSLOTS; i++) {
        occupant *o = &s->slots[s->nslots++];
        memset(o, 0, sizeof *o);
        snprintf(o->e, S_MAXNAME, "%s", occ[i].a);
        snprintf(o->anchor, S_MAXNAME, "%s", occ[i].b);
    }

    pair one[4];
    s->has_picked = enumerate(s, "picked", one, 4) > 0;
    if (s->has_picked) snprintf(s->picked, S_MAXNAME, "%s", one[0].a);
    s->has_aimed = enumerate(s, "aimed", one, 4) > 0;
    if (s->has_aimed) snprintf(s->aimed, S_MAXNAME, "%s", one[0].a);

    build_menu(s);
}

/* ---- drawing ------------------------------------------------------------------ */

typedef struct { int x, y, w, h; } box;

static box box_of(scene *s, const char *anchor)
{
    char n[S_MAXTERM];
    box b;
    snprintf(n, sizeof n, "ax(%s)", anchor); b.x = (int)value_of(s, n, 0);
    snprintf(n, sizeof n, "ay(%s)", anchor); b.y = (int)value_of(s, n, 0);
    snprintf(n, sizeof n, "aw(%s)", anchor); b.w = (int)value_of(s, n, 0);
    snprintf(n, sizeof n, "ah(%s)", anchor); b.h = (int)value_of(s, n, 0);
    return b;
}

/* the frozen look of each declared style */
static void draw_style(scene *s, const char *style, box b)
{
    if (strcmp(style, "st_title") == 0)      plat_rectfill(s->p, b.x, b.y, b.w, b.h, 2);
    else if (strcmp(style, "st_room") == 0) {
        plat_rectfill(s->p, b.x, b.y, b.w, b.h, 1);
        plat_rect(s->p, b.x - 1, b.y - 1, b.w + 2, b.h + 2, 3);
    }
    else if (strcmp(style, "st_bar") == 0)   plat_line(s->p, b.x, b.y, b.x + b.w - 1, b.y, 2);
    else if (strcmp(style, "st_button") == 0)     plat_rect(s->p, b.x, b.y, b.w, b.h, 3);
    else if (strcmp(style, "st_button_off") == 0) plat_rect(s->p, b.x, b.y, b.w, b.h, 2);
}

static int sprite_of(scene *s, const char *e)
{
    for (int i = 0; i < s->nshows; i++)
        if (strcmp(s->shows[i].a, e) == 0)
            return iface_enum_index(s->f, "sprite", s->shows[i].b);
    return -1;
}

void scene_draw(scene *s, const char *sheet, const char *why)
{
    char label[S_MAXNAME];
    plat_cls(s->p, 0);

    for (int i = 0; i < s->npanels; i++)
        draw_style(s, s->panels[i].b, box_of(s, s->panels[i].a));

    /* occupants: a region packs its contents into a row of cells */
    for (int i = 0; i < s->nslots; i++) {
        occupant *o = &s->slots[i];
        int k = 0;
        for (int j = 0; j < i; j++) if (strcmp(s->slots[j].anchor, o->anchor) == 0) k++;
        box b = box_of(s, o->anchor);
        int x = b.x + 24 + (k % 4) * 40, y = b.y + 28 + (k / 4) * 44;
        int idx = sprite_of(s, o->e);
        plat_spr_opts opts = { false, false, 1.0f };
        if (idx >= 0) plat_spr(s->p, sheet, idx, x, y, opts);
        if (s->has_picked && strcmp(o->e, s->picked) == 0)
            plat_rect(s->p, x - 3, y - 3, 22, 22, 6);
        else if (s->has_aimed && strcmp(o->e, s->aimed) == 0)
            plat_rect(s->p, x - 3, y - 3, 22, 22, 10);
        o->x = x; o->y = y; o->drawn = true;

        /* whatever this occupant carries rides beside it */
        int carried = 0;
        for (int j = 0; j < s->nheld; j++) if (strcmp(s->held[j].b, o->e) == 0) carried++;
        int c = 0;
        for (int j = 0; j < s->nheld; j++) {
            if (strcmp(s->held[j].b, o->e) != 0) continue;
            int si = sprite_of(s, s->held[j].a);
            plat_spr_opts held_opts = { false, false, 0.9f };
            if (si >= 0)
                plat_spr(s->p, sheet, si, x + 8 - carried * 7 + c * 14, y + 20, held_opts);
            c++;
        }
    }

    for (int i = 0; i < s->nshaded; i++) {
        box b = box_of(s, s->shaded[i]);
        char fog[S_MAXNAME];
        snprintf(fog, sizeof fog, "%s_fog", sheet);
        for (int ty = 0; ty * 8 < b.h; ty++)
            for (int tx = 0; tx * 8 < b.w; tx++)
                plat_shade(s->p, fog, 0, b.x + tx * 8, b.y + ty * 8);
    }

    for (int i = 0; i < s->ncaptions; i++) {
        box b = box_of(s, s->captions[i].a);
        /* a caption on a region sits above it; one on a bar sits inside it */
        bool inside = b.h >= 12;
        scene_say(s->captions[i].b, label, sizeof label);
        plat_print(s->p, label, b.x + (inside ? 8 : 0), inside ? b.y + 3 : b.y - 11,
                   inside ? 5 : 3, true);
    }

    for (int i = 0; i < s->ngauges; i++) {
        box b = box_of(s, s->gauges[i].a);
        const char *who = s->gauges[i].b;
        /* The numbers are the STORY's: `gauge_value`/`gauge_max` are derived
         * values it defines and the engine evaluates, so nothing here knows
         * what a bar is measuring — and the colour is a judgment, not a
         * threshold frozen in this file. */
        char n[S_MAXTERM];
        snprintf(n, sizeof n, "gauge_value(%s)", who);
        long v = value_of(s, n, 0);
        snprintf(n, sizeof n, "gauge_max(%s)", who);
        long max = value_of(s, n, 1);
        if (max <= 0) max = 1;
        snprintf(n, sizeof n, "gauge_low(%s)", who);
        bool low = proved_atom(s, n);
        int y = b.y + i * 11;
        scene_say(who, label, sizeof label);
        plat_print(s->p, label, b.x, y,
                   (s->has_picked && strcmp(who, s->picked) == 0) ? 5 : 3, true);
        plat_rectfill(s->p, b.x + 44, y + 1, 48, 6, 2);
        int fill = (int)((48 * v + max / 2) / max);
        if (fill < 0) fill = 0;
        plat_rectfill(s->p, b.x + 44, y + 1, fill, 6, low ? 10 : 13);
        char num[16];
        snprintf(num, sizeof num, "%ld", v);
        plat_print(s->p, num, b.x + 98, y, 3, true);
    }

    for (int i = 0; i < s->nmenu; i++) {
        menu_row *r = &s->menu[i];
        box b = { r->x, r->y, r->w, r->h };
        draw_style(s, r->ok ? "st_button" : "st_button_off", b);
        plat_print(s->p, r->label, r->x + 4, r->y + 2, r->ok ? 5 : 2, true);
    }

    if (why && *why) {
        plat_rectfill(s->p, 4, 18, 632, 198, 1);
        plat_rect(s->p, 4, 18, 632, 198, 2);
        int ly = 22;
        const char *line = why;
        while (*line && ly <= 210) {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            char buf[160];
            if (len > 155) len = 155;
            memcpy(buf, line, len);
            buf[len] = 0;
            bool blank = true;
            for (size_t k = 0; k < len; k++) if (!isspace((unsigned char)buf[k])) blank = false;
            if (!blank) {
                plat_print(s->p, buf, 8, ly, strstr(buf, "-- applicable") ? 10 : 4, false);
                ly += 8;
            }
            if (!nl) break;
            line = nl + 1;
        }
    }
}

/* ---- input -------------------------------------------------------------------- */

int scene_targets(scene *s, plat_target *out, int cap)
{
    int n = 0;
    s->nids = 0;
    for (int i = 0; i < s->nmenu && n < cap; i++) {
        snprintf(s->ids[s->nids], sizeof s->ids[0], "cmd:%s", s->menu[i].term);
        out[n].id = s->ids[s->nids++];
        out[n].x = s->menu[i].x; out[n].y = s->menu[i].y;
        out[n].w = s->menu[i].w; out[n].h = s->menu[i].h;
        n++;
    }
    for (int i = 0; i < s->nslots && n < cap; i++) {
        if (!s->slots[i].drawn) continue;
        snprintf(s->ids[s->nids], sizeof s->ids[0], "ent:%s", s->slots[i].e);
        out[n].id = s->ids[s->nids++];
        out[n].x = s->slots[i].x - 3; out[n].y = s->slots[i].y - 3;
        out[n].w = 22; out[n].h = 22;
        n++;
    }
    return n;
}

static bool cmd_hit(scene *s, int i, scene_hit *out)
{
    out->kind = SCENE_CMD;
    out->term = s->menu[i].term;
    out->ok = s->menu[i].ok;
    out->blocker = s->menu[i].blocker;
    out->has_blocker = s->menu[i].has_blocker;
    out->entity = NULL;
    return true;
}

bool scene_target(scene *s, const char *id, scene_hit *out)
{
    memset(out, 0, sizeof *out);
    out->kind = SCENE_NOTHING;
    if (!id) return false;
    if (strncmp(id, "cmd:", 4) == 0) {
        for (int i = 0; i < s->nmenu; i++)
            if (strcmp(s->menu[i].term, id + 4) == 0) return cmd_hit(s, i, out);
        return false;
    }
    if (strncmp(id, "ent:", 4) == 0) {
        for (int i = 0; i < s->nslots; i++)
            if (strcmp(s->slots[i].e, id + 4) == 0) {
                out->kind = SCENE_ENTITY;
                out->entity = s->slots[i].e;
                return true;
            }
    }
    return false;
}

bool scene_hit_at(scene *s, int x, int y, scene_hit *out)
{
    memset(out, 0, sizeof *out);
    out->kind = SCENE_NOTHING;
    for (int i = 0; i < s->nmenu; i++) {
        menu_row *r = &s->menu[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return cmd_hit(s, i, out);
    }
    for (int i = 0; i < s->nslots; i++) {
        occupant *o = &s->slots[i];
        if (!o->drawn) continue;
        if (x >= o->x - 3 && x < o->x + 19 && y >= o->y - 3 && y < o->y + 19) {
            out->kind = SCENE_ENTITY;
            out->entity = o->e;
            return true;
        }
    }
    return false;
}

int         scene_menu_count(const scene *s) { return s->nmenu; }
const char *scene_menu_label(const scene *s, int i)
{
    return (i >= 0 && i < s->nmenu) ? s->menu[i].label : NULL;
}
const char *scene_menu_term(const scene *s, int i)
{
    return (i >= 0 && i < s->nmenu) ? s->menu[i].term : NULL;
}
bool scene_menu_ok(const scene *s, int i)
{
    return (i >= 0 && i < s->nmenu) && s->menu[i].ok;
}
