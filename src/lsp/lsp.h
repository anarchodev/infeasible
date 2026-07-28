#ifndef INF_LSP_LSP_H
#define INF_LSP_LSP_H

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>

/* A native language server for the .story surface language (DESIGN.md §6.1
 * item 7). Transport is JSON-RPC 2.0 over stdio with LSP `Content-Length`
 * framing; the server links the compiler directly (deps: lang), so it reads
 * the same `story_diags` an author sees at build time.
 *
 * This slice is the spine: lifecycle (initialize / shutdown / exit), full-text
 * document sync (didOpen / didChange / didClose), and push diagnostics — the
 * authoring signal §6.1 items 1/2/4 already compute. Navigation over the
 * grounded vocabulary (go-to-definition, "rules that conclude p", attacker and
 * dependency cones — §6.1 item 7) is a later slice on this same loop.
 *
 * `lsp_dispatch` is the pure core — feed it a message body, capture replies
 * through a sink — so behaviour is testable without spawning a process
 * (tests/test_lsp.c). `lsp_run` wires it to framed stdio. */

typedef struct lsp_server lsp_server;

lsp_server *lsp_new(void);
void        lsp_free(lsp_server *s);

/* Sink for one outgoing JSON-RPC message body (no framing). */
typedef void (*lsp_emit_fn)(void *ud, const char *body, size_t len);

/* Dispatch one incoming message body; emit zero or more replies/notifications
 * through `emit`. Returns true once the peer has requested `exit`. */
bool lsp_dispatch(lsp_server *s, const char *body, size_t len,
                  lsp_emit_fn emit, void *ud);

/* Blocking read/dispatch/write loop over framed stdio. Returns the process
 * exit code per the LSP spec: 0 if `exit` followed a `shutdown`, else 1. */
int  lsp_run(lsp_server *s, FILE *in, FILE *out);

#endif
