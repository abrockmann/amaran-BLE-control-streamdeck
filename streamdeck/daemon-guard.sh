#!/bin/bash
# Keeps the mesh daemon usable. Three jobs:
#
#   1. If it is not running, start it.
#   2. If it is older than the Mac's last wake-up, restart it.
#   3. If it runs but does not answer on its API, replace it as well.
#
# Why (2): after a sleep cycle the GATT connection survives as far as the system
# is concerned — the light is still listed as a connected device — but the mesh
# session on top of it is dead: proxy filter and sequence numbers date from
# before the sleep. The daemon then sends into the void and still reports
# success, because the protocol has no acknowledgement. It never notices on its
# own; the wake-up time is the only reliable hint.
#
# Why (3): a live process is not a reachable daemon. If it hangs in the BLE
# connection attempt, it never opens its socket and API — and without this probe
# the guard would see the process and be satisfied. On 2026-09-11 it hung like
# that for 28 minutes on a foreign device that had matched the scan by name
# only, without writing a single log line.
#
# Always start through "open -a": only then does macOS attribute the Bluetooth
# request to the app bundle that holds the permission.
set -u
APP="${AMARAN_DAEMON_APP:-$HOME/Applications/amaran-daemon.app}"
BIN="$APP/Contents/MacOS/node"
LOG="$HOME/Library/Logs/amaran-daemon-guard.log"
PORT="${AMARAN_PORT:-2708}"
# Start-up time before the API probe applies: a fresh daemon first scans and
# connects (measured at about 13 s). Without this grace period the guard would
# kill every start on its next run. 0 = check immediately (tests).
API_GRACE="${AMARAN_API_GRACE:-90}"
say() { printf '%s  %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"; }

[ -d "$APP" ] || { say "ERROR: $APP missing — run build-apps.sh"; exit 1; }

# Two guard runs must not overtake each other. The one-minute timer and the call
# from amaran-cmd.sh (before every key press) can coincide, and a repair run can
# take up to a minute — seen on 2026-09-11: one run was still waiting for the
# API while the other had already started a second daemon. Without a lock one
# run kills what the other just built.
# mkdir is atomic; the PID inside makes an orphaned lock detectable.
# In the user's own Caches folder, not in $TMPDIR: launchd and the plugin do not
# necessarily hand the guard the same TMPDIR, and two different lock paths would
# not exclude each other. /tmp would let another account pre-create the lock.
mkdir -p "$HOME/Library/Caches"
LOCK="$HOME/Library/Caches/amaran-daemon-guard.lock"
take_lock() {
  if mkdir "$LOCK" 2>/dev/null; then
    echo $$ > "$LOCK/pid"; trap 'rm -rf "$LOCK"' EXIT; return 0
  fi
  holder="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ -n "${holder:-}" ] && kill -0 "$holder" 2>/dev/null; then
    return 1   # another run holds it — step back quietly
  fi
  say "removed orphaned lock of PID ${holder:-?}"
  rm -rf "$LOCK"
  mkdir "$LOCK" 2>/dev/null || return 1
  echo $$ > "$LOCK/pid"; trap 'rm -rf "$LOCK"' EXIT
  return 0
}
take_lock || exit 0

# WAKE_OVERRIDE only exists to test the restart path.
wake="${WAKE_OVERRIDE:-$(sysctl -n kern.waketime 2>/dev/null | awk '{gsub(/,/,""); print $4}')}"
case "$wake" in ''|*[!0-9]*) say "kern.waketime unreadable — skipped"; exit 0 ;; esac

# Healthy means OUR daemon answers: any process on the port would otherwise do —
# for instance one of another account, while ours is down.
# With an apiKey in lights.json the daemon answers 401 — that is still our daemon.
api_up() {
  case "$(curl -sS -m 3 "http://127.0.0.1:$PORT/lights" 2>/dev/null)" in
    *'"daemon":true'*|*'"error":"Unauthorized"'*) return 0 ;;
    *) return 1 ;;
  esac
}
# Only this account's processes: pgrep -f alone also matches other users'.
own_pgrep() { pgrep -U "$(id -u)" -f "$BIN"; }

# 30 s was too short: a start that only came through after about 40 s was
# reported as "API not answering" although it ran afterwards — a report that was
# wrong. Conversely, waiting on makes no sense once the daemon has exited again;
# it does that by itself as soon as it finds no mesh node. Both cases are now
# told apart.
start_daemon() {
  open -a "$APP" >/dev/null 2>&1 || { say "could not start $APP"; return 1; }
  wait_s="${AMARAN_START_WAIT:-60}"
  i=0
  while [ "$i" -lt "$wait_s" ]; do
    i=$((i + 1))
    sleep 1
    api_up && return 0
    # Only check from the third second on — before that the bundle is often not
    # yet visible as a process, and we would declare our own start dead.
    if [ "$i" -ge 3 ] && ! own_pgrep >/dev/null 2>&1; then
      say "daemon exited again while connecting — no mesh node reachable"
      return 1
    fi
  done
  say "started, but the API does not answer (waited ${wait_s} s)"
  return 1
}

stop_daemon() {
  kill "$1" 2>/dev/null
  for _ in $(seq 1 10); do sleep 1; kill -0 "$1" 2>/dev/null || break; done
  kill -9 "$1" 2>/dev/null
  sleep 1
}

pid="$(own_pgrep | head -1)"
if [ -z "$pid" ]; then
  say "daemon not running — starting it"
  start_daemon && say "running again"
  exit 0
fi

# Start time from the elapsed run time, so no date parsing is needed.
started=$(ps -o etime= -p "$pid" | awk -v now="$(date +%s)" '
  { t=$1; d=0
    if (index(t,"-")) { split(t,a,"-"); d=a[1]; t=a[2] }
    n=split(t,b,":")
    s = (n==3) ? b[1]*3600+b[2]*60+b[3] : (n==2) ? b[1]*60+b[2] : b[1]
    printf "%d", now - (s + d*86400) }')

if [ "$started" -lt "$((wake - 5))" ]; then
  say "daemon (PID $pid, started $(date -r "$started" '+%H:%M:%S')) is older than the wake-up at $(date -r "$wake" '+%H:%M:%S') — mesh session is stale, restarting"
  stop_daemon "$pid"
  start_daemon && say "reconnected (PID $(own_pgrep | head -1))"
  exit 0
fi

age=$(( $(date +%s) - started ))
if [ "$age" -ge "$API_GRACE" ] && ! api_up; then
  # Second chance: the daemon is single-threaded, and an ongoing mesh send can
  # block the answer briefly. Only silence twice counts as dead — a false alarm
  # would throw away a working connection.
  sleep 3
  if ! api_up; then
    say "daemon (PID $pid, $age s old) runs but does not answer — restarting"
    stop_daemon "$pid"
    start_daemon && say "running again (PID $(own_pgrep | head -1))"
  fi
fi
exit 0
