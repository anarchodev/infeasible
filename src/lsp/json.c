#include "lsp/json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ parse */

typedef struct {
    const char *p, *end;
    arena      *a;
    bool        ok;
} jp;

static void skip_ws(jp *j)
{
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

static char peek(jp *j) { return j->p < j->end ? *j->p : '\0'; }

static json *jnew(jp *j, json_kind k)
{
    json *v = arena_alloc(j->a, sizeof *v);
    v->kind = k;
    return v;
}

static bool parse_hex4(jp *j, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (j->p >= j->end) return false;
        char c = *j->p++;
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

static void utf8_emit(char **w, unsigned cp)
{
    if (cp < 0x80) {
        *(*w)++ = (char)cp;
    } else if (cp < 0x800) {
        *(*w)++ = (char)(0xC0 | (cp >> 6));
        *(*w)++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *(*w)++ = (char)(0xE0 | (cp >> 12));
        *(*w)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*w)++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *(*w)++ = (char)(0xF0 | (cp >> 18));
        *(*w)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *(*w)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*w)++ = (char)(0x80 | (cp & 0x3F));
    }
}

/* Decode a "…" string starting at peek()=='"'. The raw span between quotes is
 * an upper bound on the decoded byte length (every escape shrinks), so one
 * arena buffer of that size + a NUL always fits. */
static const char *jstring(jp *j, size_t *outlen)
{
    j->p++;  /* opening quote */
    size_t raw = 0;
    for (const char *q = j->p; q < j->end && *q != '"'; q++, raw++) {
        if (*q == '\\') { q++; if (q >= j->end) { j->ok = false; return NULL; } }
    }
    char *buf = arena_alloc(j->a, raw + 1);
    char *w   = buf;

    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { *w++ = c; continue; }
        if (j->p >= j->end) { j->ok = false; return NULL; }
        char e = *j->p++;
        switch (e) {
            case '"':  *w++ = '"';  break;
            case '\\': *w++ = '\\'; break;
            case '/':  *w++ = '/';  break;
            case 'b':  *w++ = '\b'; break;
            case 'f':  *w++ = '\f'; break;
            case 'n':  *w++ = '\n'; break;
            case 'r':  *w++ = '\r'; break;
            case 't':  *w++ = '\t'; break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(j, &cp)) { j->ok = false; return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    j->p + 1 < j->end && j->p[0] == '\\' && j->p[1] == 'u') {
                    j->p += 2;
                    unsigned lo;
                    if (!parse_hex4(j, &lo)) { j->ok = false; return NULL; }
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { utf8_emit(&w, cp); cp = lo; }
                }
                utf8_emit(&w, cp);
                break;
            }
            default: j->ok = false; return NULL;
        }
    }
    if (j->p >= j->end) { j->ok = false; return NULL; }
    j->p++;  /* closing quote */
    *w = '\0';
    if (outlen) *outlen = (size_t)(w - buf);
    return buf;
}

static json *jvalue(jp *j);

static json *jarray(jp *j)
{
    j->p++;  /* [ */
    json *v = jnew(j, JSON_ARR);
    struct node { json *val; struct node *next; } *head = NULL, *tail = NULL;
    size_t count = 0;
    skip_ws(j);
    if (peek(j) != ']') {
        for (;;) {
            json *el = jvalue(j);
            if (!j->ok) return v;
            struct node *nd = arena_alloc(j->a, sizeof *nd);
            nd->val = el;
            if (tail) tail->next = nd; else head = nd;
            tail = nd;
            count++;
            skip_ws(j);
            if (peek(j) == ',') { j->p++; skip_ws(j); continue; }
            break;
        }
    }
    if (peek(j) != ']') { j->ok = false; return v; }
    j->p++;
    v->as.arr.items = arena_alloc(j->a, (count ? count : 1) * sizeof(json *));
    v->as.arr.len   = count;
    size_t i = 0;
    for (struct node *n = head; n; n = n->next) v->as.arr.items[i++] = n->val;
    return v;
}

static json *jobject(jp *j)
{
    j->p++;  /* { */
    json *v = jnew(j, JSON_OBJ);
    struct node { json_member mem; struct node *next; } *head = NULL, *tail = NULL;
    size_t count = 0;
    skip_ws(j);
    if (peek(j) != '}') {
        for (;;) {
            skip_ws(j);
            if (peek(j) != '"') { j->ok = false; return v; }
            size_t klen;
            const char *key = jstring(j, &klen);
            if (!j->ok) return v;
            skip_ws(j);
            if (peek(j) != ':') { j->ok = false; return v; }
            j->p++;
            json *val = jvalue(j);
            if (!j->ok) return v;
            struct node *nd = arena_alloc(j->a, sizeof *nd);
            nd->mem.key = key;
            nd->mem.keylen = klen;
            nd->mem.val = val;
            if (tail) tail->next = nd; else head = nd;
            tail = nd;
            count++;
            skip_ws(j);
            if (peek(j) == ',') { j->p++; continue; }
            break;
        }
    }
    skip_ws(j);
    if (peek(j) != '}') { j->ok = false; return v; }
    j->p++;
    v->as.obj.m   = arena_alloc(j->a, (count ? count : 1) * sizeof(json_member));
    v->as.obj.len = count;
    size_t i = 0;
    for (struct node *n = head; n; n = n->next) v->as.obj.m[i++] = n->mem;
    return v;
}

static bool lit(jp *j, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return false;
    j->p += n;
    return true;
}

static json *jvalue(jp *j)
{
    skip_ws(j);
    char c = peek(j);
    switch (c) {
        case '"': {
            json *v = jnew(j, JSON_STR);
            v->as.str.s = jstring(j, &v->as.str.len);
            return v;
        }
        case '{': return jobject(j);
        case '[': return jarray(j);
        case 't': {
            json *v = jnew(j, JSON_BOOL);
            v->as.b = true;
            if (!lit(j, "true")) j->ok = false;
            return v;
        }
        case 'f': {
            json *v = jnew(j, JSON_BOOL);
            v->as.b = false;
            if (!lit(j, "false")) j->ok = false;
            return v;
        }
        case 'n': {
            json *v = jnew(j, JSON_NULL);
            if (!lit(j, "null")) j->ok = false;
            return v;
        }
        default: {
            if (c == '-' || (c >= '0' && c <= '9')) {
                char *endp;
                json *v = jnew(j, JSON_NUM);
                v->as.num = strtod(j->p, &endp);
                if (endp == j->p) { j->ok = false; return v; }
                j->p = endp;
                return v;
            }
            j->ok = false;
            return jnew(j, JSON_NULL);
        }
    }
}

json *json_parse(arena *a, const char *src, size_t len)
{
    jp j = { src, src + len, a, true };
    json *v = jvalue(&j);
    return j.ok ? v : NULL;
}

/* --------------------------------------------------------------- accessors */

const json *json_get(const json *v, const char *key)
{
    if (!v || v->kind != JSON_OBJ) return NULL;
    for (size_t i = 0; i < v->as.obj.len; i++)
        if (strcmp(v->as.obj.m[i].key, key) == 0) return v->as.obj.m[i].val;
    return NULL;
}

bool json_is(const json *v, json_kind k) { return v && v->kind == k; }

const char *json_str(const json *v)
{
    return (v && v->kind == JSON_STR) ? v->as.str.s : NULL;
}

double json_num(const json *v, double dflt)
{
    return (v && v->kind == JSON_NUM) ? v->as.num : dflt;
}

long json_int(const json *v, long dflt)
{
    return (v && v->kind == JSON_NUM) ? (long)v->as.num : dflt;
}

bool json_bool(const json *v, bool dflt)
{
    return (v && v->kind == JSON_BOOL) ? v->as.b : dflt;
}

size_t json_arr_len(const json *v)
{
    return (v && v->kind == JSON_ARR) ? v->as.arr.len : 0;
}

const json *json_arr_at(const json *v, size_t i)
{
    if (!v || v->kind != JSON_ARR || i >= v->as.arr.len) return NULL;
    return v->as.arr.items[i];
}

/* ------------------------------------------------------------------ strbuf */

static void sb_grow(strbuf *sb, size_t need)
{
    if (sb->len + need + 1 <= sb->cap) return;
    size_t cap = sb->cap ? sb->cap : 128;
    while (cap < sb->len + need + 1) cap *= 2;
    sb->buf = realloc(sb->buf, cap);
    sb->cap = cap;
}

void sb_init(strbuf *sb) { sb->buf = NULL; sb->len = 0; sb->cap = 0; }
void sb_free(strbuf *sb) { free(sb->buf); sb->buf = NULL; sb->len = sb->cap = 0; }
void sb_reset(strbuf *sb) { sb->len = 0; if (sb->buf) sb->buf[0] = '\0'; }

void sb_rawn(strbuf *sb, const char *s, size_t n)
{
    sb_grow(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_raw(strbuf *sb, const char *s) { sb_rawn(sb, s, strlen(s)); }

void sb_char(strbuf *sb, char c) { sb_rawn(sb, &c, 1); }

void sb_int(strbuf *sb, long v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%ld", v);
    if (n > 0) sb_rawn(sb, tmp, (size_t)n);
}

void sb_jstrn(strbuf *sb, const char *s, size_t n)
{
    sb_char(sb, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_rawn(sb, "\\\"", 2); break;
            case '\\': sb_rawn(sb, "\\\\", 2); break;
            case '\b': sb_rawn(sb, "\\b", 2);  break;
            case '\f': sb_rawn(sb, "\\f", 2);  break;
            case '\n': sb_rawn(sb, "\\n", 2);  break;
            case '\r': sb_rawn(sb, "\\r", 2);  break;
            case '\t': sb_rawn(sb, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char u[8];
                    snprintf(u, sizeof u, "\\u%04x", c);
                    sb_rawn(sb, u, 6);
                } else {
                    sb_char(sb, (char)c);   /* UTF-8 bytes pass through */
                }
        }
    }
    sb_char(sb, '"');
}

void sb_jstr(strbuf *sb, const char *s) { sb_jstrn(sb, s, strlen(s)); }
