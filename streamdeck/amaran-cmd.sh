#!/bin/bash
# Shared logic of the Stream Deck switch bundles: make sure a daemon is there,
# send the command.
# Usage: amaran-cmd.sh <lightKey|all> <on|off|toggle> [project dir] [daemon app]
#
# "toggle": against the ESP32 bridge the bridge flips the light itself (it knows
# the state). Against the local daemon the lights do not report their state to
# the asker (the reply always goes to the original provisioner), so the state
# is recorded locally — and updated by on/off too, so toggles and fixed keys
# stay in step.
set -u
LIGHT="${1:?light missing}"; CMD="${2:?command missing}"
PROJECT="${3:-$HOME/Code/amaran-ble-control}"
DAEMON_APP="${4:-$HOME/Applications/amaran-daemon.app}"
STATE_DIR="$HOME/Library/Application Support/amaran-streamdeck"
LOG="$HOME/Library/Logs/amaran-streamdeck.log"
mkdir -p "$STATE_DIR"
STATE_FILE="$STATE_DIR/state-$LIGHT"
say() { printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"; }


# Target: local mesh daemon (default) or the ESP32 bridge on the network.
# build-apps.sh bakes AMARAN_HOST into the bundles.
HOST="${AMARAN_HOST:-127.0.0.1}"
case "$HOST" in
  127.0.0.1|localhost) LOCAL_TARGET=1 ;;
  *)                   LOCAL_TARGET=0 ;;
esac

if [ "$LOCAL_TARGET" = "1" ]; then
  # Ask the guard briefly before sending. Normally that costs a ps and a
  # sysctl; but if the daemon went stale over a sleep cycle, the key press would
  # otherwise fizzle out — the protocol reports no error.
  GUARD="$(dirname "$0")/daemon-guard.sh"
  [ -x "$GUARD" ] && "$GUARD" >/dev/null 2>&1
  PORT="${AMARAN_PORT:-$(python3 -c "import json;print(json.load(open('$PROJECT/lights.json')).get('http',{}).get('port',2708))" 2>/dev/null || echo 2708)}"
else
  # Against a device on the network there is no local daemon to guard and none
  # to start — the ESP32 runs all the time anyway.
  PORT="${AMARAN_PORT:-80}"
fi
BASE="http://$HOST:$PORT"

if [ "$CMD" = "toggle" ] && [ "$LOCAL_TARGET" != "1" ]; then
  # The bridge knows the state and flips the light itself: one call instead of
  # query plus command, and the answer names the new state.
  resp="$(curl -sS -m 4 -X POST "$BASE/lights/$LIGHT/toggle" 2>>"$LOG")"
  case "$resp" in
    *'"on":true'*)  printf on  > "$STATE_FILE"; say "$LIGHT toggle -> on (bridge)";  exit 0 ;;
    *'"on":false'*) printf off > "$STATE_FILE"; say "$LIGHT toggle -> off (bridge)"; exit 0 ;;
  esac
  say "$LIGHT toggle -> bridge $BASE gave an unexpected answer: '$resp'"
  osascript -e "display notification \"Bridge not answering: $HOST\" with title \"amaran\"" >/dev/null 2>&1
  exit 1
fi
if [ "$CMD" = "toggle" ]; then
  last="off"; [ -f "$STATE_FILE" ] && last="$(cat "$STATE_FILE" 2>/dev/null || echo off)"
  [ "$last" = "on" ] && CMD="off" || CMD="on"
  say "$LIGHT toggle (recorded: last $last) -> $CMD"
fi
send() { curl -sS -m 4 -X POST "$BASE/lights/$LIGHT/$CMD" -o /dev/null -w "%{http_code}" 2>>"$LOG"; }
done_ok() { printf '%s' "$CMD" > "$STATE_FILE"; say "$LIGHT $CMD -> ok$1"; exit 0; }

code="$(send || true)"
[ "$code" = "200" ] && done_ok ""

if [ "$LOCAL_TARGET" != "1" ]; then
  say "$LIGHT $CMD -> bridge $BASE not answering (HTTP '$code')"
  osascript -e "display notification \"Bridge not answering: $HOST\" with title \"amaran\"" >/dev/null 2>&1
  exit 1
fi

# No daemon reachable: start the daemon bundle through LaunchServices. Only then
# does macOS attribute the Bluetooth request to this bundle (and not to a node
# from outside, which would be killed with SIGABRT without asking).
say "$LIGHT $CMD -> no daemon (HTTP '$code'), starting $DAEMON_APP"
open -a "$DAEMON_APP" >>"$LOG" 2>&1 || { say "could not start $DAEMON_APP"; exit 1; }
for _ in $(seq 1 30); do
  sleep 1
  code="$(send || true)"
  [ "$code" = "200" ] && done_ok " (daemon started)"
done
say "ERROR: daemon not answering. Bluetooth permission granted to amaran-daemon.app?"
osascript -e 'display notification "Daemon not answering — check Bluetooth permission" with title "amaran"' >/dev/null 2>&1
exit 1
