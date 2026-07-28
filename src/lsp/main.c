/* `story-lsp`: the native .story language server (DESIGN.md §6.1 item 7),
 * speaking JSON-RPC 2.0 over stdio. An editor spawns it and drives the LSP
 * lifecycle; it pushes compiler diagnostics as the author types. All logic
 * lives in the `lsp_*` core (src/lsp/lsp.c) — this is just the stdio wiring. */

#include "lsp/lsp.h"

#include <stdio.h>

int main(void)
{
    lsp_server *s = lsp_new();
    if (!s) return 1;
    int code = lsp_run(s, stdin, stdout);
    lsp_free(s);
    return code;
}
