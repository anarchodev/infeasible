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

/* Cone fixture: `open` is concluded by `can` and attacked (`~open`) by `blk`.
 *   L1: "rule can: strong => open"   open HEAD (positive) at char 20
 *   L2: "rule blk: weak => ~open"    open HEAD (negated / attacker)          */
#define SRC_CONE \
    "state ( strong weak )\n" \
    "rule can: strong => open\n" \
    "rule blk: weak => ~open"

/* Call-hierarchy fixture — a two-hop support chain a -> x -> y:
 *   L1: "rule r1: a => x"        x HEAD at char 14
 *   L2: "rule r2: x & b => y"    x feeds y
 * outgoing(x) = {a} (its premise); incoming(x) = {y} (what it feeds).      */
#define SRC_CH \
    "state ( a b )\n" \
    "rule r1: a => x\n" \
    "rule r2: x & b => y"

/* UTF-16 position fixture: a mid-line block comment with a 2-byte character
 * (é = C3 A9) sits before `ax` on L1, so `ax`'s BYTE column (17) and its
 * UTF-16 column (16) differ by one. Pins that the protocol boundary reports /
 * accepts UTF-16, not bytes.  L1: rule r: /* é *​/ ax => bx                 */
#define SRC_UTF8 \
    "state ( ax )\n" \
    "rule r: /* \xC3\xA9 */ ax => bx"

/* Cache-invalidation fixture: `foo`'s declaration moves from L0 to L2 across an
 * edit; a stale cached model would keep pointing at L0. */
#define SRC_V1 "state ( foo )\nrule r: foo => bar"
#define SRC_V2 "\n\nstate ( foo )\nrule r: foo => bar"

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

/* A callHierarchy/{incoming,outgoing}Calls request. The server reads only
 * item.name and item.uri; range fields are filler to look spec-shaped. */
static void build_ch_req(strbuf *m, const char *method, const char *uri,
                         const char *name, int id)
{
    sb_reset(m);
    sb_raw(m, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_int(m, id);
    sb_raw(m, ",\"method\":");
    sb_jstr(m, method);
    sb_raw(m, ",\"params\":{\"item\":{\"name\":");
    sb_jstr(m, name);
    sb_raw(m, ",\"uri\":");
    sb_jstr(m, uri);
    sb_raw(m, ",\"kind\":24,"
              "\"range\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":0}},"
              "\"selectionRange\":{\"start\":{\"line\":0,\"character\":0},"
              "\"end\":{\"line\":0,\"character\":0}}}}}");
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
    CHECK(strstr(cap.buf, "\"detail\":\"fluent\"") != NULL);   /* concept in detail */
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

/* Hover cone summary: the atom under the cursor, the rules that conclude it,
 * and the rules that attack it (`~p` heads) — the dependency/attacker cone. */
static int test_hover(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    const char *uri = "file:///cone.story";

    build_open(&msg, uri, SRC_CONE);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* hover on the positive head `open` (L1 char 20) -> concluder + attacker */
    build_pos_req(&msg, "textDocument/hover", uri, 1, 20, 20);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "conclusion") != NULL);
    CHECK(strstr(cap.buf, "Concluded by") != NULL);
    CHECK(strstr(cap.buf, "can") != NULL);
    CHECK(strstr(cap.buf, "Attacked by") != NULL);   /* polarity split */
    CHECK(strstr(cap.buf, "blk") != NULL);
    sb_reset(&cap);

    /* hover off any atom (the `rule` keyword, L1 char 0) -> null. */
    build_pos_req(&msg, "textDocument/hover", uri, 1, 0, 21);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"result\":null") != NULL);

    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* Call hierarchy: the navigable cone. prepare roots on the atom; outgoing
 * walks to premises (affected-by), incoming walks to dependents (affects). */
static int test_call_hierarchy(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    const char *uri = "file:///ch.story";

    build_open(&msg, uri, SRC_CH);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* prepare on the head `x` (L1 char 14) -> a root item named x. */
    build_pos_req(&msg, "textDocument/prepareCallHierarchy", uri, 1, 14, 30);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"name\":\"x\"") != NULL);
    sb_reset(&cap);

    /* outgoing(x) -> its premise `a`, and NOT the downstream `y`. */
    build_ch_req(&msg, "callHierarchy/outgoingCalls", uri, "x", 31);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"to\":") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"a\"") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"y\"") == NULL);
    sb_reset(&cap);

    /* incoming(x) -> what it feeds: the head `y` of r2. */
    build_ch_req(&msg, "callHierarchy/incomingCalls", uri, "x", 32);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"from\":") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"y\"") != NULL);
    sb_reset(&cap);

    /* outgoing(y) -> both premises of r2: `x` and `b`. */
    build_ch_req(&msg, "callHierarchy/outgoingCalls", uri, "y", 33);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"name\":\"x\"") != NULL);
    CHECK(strstr(cap.buf, "\"name\":\"b\"") != NULL);
    sb_reset(&cap);

    /* prepare off any atom -> null. */
    build_pos_req(&msg, "textDocument/prepareCallHierarchy", uri, 1, 0, 34);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"result\":null") != NULL);

    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* Position encoding: LSP characters are UTF-16 code units, but the compiler
 * emits byte columns. A 2-byte char before `ax` on L1 makes them differ (byte
 * 17 vs UTF-16 16); check both the outgoing and incoming remap. */
static int test_utf16(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    const char *uri = "file:///u.story";

    build_open(&msg, uri, SRC_UTF8);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* OUTGOING: references from ax's (ASCII) decl includes its L1 body use,
     * whose reported character must be the UTF-16 column 16, not the byte 17. */
    build_pos_req(&msg, "textDocument/references", uri, 0, 8, 40);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":1,\"character\":16") != NULL);
    CHECK(strstr(cap.buf, "\"line\":1,\"character\":17") == NULL);
    sb_reset(&cap);

    /* INCOMING: a position at UTF-16 column 16 on L1 must resolve to `ax`
     * (byte 17 in the model) -> definition returns its decl. Without the
     * remap, column 16 would land on the space before `ax` and miss. */
    build_pos_req(&msg, "textDocument/definition", uri, 1, 16, 41);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":0,\"character\":8") != NULL);

    sb_free(&msg); sb_free(&cap);
    lsp_free(s);
    return 0;
}

/* The span model is cached per document and reused across navigation requests;
 * a didChange must invalidate it so navigation reflects the new text, not the
 * stale compile. `foo`'s decl moves L0 -> L2 across the edit. */
static int test_cache_invalidation(void)
{
    lsp_server *s = lsp_new();
    strbuf cap; sb_init(&cap);
    strbuf msg; sb_init(&msg);
    const char *uri = "file:///cache.story";

    build_open(&msg, uri, SRC_V1);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* definition on foo's body use (L1) -> its decl at L0. */
    build_pos_req(&msg, "textDocument/definition", uri, 1, 8, 50);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":0,\"character\":8") != NULL);
    sb_reset(&cap);

    /* edit: two blank lines pushed in front. */
    build_change(&msg, uri, SRC_V2);
    dispatch(s, &cap, msg.buf);
    sb_reset(&cap);

    /* definition on foo's body (now L3) -> its decl at L2, never the stale L0. */
    build_pos_req(&msg, "textDocument/definition", uri, 3, 8, 51);
    dispatch(s, &cap, msg.buf);
    CHECK(strstr(cap.buf, "\"line\":2,\"character\":8") != NULL);
    CHECK(strstr(cap.buf, "\"line\":0") == NULL);

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
    if (test_hover())       return 1;
    if (test_call_hierarchy()) return 1;
    if (test_utf16())       return 1;
    if (test_cache_invalidation()) return 1;
    printf("test_lsp: all passed\n");
    return 0;
}
