/* Golden test for the .story language server (DESIGN.md §6.1 item 7). Drives
 * the pure dispatch core (lsp_dispatch) with crafted JSON-RPC bodies and
 * captures the emitted replies/notifications through a sink — no process, no
 * framing, fully deterministic. Pins the lifecycle handshake, full-text
 * document sync, and that compiler diagnostics reach the wire as LSP
 * publishDiagnostics with the right severities and ranges. Also unit-checks
 * the minimal JSON layer (src/lsp/json.c) the server is built on. */

#include "lsp/lsp.h"
#include "lsp/json.h"
#include "core/arena.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            return 1; \
        } \
    } while (0)

/* Story sources with known diagnostics (mirroring tests/test_parse.c). */
#define SRC_CLEAN "state y\nrule a: derived => x\nrule b: y => derived"
#define SRC_ERROR "rule bad1: a =>\n"                    /* missing head -> error */
#define SRC_WARN  "state holding\nrule r: hodling => weak" /* typo -> warning     */

/* Navigation fixture — exact columns matter, so lay it out explicitly:
 *   L0: "state ( alive )"          alive: fluent decl at char 8
 *   L1: "rule mk: alive => happy"  alive body char 9; happy HEAD char 18
 *   L2: "rule br: happy => calm"   happy body char 9; calm HEAD char 18
 * `alive` is a pure fluent (decl target); `happy` is a pure conclusion (head
 * target) — so definition exercises both without fluent-as-head ambiguity. */
#define SRC_NAV \
    "state ( alive )\n" \
    "rule mk: alive => happy\n" \
    "rule br: happy => calm"

/* Capture sink: accumulate every emitted body, newline-separated, for strstr. */
static void cap_emit(void *ud, const char *body, size_t len)
{
    strbuf *sb = ud;
    sb_rawn(sb, body, len);
    sb_char(sb, '\n');
}

static void build_open(strbuf *m, const char *uri, const char *text)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\","
              "\"params\":{\"textDocument\":{\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, ",\"text\":");
    sb_jstr(m, text);
    sb_raw(m, "}}}");
}

static void build_change(strbuf *m, const char *uri, const char *text)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\","
              "\"params\":{\"textDocument\":{\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, "},\"contentChanges\":[{\"text\":");
    sb_jstr(m, text);
    sb_raw(m, "}]}}");
}

static void build_close(strbuf *m, const char *uri)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\","
              "\"params\":{\"textDocument\":{\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, "}}}");
}

static void build_pos_req(strbuf *m, const char *method, const char *uri,
                          int line, int ch, int id)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_int(m, id);
    sb_raw(m, ",\"method\":");
    sb_jstr(m, method);
    sb_raw(m, ",\"params\":{\"textDocument\":{\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, "},\"position\":{\"line\":");
    sb_int(m, line);
    sb_raw(m, ",\"character\":");
    sb_int(m, ch);
    sb_raw(m, "}}}");
}

static void build_docsym(strbuf *m, const char *uri, int id)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_int(m, id);
    sb_raw(m, ",\"method\":\"textDocument/documentSymbol\","
              "\"params\":{\"textDocument\":{\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, "}}}");
}

static bool dispatch(lsp_server *s, strbuf *cap, const char *body)
{
    return lsp_dispatch(s, body, strlen(body), cap_emit, cap);
}

/* initialize -> a result echoing the id, advertising sync capabilities. */
static int test_initialize(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    dispatch(s, &cap,
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"initialize\",\"params\":{}}");
    CHECK(strstr(cap.buf, "\"id\":7") != NULL);
    CHECK(strstr(cap.buf, "\"capabilities\"") != NULL);
    CHECK(strstr(cap.buf, "\"textDocumentSync\"") != NULL);
    CHECK(strstr(cap.buf, "\"openClose\":true") != NULL);
    sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* didOpen a clean source -> publishDiagnostics with an empty list. */
static int test_open_clean(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    build_open(&msg, "file:///clean.story", SRC_CLEAN);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "textDocument/publishDiagnostics") != NULL);
    CHECK(strstr(cap.buf, "\"uri\":\"file:///clean.story\"") != NULL);
    CHECK(strstr(cap.buf, "\"diagnostics\":[]") != NULL);
    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* didOpen a broken source -> an error diagnostic (severity 1) at line 0. */
static int test_open_error(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    build_open(&msg, "file:///bad.story", SRC_ERROR);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "textDocument/publishDiagnostics") != NULL);
    CHECK(strstr(cap.buf, "\"severity\":1") != NULL);
    CHECK(strstr(cap.buf, "expected an atom name") != NULL);
    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* didChange (full sync) re-analyzes: a typo surfaces as a warning (severity 2)
 * carrying the offending atom in its message. */
static int test_change_warns(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);

    build_open(&msg, "file:///w.story", SRC_CLEAN);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    build_change(&msg, "file:///w.story", SRC_WARN);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"severity\":2") != NULL);
    CHECK(strstr(cap.buf, "hodling") != NULL);
    /* `hodling` sits at 1-based line 2, col 9, width 7: pins the position
     * remap (line-1, col-1) and the identifier-width squiggle in one shot. */
    CHECK(strstr(cap.buf, "\"start\":{\"line\":1,\"character\":8}") != NULL);
    CHECK(strstr(cap.buf, "\"end\":{\"line\":1,\"character\":15}") != NULL);
    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* didClose clears diagnostics (empty list published for the uri). */
static int test_close_clears(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);

    build_open(&msg, "file:///c.story", SRC_ERROR);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    build_close(&msg, "file:///c.story");
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"uri\":\"file:///c.story\"") != NULL);
    CHECK(strstr(cap.buf, "\"diagnostics\":[]") != NULL);
    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* shutdown replies null; exit ends the dispatch loop; an unknown *request*
 * gets a MethodNotFound error while an unknown *notification* is dropped. */
static int test_lifecycle(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);

    bool exit1 = dispatch(s, &cap,
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\"}");
    CHECK(!exit1);
    CHECK(strstr(cap.buf, "\"result\":null") != NULL);

    sb_reset(&cap);
    bool done = dispatch(s, &cap, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    CHECK(done);

    sb_reset(&cap);
    dispatch(s, &cap, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"no/such\"}");
    CHECK(strstr(cap.buf, "-32601") != NULL);

    sb_reset(&cap);
    dispatch(s, &cap, "{\"jsonrpc\":\"2.0\",\"method\":\"$/whatever\"}");
    CHECK(cap.len == 0);   /* unknown notification: silent */

    sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* Navigation over the compiler's span model (no re-parse): go-to-definition,
 * find-references, and the document outline. */
static int test_navigation(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    const char *uri = "file:///nav.story";

    build_open(&msg, uri, SRC_NAV);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* definition on the body use of `alive` (L1) -> its fluent decl (L0 char 8),
     * NOT the body use itself. */
    build_pos_req(&msg, "textDocument/definition", uri, 1, 9, 10);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":0,\"character\":8") != NULL);
    CHECK(strstr(cap.buf, "\"character\":9") == NULL);
    sb_reset(&cap);

    /* definition on the body use of `happy` (L2) -> the rule that concludes it
     * (mk's head, L1 char 18) — "find all rules that conclude p". */
    build_pos_req(&msg, "textDocument/definition", uri, 2, 9, 11);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":1,\"character\":18") != NULL);
    sb_reset(&cap);

    /* references on `happy` (click its head, L1 char 18) -> head + body use. */
    build_pos_req(&msg, "textDocument/references", uri, 1, 18, 12);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":1,\"character\":18") != NULL);  /* head */
    CHECK(strstr(cap.buf, "\"line\":2,\"character\":9") != NULL);   /* body */
    sb_reset(&cap);

    /* documentSymbol -> the outline: the fluent (Field=8) and both rules. */
    build_docsym(&msg, uri, 13);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"name\":\"alive\"") != NULL);
    CHECK(strstr(cap.buf, "\"kind\":8") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"mk\"") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"br\"") != NULL);
    sb_reset(&cap);

    /* definition on a keyword / non-atom position -> empty result. */
    build_pos_req(&msg, "textDocument/definition", uri, 0, 0, 14);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"result\":[]") != NULL);

    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* The minimal JSON layer: parse a nested object, typed accessors, escapes,
 * and rejection of malformed input. */
static int test_json(void)
{
    arena a; arena_init(&a);
    const char *src =
        "{\"n\":42,\"neg\":-3,\"b\":true,\"s\":\"a\\nb\","
        "\"arr\":[1,2,3],\"o\":{\"k\":\"v\"}}";
    json *v = json_parse(&a, src, strlen(src));
    CHECK(v != NULL);
    CHECK(json_is(v, JSON_OBJ));
    CHECK(json_int(json_get(v, "n"), 0) == 42);
    CHECK(json_int(json_get(v, "neg"), 0) == -3);
    CHECK(json_bool(json_get(v, "b"), false) == true);

    const char *s = json_str(json_get(v, "s"));
    CHECK(s != NULL && strcmp(s, "a\nb") == 0);   /* \n decoded */

    const json *arr = json_get(v, "arr");
    CHECK(json_arr_len(arr) == 3);
    CHECK(json_int(json_arr_at(arr, 2), 0) == 3);
    CHECK(json_int(json_arr_at(arr, 9), -1) == -1);   /* OOB -> default */

    CHECK(strcmp(json_str(json_get(json_get(v, "o"), "k")), "v") == 0);

    CHECK(json_parse(&a, "{bad", 4) == NULL);
    CHECK(json_parse(&a, "[1,2", 4) == NULL);

    arena_release(&a);
    return 0;
}

int main(void)
{
    if (test_json())        return 1;
    if (test_initialize())  return 1;
    if (test_open_clean())  return 1;
    if (test_open_error())  return 1;
    if (test_change_warns()) return 1;
    if (test_close_clears()) return 1;
    if (test_lifecycle())   return 1;
    if (test_navigation())  return 1;
    printf("test_lsp: all passed\n");
    return 0;
}
