#ifndef INF_LSP_JSON_H
#define INF_LSP_JSON_H

#include <stddef.h>
#include <stdbool.h>

#include "core/arena.h"

/* Minimal JSON for the .story language server (DESIGN.md §6.1 item 7): a
 * parser into an arena-allocated value tree (incoming JSON-RPC request bodies)
 * and a growable string builder that escapes as it appends (outgoing bodies).
 *
 * Enough of RFC 8259 for JSON-RPC 2.0 traffic — objects, arrays, strings (with
 * the standard escapes and \uXXXX incl. surrogate pairs, decoded to UTF-8),
 * numbers, true/false/null. Not a general-purpose library: numbers are held as
 * double, duplicate object keys resolve to the first seen. Parsing is bounded
 * by an explicit length, so bodies need not be NUL-terminated. */

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ
} json_kind;

typedef struct json        json;
typedef struct json_member json_member;

struct json {
    json_kind kind;
    union {
        bool   b;
        double num;
        struct { const char *s; size_t len; } str;   /* decoded, NUL-terminated */
        struct { json **items; size_t len; } arr;
        struct { json_member *m; size_t len; } obj;
    } as;
};

struct json_member {
    const char *key;      /* decoded, NUL-terminated */
    size_t      keylen;
    json       *val;
};

/* Parse `len` bytes of `src` into `a`. Returns NULL on malformed input. */
json *json_parse(arena *a, const char *src, size_t len);

/* Object field by NUL-terminated key; NULL if `v` is not an object or absent. */
const json *json_get(const json *v, const char *key);

/* Typed accessors, tolerant of NULL / kind mismatch (fall back to the given
 * default, or NULL). `json_str` returns the decoded NUL-terminated bytes. */
bool        json_is(const json *v, json_kind k);
const char *json_str(const json *v);
double      json_num(const json *v, double dflt);
long        json_int(const json *v, long dflt);
bool        json_bool(const json *v, bool dflt);
size_t      json_arr_len(const json *v);
const json *json_arr_at(const json *v, size_t i);

/* ---- output: a growable, self-escaping string builder ---- */

typedef struct {
    char  *buf;      /* always NUL-terminated at [len]; owns heap */
    size_t len, cap;
} strbuf;

void sb_init(strbuf *sb);
void sb_free(strbuf *sb);
void sb_reset(strbuf *sb);
void sb_raw(strbuf *sb, const char *s);              /* append verbatim */
void sb_rawn(strbuf *sb, const char *s, size_t n);
void sb_char(strbuf *sb, char c);
void sb_int(strbuf *sb, long v);
void sb_jstr(strbuf *sb, const char *s);             /* append "…" JSON-escaped */
void sb_jstrn(strbuf *sb, const char *s, size_t n);

#endif
