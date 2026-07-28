#include "lsp/lsp.h"
#include "lsp/json.h"

#include "core/arena.h"
#include "core/intern.h"
#include "lang/story.h"
#include "state/world.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* LSP DiagnosticSeverity */
#define LSP_SEV_ERROR   1
#define LSP_SEV_WARNING 2

/* JSON-RPC error codes */
#define RPC_METHOD_NOT_FOUND (-32601)

/* Local dup — the codebase avoids POSIX strdup to stay warning-clean under
 * strict C17 (mirrors dl_col.c's xstrdup). */
static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

typedef struct { char *uri; char *text; } lsp_doc;

struct lsp_server {
    lsp_doc *docs;
    size_t   ndocs, cap;
    bool     got_shutdown;   /* `shutdown` seen — a clean `exit` returns 0 */
};

lsp_server *lsp_new(void)
{
    lsp_server *s = calloc(1, sizeof *s);
    return s;
}

void lsp_free(lsp_server *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->ndocs; i++) {
        free(s->docs[i].uri);
        free(s->docs[i].text);
    }
    free(s->docs);
    free(s);
}

/* ------------------------------------------------------------ document store */

static lsp_doc *doc_find(lsp_server *s, const char *uri)
{
    for (size_t i = 0; i < s->ndocs; i++)
        if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    return NULL;
}

static void doc_put(lsp_server *s, const char *uri, const char *text)
{
    lsp_doc *d = doc_find(s, uri);
    if (d) {
        free(d->text);
        d->text = xstrdup(text);
        return;
    }
    if (s->ndocs == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->docs = realloc(s->docs, s->cap * sizeof *s->docs);
    }
    s->docs[s->ndocs].uri  = xstrdup(uri);
    s->docs[s->ndocs].text = xstrdup(text);
    s->ndocs++;
}

static void doc_remove(lsp_server *s, const char *uri)
{
    for (size_t i = 0; i < s->ndocs; i++) {
        if (strcmp(s->docs[i].uri, uri) == 0) {
            free(s->docs[i].uri);
            free(s->docs[i].text);
            s->docs[i] = s->docs[--s->ndocs];
            return;
        }
    }
}

/* --------------------------------------------------------- diagnostics bridge */

/* Byte offset of 1-based (line,col) within `text`, clamped to the buffer. */
static const char *at_pos(const char *text, int line, int col)
{
    const char *p = text;
    for (int l = 1; l < line && *p; p++)
        if (*p == '\n') l++;
    for (int c = 1; c < col && *p && *p != '\n'; c++) p++;
    return p;
}

/* Width of the identifier under a diagnostic's start, so the squiggle covers
 * the offending token rather than a single caret. Falls back to one column.
 * NOTE: columns are byte offsets; LSP characters are UTF-16 code units. Equal
 * for ASCII .story sources — the non-ASCII remap is deferred (a known LSP
 * subtlety, revisit alongside multibyte identifiers). */
static int token_width(const char *text, int line, int col)
{
    if (!text || line < 1 || col < 1) return 1;
    const char *p = at_pos(text, line, col);
    int n = 0;
    while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '_')) n++;
    return n > 0 ? n : 1;
}

static void emit_diagnostic(strbuf *sb, const story_diag *d, const char *text)
{
    int line0 = d->line > 0 ? d->line - 1 : 0;
    int col0  = d->col  > 0 ? d->col  - 1 : 0;
    int end   = col0 + token_width(text, d->line, d->col);

    sb_raw(sb, "{\"range\":{\"start\":{\"line\":");
    sb_int(sb, line0);
    sb_raw(sb, ",\"character\":");
    sb_int(sb, col0);
    sb_raw(sb, "},\"end\":{\"line\":");
    sb_int(sb, line0);
    sb_raw(sb, ",\"character\":");
    sb_int(sb, end);
    sb_raw(sb, "}},\"severity\":");
    sb_int(sb, d->sev == STORY_ERROR ? LSP_SEV_ERROR : LSP_SEV_WARNING);
    sb_raw(sb, ",\"source\":\"infeasible\",\"message\":");
    sb_jstr(sb, d->msg);
    sb_char(sb, '}');
}

/* Compile `text` and push a publishDiagnostics notification for `uri`. A NULL
 * text (closed document) publishes an empty list, clearing the editor. */
static void publish(lsp_server *s, const char *uri, const char *text,
                    lsp_emit_fn emit, void *ud)
{
    (void)s;
    strbuf sb;
    sb_init(&sb);
    sb_raw(&sb, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\""
                ",\"params\":{\"uri\":");
    sb_jstr(&sb, uri);
    sb_raw(&sb, ",\"diagnostics\":[");

    if (text) {
        story_diag di[256];
        story_diags diags = { di, (int)(sizeof di / sizeof di[0]), 0, 0 };
        intern *syms = intern_new();
        world *w = story_compile(text, "<lsp>", syms, &diags);

        int shown = diags.count < diags.cap ? diags.count : diags.cap;
        for (int i = 0; i < shown; i++) {
            if (i) sb_char(&sb, ',');
            emit_diagnostic(&sb, &di[i], text);
        }
        if (w) world_free(w);
        intern_free(syms);
    }

    sb_raw(&sb, "]}}");
    emit(ud, sb.buf, sb.len);
    sb_free(&sb);
}

/* ------------------------------------------------------------------ replies */

/* Serialize a request id verbatim (LSP ids are integer or string; null when
 * a request arrived without one). */
static void write_id(strbuf *sb, const json *id)
{
    if (json_is(id, JSON_NUM))      sb_int(sb, json_int(id, 0));
    else if (json_is(id, JSON_STR)) sb_jstr(sb, json_str(id));
    else                            sb_raw(sb, "null");
}

/* `result` is a raw JSON fragment (already serialized). */
static void reply_result(lsp_emit_fn emit, void *ud, const json *id,
                         const char *result)
{
    strbuf sb;
    sb_init(&sb);
    sb_raw(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&sb, id);
    sb_raw(&sb, ",\"result\":");
    sb_raw(&sb, result);
    sb_char(&sb, '}');
    emit(ud, sb.buf, sb.len);
    sb_free(&sb);
}

static void reply_error(lsp_emit_fn emit, void *ud, const json *id,
                        int code, const char *message)
{
    strbuf sb;
    sb_init(&sb);
    sb_raw(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(&sb, id);
    sb_raw(&sb, ",\"error\":{\"code\":");
    sb_int(&sb, code);
    sb_raw(&sb, ",\"message\":");
    sb_jstr(&sb, message);
    sb_raw(&sb, "}}");
    emit(ud, sb.buf, sb.len);
    sb_free(&sb);
}

/* Capabilities advertised on `initialize`: open/close notifications and full
 * document sync (change kind 1). Push diagnostics need no capability. */
static const char *INITIALIZE_RESULT =
    "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,\"change\":1}"
    ",\"definitionProvider\":true,\"referencesProvider\":true"
    ",\"documentSymbolProvider\":true}"
    ",\"serverInfo\":{\"name\":\"infeasible-story-lsp\",\"version\":\"0.1.0\"}}";

/* ---------------------------------------------------------------- navigation */

/* Compile `text` for its span model only (diagnostics dropped); the model
 * copies names, so `syms` is freed immediately. NULL on allocation failure.
 * TODO: cache per document — recompiling on every navigation request is fine
 * for editor-sized files but wasteful; invalidate on didChange. */
static story_model *model_for(const char *text)
{
    story_model *m = NULL;
    intern *syms = intern_new();
    world *w = story_compile_model(text, "<lsp>", syms, NULL, &m);
    if (w) world_free(w);
    intern_free(syms);
    return m;
}

/* The occurrence whose token covers 1-based (line, col), or NULL. */
static const story_occ *occ_at(const story_model *m, int line, int col)
{
    int n;
    const story_occ *occs = story_model_occs(m, &n);
    for (int i = 0; i < n; i++)
        if (occs[i].line == line &&
            col >= occs[i].col && col < occs[i].col + occs[i].len)
            return &occs[i];
    return NULL;
}

/* 1-based (line,col,len) -> an LSP Range (0-based, half-open on the end). */
static void write_range(strbuf *sb, int line, int col, int len)
{
    int l0 = line > 0 ? line - 1 : 0;
    int c0 = col  > 0 ? col  - 1 : 0;
    sb_raw(sb, "{\"start\":{\"line\":");
    sb_int(sb, l0);
    sb_raw(sb, ",\"character\":");
    sb_int(sb, c0);
    sb_raw(sb, "},\"end\":{\"line\":");
    sb_int(sb, l0);
    sb_raw(sb, ",\"character\":");
    sb_int(sb, c0 + len);
    sb_raw(sb, "}}");
}

static void write_location(strbuf *sb, const char *uri, const story_occ *o)
{
    sb_raw(sb, "{\"uri\":");
    sb_jstr(sb, uri);
    sb_raw(sb, ",\"range\":");
    write_range(sb, o->line, o->col, o->len);
    sb_char(sb, '}');
}

/* story_sym_kind -> LSP SymbolKind. */
static int symbol_kind(story_sym_kind k)
{
    switch (k) {
        case STORY_SYM_SORT:     return 5;   /* Class     */
        case STORY_SYM_DOMAIN:   return 5;   /* Class     */
        case STORY_SYM_ENTITY:   return 14;  /* Constant  */
        case STORY_SYM_FLUENT:   return 8;   /* Field     */
        case STORY_SYM_PROVIDER: return 11;  /* Interface */
        case STORY_SYM_FUNCTION: return 12;  /* Function  */
        case STORY_SYM_ENUM:     return 10;  /* Enum      */
        case STORY_SYM_ACTION:   return 6;   /* Method    */
        case STORY_SYM_RULE:     return 24;  /* Event     */
    }
    return 13;   /* Variable — unreachable fallback */
}

/* params.position -> 1-based line/col; document text via the store. */
static lsp_doc *nav_target(lsp_server *s, const json *msg, int *line, int *col)
{
    const json *params = json_get(msg, "params");
    const char *uri = json_str(json_get(json_get(params, "textDocument"), "uri"));
    const json *pos = json_get(params, "position");
    *line = (int)json_int(json_get(pos, "line"), 0) + 1;
    *col  = (int)json_int(json_get(pos, "character"), 0) + 1;
    return uri ? doc_find(s, uri) : NULL;
}

static const char *nav_uri(const json *msg)
{
    return json_str(json_get(json_get(json_get(msg, "params"), "textDocument"),
                             "uri"));
}

/* Go-to-definition: an atom resolves to its declaration site(s) and every rule
 * that concludes it (head occurrences) — "find all rules that conclude p". */
static void on_definition(lsp_server *s, const json *msg, const json *id,
                          lsp_emit_fn emit, void *ud)
{
    int line, col;
    lsp_doc *d = nav_target(s, msg, &line, &col);
    const char *uri = nav_uri(msg);
    strbuf rb; sb_init(&rb); sb_char(&rb, '[');
    if (d) {
        story_model *m = model_for(d->text);
        const story_occ *hit = occ_at(m, line, col);
        if (hit) {
            int n; const story_occ *occs = story_model_occs(m, &n);
            int emitted = 0;
            for (int i = 0; i < n; i++) {
                if ((occs[i].role == STORY_OCC_DECL ||
                     occs[i].role == STORY_OCC_HEAD) &&
                    strcmp(occs[i].name, hit->name) == 0) {
                    if (emitted++) sb_char(&rb, ',');
                    write_location(&rb, uri, &occs[i]);
                }
            }
        }
        story_model_free(m);
    }
    sb_char(&rb, ']');
    reply_result(emit, ud, id, rb.buf);
    sb_free(&rb);
}

/* Find-references: every occurrence of the atom under the cursor (declaration
 * included unless context.includeDeclaration is false). */
static void on_references(lsp_server *s, const json *msg, const json *id,
                          lsp_emit_fn emit, void *ud)
{
    int line, col;
    lsp_doc *d = nav_target(s, msg, &line, &col);
    const char *uri = nav_uri(msg);
    bool incl = json_bool(
        json_get(json_get(json_get(msg, "params"), "context"),
                 "includeDeclaration"), true);
    strbuf rb; sb_init(&rb); sb_char(&rb, '[');
    if (d) {
        story_model *m = model_for(d->text);
        const story_occ *hit = occ_at(m, line, col);
        if (hit) {
            int n; const story_occ *occs = story_model_occs(m, &n);
            int emitted = 0;
            for (int i = 0; i < n; i++) {
                if (strcmp(occs[i].name, hit->name) != 0) continue;
                if (!incl && occs[i].role == STORY_OCC_DECL) continue;
                if (emitted++) sb_char(&rb, ',');
                write_location(&rb, uri, &occs[i]);
            }
        }
        story_model_free(m);
    }
    sb_char(&rb, ']');
    reply_result(emit, ud, id, rb.buf);
    sb_free(&rb);
}

/* Document outline: every declaration, as a flat DocumentSymbol[]. */
static void on_document_symbol(lsp_server *s, const json *msg, const json *id,
                               lsp_emit_fn emit, void *ud)
{
    const char *uri = nav_uri(msg);
    lsp_doc *d = uri ? doc_find(s, uri) : NULL;
    strbuf rb; sb_init(&rb); sb_char(&rb, '[');
    if (d) {
        story_model *m = model_for(d->text);
        int n; const story_symbol *syms = story_model_symbols(m, &n);
        for (int i = 0; i < n; i++) {
            if (i) sb_char(&rb, ',');
            sb_raw(&rb, "{\"name\":");
            sb_jstr(&rb, syms[i].name);
            sb_raw(&rb, ",\"kind\":");
            sb_int(&rb, symbol_kind(syms[i].kind));
            sb_raw(&rb, ",\"range\":");
            write_range(&rb, syms[i].line, syms[i].col, syms[i].len);
            sb_raw(&rb, ",\"selectionRange\":");
            write_range(&rb, syms[i].line, syms[i].col, syms[i].len);
            sb_char(&rb, '}');
        }
        story_model_free(m);
    }
    sb_char(&rb, ']');
    reply_result(emit, ud, id, rb.buf);
    sb_free(&rb);
}

/* ----------------------------------------------------------------- dispatch */

/* Pull params.textDocument.uri (borrowed from the parse arena). */
static const char *doc_uri(const json *msg)
{
    return json_str(json_get(json_get(json_get(msg, "params"), "textDocument"),
                             "uri"));
}

static void on_did_open(lsp_server *s, const json *msg, lsp_emit_fn emit, void *ud)
{
    const json *td = json_get(json_get(msg, "params"), "textDocument");
    const char *uri  = json_str(json_get(td, "uri"));
    const char *text = json_str(json_get(td, "text"));
    if (!uri || !text) return;
    doc_put(s, uri, text);
    publish(s, uri, doc_find(s, uri)->text, emit, ud);
}

static void on_did_change(lsp_server *s, const json *msg, lsp_emit_fn emit, void *ud)
{
    const char *uri = doc_uri(msg);
    if (!uri) return;
    /* Full sync: the last content change carries the whole document text. */
    const json *changes = json_get(json_get(msg, "params"), "contentChanges");
    size_t n = json_arr_len(changes);
    if (n == 0) return;
    const char *text = json_str(json_get(json_arr_at(changes, n - 1), "text"));
    if (!text) return;
    doc_put(s, uri, text);
    publish(s, uri, doc_find(s, uri)->text, emit, ud);
}

static void on_did_close(lsp_server *s, const json *msg, lsp_emit_fn emit, void *ud)
{
    const char *uri = doc_uri(msg);
    if (!uri) return;
    doc_remove(s, uri);
    publish(s, uri, NULL, emit, ud);   /* clear diagnostics */
}

bool lsp_dispatch(lsp_server *s, const char *body, size_t len,
                  lsp_emit_fn emit, void *ud)
{
    arena a;
    arena_init(&a);
    bool exit_now = false;

    json *msg = json_parse(&a, body, len);
    const json *m  = json_get(msg, "method");
    const json *id = json_get(msg, "id");
    const char *method = json_str(m);

    if (!method) {
        /* Unparseable, or a response we don't issue requests for: ignore. */
    } else if (strcmp(method, "initialize") == 0) {
        reply_result(emit, ud, id, INITIALIZE_RESULT);
    } else if (strcmp(method, "initialized") == 0) {
        /* notification, no reply */
    } else if (strcmp(method, "shutdown") == 0) {
        s->got_shutdown = true;
        reply_result(emit, ud, id, "null");
    } else if (strcmp(method, "exit") == 0) {
        exit_now = true;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        on_did_open(s, msg, emit, ud);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        on_did_change(s, msg, emit, ud);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        on_did_close(s, msg, emit, ud);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        on_definition(s, msg, id, emit, ud);
    } else if (strcmp(method, "textDocument/references") == 0) {
        on_references(s, msg, id, emit, ud);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        on_document_symbol(s, msg, id, emit, ud);
    } else if (id) {
        /* An unknown *request* must be answered; notifications are dropped. */
        reply_error(emit, ud, id, RPC_METHOD_NOT_FOUND, "method not found");
    }

    arena_release(&a);
    return exit_now;
}

/* --------------------------------------------------------------- stdio loop */

typedef struct { FILE *out; } run_ctx;

static void run_emit(void *ud, const char *body, size_t len)
{
    FILE *out = ((run_ctx *)ud)->out;
    fprintf(out, "Content-Length: %zu\r\n\r\n", len);
    fwrite(body, 1, len, out);
    fflush(out);
}

/* Case-insensitive prefix test — avoids POSIX strncasecmp for a clean strict
 * C17 build. `pfx` is ASCII. */
static bool header_is(const char *line, const char *pfx)
{
    for (; *pfx; line++, pfx++) {
        int a = tolower((unsigned char)*line), b = tolower((unsigned char)*pfx);
        if (a != b) return false;
    }
    return true;
}

/* Read one framed message; returns a malloc'd body (caller frees) and its
 * length, or NULL at EOF / on a malformed frame. */
static char *read_frame(FILE *in, size_t *out_len)
{
    size_t content_length = 0;
    bool   have_length = false;
    char   line[1024];

    for (;;) {
        if (!fgets(line, sizeof line, in)) return NULL;   /* EOF */
        if (line[0] == '\r' && line[1] == '\n') break;    /* header terminator */
        if (line[0] == '\n') break;
        /* Case-insensitive "Content-Length:" per the LSP header grammar. */
        if (header_is(line, "Content-Length:")) {
            content_length = (size_t)strtoul(line + 15, NULL, 10);
            have_length = true;
        }
    }
    if (!have_length) return NULL;

    char *body = malloc(content_length + 1);
    if (!body) return NULL;
    if (fread(body, 1, content_length, in) != content_length) {
        free(body);
        return NULL;
    }
    body[content_length] = '\0';
    *out_len = content_length;
    return body;
}

int lsp_run(lsp_server *s, FILE *in, FILE *out)
{
    run_ctx ctx = { out };
    for (;;) {
        size_t n;
        char *body = read_frame(in, &n);
        if (!body) break;
        bool done = lsp_dispatch(s, body, n, run_emit, &ctx);
        free(body);
        if (done) break;
    }
    return s->got_shutdown ? 0 : 1;
}
