#!/bin/sh
# Serve the cellar. Nothing but a static file server — and the only thing this
# script really guarantees is the ROOT it serves from.
#
# That matters because the page fetches the `.story` SOURCE at runtime
# (DESIGN.md §12: source is always shipped, the compiled form is a cache), so
# `examples/` has to be reachable from the page. Serving `web/` alone gives a
# 404 on the story and a blank canvas with an error line — a confusing failure
# for a correct setup, which is exactly the kind of papercut worth one script.
#
# Usage:  web/serve.sh [port]        (from anywhere)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
port=${1:-8000}

if [ ! -f "$root/web/infeasible.js" ]; then
    echo "web/infeasible.js is missing — building it first (dev-time only)." >&2
    "$root/web/build.sh"
fi

echo "the cellar: http://localhost:$port/web/"
cd "$root"
exec python3 -m http.server "$port"
