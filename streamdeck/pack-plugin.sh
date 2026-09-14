#!/bin/bash
# Builds a .streamDeckPlugin file: installed with a double-click, no clone and no
# Node setup needed. Meant for the ESP32 bridge — the plugin then finds it as
# amaran-esp32.local, or takes the address from its settings. For the local Mac
# daemon use install-plugin.sh instead: it also tells the plugin where the
# daemon guard lives.
#
#   streamdeck/pack-plugin.sh            # -> streamdeck/dist/
#   streamdeck/pack-plugin.sh <out-dir>
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/plugin/com.arnebrockmann.amaran.sdPlugin"
OUT="${1:-$HERE/dist}"

python3 "$HERE/plugin/make-icons.py" >/dev/null
if [ -e "$SRC/paths.json" ]; then
  echo "$SRC/paths.json exists — it holds machine-specific paths and must not be packed." >&2
  exit 1
fi
mkdir -p "$OUT"

# The Elgato CLI comes from streamdeck/pack/package-lock.json, which pins all 69
# packages — "npx @elgato/cli@1.9.0" pinned only the CLI itself, and its
# dependencies with caret ranges could pull in a freshly published, possibly
# compromised release. npm ci installs exactly the lockfile, without install
# scripts (none of the packages needs one). Bump deliberately: change the version
# in streamdeck/pack/package.json, then run
# "npm install --package-lock-only --ignore-scripts" there.
# --no-update-check keeps the CLI from fetching newer manifest schemas, so the
# same tree always packs the same way.
PACK="$HERE/pack"
npm ci --prefix "$PACK" --ignore-scripts --no-audit --no-fund --loglevel=error
"$PACK/node_modules/.bin/streamdeck" pack "$SRC" --output "$OUT" --force --no-update-check
