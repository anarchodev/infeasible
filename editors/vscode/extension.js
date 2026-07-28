// Thin VS Code client for the infeasible .story language (DESIGN.md §6.1 item
// 7). All intelligence lives in the native `story-lsp` server (src/lsp/); this
// spawns it over stdio and wires it to VS Code via vscode-languageclient. No
// language logic here — if you find yourself adding some, it belongs in the
// server so every editor gets it.

const path = require("path");
const fs = require("fs");
const { workspace, window, commands } = require("vscode");
const { LanguageClient, TransportKind } = require("vscode-languageclient/node");

let client;

// Resolve the server executable: an absolute path as-is; a bare name off PATH;
// a relative path against the workspace. When the setting is left at its
// default, prefer a CMake `build/story-lsp` in the workspace if one exists.
function resolveServerCommand() {
  const configured = workspace.getConfiguration("story").get("serverPath") || "story-lsp";
  const folder = workspace.workspaceFolders && workspace.workspaceFolders[0];
  const root = folder ? folder.uri.fsPath : undefined;

  if (path.isAbsolute(configured)) return configured;

  if (configured === "story-lsp" && root) {
    const built = path.join(root, "build", "story-lsp");
    if (fs.existsSync(built)) return built;
  }
  if (configured.includes(path.sep) && root) return path.join(root, configured);
  return configured; // bare name -> PATH lookup
}

function newClient() {
  const command = resolveServerCommand();

  // Warn early if we can point at a concrete file that isn't there; a bare
  // PATH name we can't cheaply check, so let the spawn error surface instead.
  if ((path.isAbsolute(command) || command.includes(path.sep)) && !fs.existsSync(command)) {
    window.showWarningMessage(
      `story-lsp not found at ${command}. Build it with \`cmake --build build\`, ` +
        "or set `story.serverPath`."
    );
  }

  const serverOptions = { command, args: [], transport: TransportKind.stdio };
  const clientOptions = {
    documentSelector: [{ scheme: "file", language: "story" }],
    synchronize: { fileEvents: workspace.createFileSystemWatcher("**/*.story") },
  };
  // Client id "story" -> reads the `story.trace.server` setting automatically.
  return new LanguageClient("story", "infeasible .story LSP", serverOptions, clientOptions);
}

function activate(context) {
  client = newClient();
  client.start();

  context.subscriptions.push(
    commands.registerCommand("story.restartServer", async () => {
      if (client) await client.stop();
      client = newClient();
      client.start();
    })
  );
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
