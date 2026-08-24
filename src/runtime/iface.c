#include "runtime/iface.h"

#include "core/arena.h"
#include "lsp/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IF_MAXNAME = 64 };

typedef struct {
    char         name[IF_MAXNAME];
    bool         is_union;
    const char **item;                 /* entities (sort/cover) or values (enum) */
    int          nitem;
} if_domain;

typedef struct {
    char         name[IF_MAXNAME];
    const char  *arg[8];
    int          narg;
} if_pred;

struct iface {
    arena      a;                      /* owns every string the JSON tree holds */
    json      *root;
    if_domain *dom;
    int        ndom;
    if_pred   *judg;
    int        njudg;
    const char **value;
    int        nvalue;
    const char *story;
};

static void put_name(char *dst, const char *src)
{
    snprintf(dst, IF_MAXNAME, "%s", src ? src : "");
}

/* Collect the string members of an array field into an arena-backed vector. */
static const char **strs(arena *a, const json *arr, int *n)
{
    size_t len = json_arr_len(arr);
    const char **out = arena_alloc(a, (len ? len : 1) * sizeof *out);
    int k = 0;
    for (size_t i = 0; i < len; i++) {
        const char *s = json_str(json_arr_at(arr, i));
        if (s) out[k++] = s;
    }
    *n = k;
    return out;
}

iface *iface_parse(const char *src, char *err, size_t errsz)
{
    if (!src) { if (err) snprintf(err, errsz, "no artifact"); return NULL; }
    arena tmp;
    arena_init(&tmp);
    json *root = json_parse(&tmp, src, strlen(src));
    if (!root) {
        if (err) snprintf(err, errsz, "the interface artifact is not JSON");
        arena_release(&tmp);
        return NULL;
    }
    const char *kind = json_str(json_get(root, "artifact"));
    if (!kind || strcmp(kind, "infeasible.interface") != 0) {
        if (err) snprintf(err, errsz, "not an interface artifact");
        arena_release(&tmp);
        return NULL;
    }
    if (json_int(json_get(root, "version"), 0) != 1) {
        if (err) snprintf(err, errsz,
                          "interface artifact version %ld, this runtime reads 1",
                          json_int(json_get(root, "version"), 0));
        arena_release(&tmp);
        return NULL;
    }

    iface *f = calloc(1, sizeof *f);
    f->a = tmp;
    arena *a = &f->a;
    f->root = root;
    f->story = json_str(json_get(root, "story"));

    const json *sorts = json_get(root, "sorts"), *enums = json_get(root, "enums");
    size_t ns = json_arr_len(sorts), ne = json_arr_len(enums);
    f->dom = calloc(ns + ne + 1, sizeof *f->dom);
    for (size_t i = 0; i < ns; i++) {
        const json *s = json_arr_at(sorts, i);
        if_domain *d = &f->dom[f->ndom++];
        put_name(d->name, json_str(json_get(s, "name")));
        const char *k = json_str(json_get(s, "kind"));
        d->is_union = k && strcmp(k, "union") == 0;
        d->item = strs(a, json_get(s, "entities"), &d->nitem);
    }
    for (size_t i = 0; i < ne; i++) {
        const json *e = json_arr_at(enums, i);
        if_domain *d = &f->dom[f->ndom++];
        put_name(d->name, json_str(json_get(e, "name")));
        d->item = strs(a, json_get(e, "values"), &d->nitem);
    }

    const json *js = json_get(root, "judgments");
    size_t nj = json_arr_len(js);
    f->judg = calloc(nj + 1, sizeof *f->judg);
    for (size_t i = 0; i < nj; i++) {
        const json *j = json_arr_at(js, i);
        if_pred *p = &f->judg[f->njudg++];
        put_name(p->name, json_str(json_get(j, "name")));
        const json *args = json_get(j, "args");
        size_t na = json_arr_len(args);
        for (size_t k = 0; k < na && p->narg < 8; k++)
            p->arg[p->narg++] = json_str(json_arr_at(args, k));
    }

    const json *vs = json_get(root, "values");
    size_t nv = json_arr_len(vs);
    f->value = calloc(nv + 1, sizeof *f->value);
    for (size_t i = 0; i < nv; i++)
        f->value[f->nvalue++] = json_str(json_get(json_arr_at(vs, i), "name"));

    return f;
}

void iface_free(iface *f)
{
    if (!f) return;
    arena_release(&f->a);
    free(f->dom);
    free(f->judg);
    free(f->value);
    free(f);
}

static const if_domain *find_dom(const iface *f, const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < f->ndom; i++)
        if (strcmp(f->dom[i].name, name) == 0) return &f->dom[i];
    return NULL;
}

int iface_domain_size(const iface *f, const char *name)
{
    const if_domain *d = find_dom(f, name);
    return d ? d->nitem : 0;
}

const char *iface_domain_item(const iface *f, const char *name, int i)
{
    const if_domain *d = find_dom(f, name);
    return (d && i >= 0 && i < d->nitem) ? d->item[i] : NULL;
}

bool iface_is_union(const iface *f, const char *name)
{
    const if_domain *d = find_dom(f, name);
    return d && d->is_union;
}

const char *iface_sort_of(const iface *f, const char *entity)
{
    if (!entity) return NULL;
    for (int i = 0; i < f->ndom; i++) {
        /* a cover lists its members' entities again under its own name, and
         * the sort of a thing is where it was DECLARED, never a set that
         * merely admits it */
        if (f->dom[i].is_union) continue;
        for (int k = 0; k < f->dom[i].nitem; k++)
            if (strcmp(f->dom[i].item[k], entity) == 0) return f->dom[i].name;
    }
    return NULL;
}

int iface_enum_index(const iface *f, const char *enum_name, const char *value)
{
    const if_domain *d = find_dom(f, enum_name);
    if (!d || !value) return -1;
    for (int i = 0; i < d->nitem; i++)
        if (strcmp(d->item[i], value) == 0) return i;
    return -1;
}

static const if_pred *find_judg(const iface *f, const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < f->njudg; i++)
        if (strcmp(f->judg[i].name, name) == 0) return &f->judg[i];
    return NULL;
}

int iface_judgment_arity(const iface *f, const char *name)
{
    const if_pred *p = find_judg(f, name);
    return p ? p->narg : -1;
}

const char *iface_judgment_arg(const iface *f, const char *name, int i)
{
    const if_pred *p = find_judg(f, name);
    return (p && i >= 0 && i < p->narg) ? p->arg[i] : NULL;
}

bool iface_is_judgment(const iface *f, const char *pred) { return find_judg(f, pred) != NULL; }

bool iface_has_value(const iface *f, const char *name)
{
    if (!name) return false;
    for (int i = 0; i < f->nvalue; i++)
        if (f->value[i] && strcmp(f->value[i], name) == 0) return true;
    return false;
}

const char *iface_story(const iface *f) { return f->story; }
