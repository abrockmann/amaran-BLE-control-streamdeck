#!/bin/bash
# Installs the Stream Deck plugin. Stream Deck has to quit for this, otherwise
# the old code keeps running from memory while the new one is already on disk —
# a state that looks like "installed" and isn't.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/plugin/com.arnebrockmann.amaran.sdPlugin"
DST="$HOME/Library/Application Support/com.elgato.StreamDeck/Plugins/com.arnebrockmann.amaran.sdPlugin"
LOG="$HOME/Library/Logs/amaran-streamdeck-plugin.log"
python3 "$HERE/plugin/make-icons.py" >/dev/null

stop_streamdeck() {
  pgrep -qx "Stream Deck" || return 0
  # Stream Deck acknowledges the Apple event with error -128 but usually quits
  # anyway — so the return value is no success criterion, only the process
  # disappearing is.
  osascript -e 'quit app "Elgato Stream Deck"' >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do pgrep -qx "Stream Deck" || return 0; sleep 1; done
  echo "Stream Deck does not react to the Apple event — sending SIGTERM."
  pkill -x "Stream Deck" || true
  for _ in $(seq 1 10); do pgrep -qx "Stream Deck" || return 0; sleep 1; done
  echo "Stream Deck cannot be stopped — please quit it by hand." >&2
  return 1
}

stop_streamdeck
# Keep the target of an older install. The property inspector's settings win over
# paths.json anyway; without this a reinstall would silently switch an existing
# setup to the default bridge name.
OLD_PATHS=""
[ -f "$DST/paths.json" ] && OLD_PATHS="$(cat "$DST/paths.json")"
rm -rf "$DST"
cp -R "$SRC" "$DST"
# The installed plugin is a copy and does not know the repo path; without this
# file it finds the guard only through the default path. host/port/light are
# written only when set here or by the previous install — otherwise the plugin
# uses the settings from the property inspector, then amaran-esp32.local.
GUARD="$HERE/daemon-guard.sh" OLD_PATHS="$OLD_PATHS" python3 - "$DST/paths.json" <<'PY'
import json, os, sys
try:
    old = json.loads(os.environ.get("OLD_PATHS") or "{}")
except ValueError:
    old = {}
cfg = {"guard": os.environ["GUARD"],
       "wakeDriftMs": int(os.environ.get("WAKE_DRIFT_MS") or old.get("wakeDriftMs") or 10000)}
for env, key, conv in (("AMARAN_HOST", "host", str), ("AMARAN_PORT", "port", int), ("AMARAN_LIGHT", "light", str)):
    if os.environ.get(env):
        cfg[key] = conv(os.environ[env])
    elif key in old:
        cfg[key] = old[key]
with open(sys.argv[1], "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY
echo "installed: $DST"

# Only report success once a new plugin process is really running.
# Not based on the log: a chatty plugin scrolls the start line away.
marker=$(date +%s)
open -a "Elgato Stream Deck"
for _ in $(seq 1 40); do
  sleep 1
  pid=$(pgrep -f "com.arnebrockmann.amaran.sdPlugin/bin/plugin.js" | head -1) || true
  [ -n "${pid:-}" ] || continue
  # Start time via lstart, not via an embedded awk — the shell would expand its
  # $1, and with "set -u" the script would abort.
  lstart=$(ps -o lstart= -p "$pid" | sed 's/^ *//')
  started=$(date -j -f "%a %b %e %T %Y" "$lstart" +%s 2>/dev/null || echo 0)
  if [ "$started" -ge "$((marker - 2))" ]; then
    echo "Plugin running (PID $pid)."
    exit 0
  fi
done
echo "WARNING: Stream Deck restarted, but the plugin did not come up within 40 s." >&2
echo "Check: tail ~/Library/Logs/amaran-streamdeck-plugin.log" >&2
exit 1
