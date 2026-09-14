#!/bin/bash
# Sets up the LaunchAgent that runs the guard at login and every minute after
# (StartInterval 60); the guard starts or replaces the daemon bundle. Paths are
# filled in at install time, because launchd expands neither $HOME nor the tilde.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PLIST="$HOME/Library/LaunchAgents/com.arnebrockmann.amaran-daemon.plist"
[ -d "$HOME/Applications/amaran-daemon.app" ] || {
  echo "amaran-daemon.app missing — run $HERE/build-apps.sh first." >&2; exit 1; }
mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
sed -e "s|__HOME__|$HOME|g" -e "s|__GUARD__|$HERE/daemon-guard.sh|g" \
  "$HERE/com.arnebrockmann.amaran-daemon.plist.in" > "$PLIST"
chmod 644 "$PLIST"
launchctl bootout "gui/$UID/com.arnebrockmann.amaran-daemon" 2>/dev/null || true
launchctl bootstrap "gui/$UID" "$PLIST"
echo "loaded: com.arnebrockmann.amaran-daemon"
launchctl list | grep amaran || true
