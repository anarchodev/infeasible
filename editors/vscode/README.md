# infeasible `.story` — VS Code extension

A thin client for the native [`story-lsp`](../../src/lsp) language server
(DESIGN.md §6.1 item 7). All the intelligence — diagnostics, navigation,
cones — lives in the C server; this extension only spawns it over stdio and
wires it to VS Code, plus contributes the file association, comment/bracket
config, and a TextMate grammar for syntax highlighting.

## What you get

- **Diagnostics** as you type — the same compiler errors/warnings a build
  produces (orphan/typo detection, parse errors), squiggled inline.
- **Go-to-definition** on an atom → its declaration and every rule that
  concludes it.
- **Find-references** across the file.
- **Outline / breadcrumbs** (`documentSymbol`) — sorts, entities, fluents,
  providers, functions, actions, rules.
- **Hover** — a dependency/attacker cone: which rules conclude the atom, which
  *attack* it (a `~p` head / defeater), and how many rule bodies read it.

## Build & run (from source)

1. Build the server (from the repo root):

   ```sh
   cmake -B build && cmake --build build      # produces build/story-lsp
   ```

2. Install the client's one dependency (`vscode-languageclient`):

   ```sh
   cd editors/vscode && npm install
   ```

3. Launch it: open `editors/vscode/` in VS Code and press **F5** (the "Run
   .story extension" launch config opens an Extension Development Host). Open
   any `.story` file — e.g. from `examples/` — to activate it.

The extension finds the server automatically: when `story.serverPath` is left
at its default, a `build/story-lsp` under the workspace root is preferred;
otherwise the setting is used (a bare name is looked up on `PATH`).

## Settings

- `story.serverPath` — path to `story-lsp` (default `story-lsp`).
- `story.trace.server` — `off` | `messages` | `verbose`; logs the JSON-RPC
  traffic to the ".story" output channel.

Command: **infeasible .story: Restart Server** (`story.restartServer`).

## Packaging

`npx @vscode/vsce package` produces a `.vsix` you can install with
`code --install-extension infeasible-story-0.1.0.vsix`.
