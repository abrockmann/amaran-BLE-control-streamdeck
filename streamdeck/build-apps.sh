#!/bin/bash
# Builds the .app bundles:
#
#   amaran-daemon.app   holds the Bluetooth permission and runs the daemon.
#                       Contains a COPY of the node binary, because macOS
#                       attributes the Bluetooth request to the binary actually
#                       executing — a node outside the bundle has no usage
#                       description and is killed with SIGABRT without asking.
#                       Ad-hoc signed so TCC sees a stable identity.
#   amaran-on.app       only send HTTP (to 127.0.0.1 or the ESP32 bridge) — no
#   amaran-off.app      Bluetooth, no permission needed. Against the local daemon
#   amaran-toggle.app   they start the daemon bundle through LaunchServices
#                       (open -a) when needed, so its permission applies.
#                       Built only with WITH_SWITCH_APPS=1 (see below).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$HERE/.." && pwd)"
OUT="${1:-$HOME/Applications}"
LIGHT="${LIGHT:-amaranve}"
# Target of the switch commands. Empty/127.0.0.1 = local daemon; an IP on the
# network = ESP32 bridge. Baked into the bundles, because they inherit no
# environment.
AMARAN_HOST="${AMARAN_HOST:-127.0.0.1}"
AMARAN_PORT="${AMARAN_PORT:-}"
NODE_REAL="$(readlink -f "$(command -v node)")"
[ -x "$NODE_REAL" ] || { echo "node not found"; exit 1; }
mkdir -p "$OUT"

plist() { # $1=id $2=name $3=exec $4=extra
  cat <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$2</string>
	<key>CFBundleIdentifier</key><string>$1</string>
	<key>CFBundleExecutable</key><string>$3</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>LSUIElement</key><true/>
$4
</dict>
</plist>
PLIST
}

# ── Daemon bundle ────────────────────────────────────────────────────────────
APP="$OUT/amaran-daemon.app"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS"
plist "com.arnebrockmann.amaran.daemon" "amaran Daemon" "amaran-daemon" \
"	<key>NSBluetoothAlwaysUsageDescription</key>
	<string>Controls the amaran lights over the Bluetooth mesh.</string>" \
  > "$APP/Contents/Info.plist"
cp "$NODE_REAL" "$APP/Contents/MacOS/node"
cat > "$APP/Contents/MacOS/amaran-daemon" <<RUN
#!/bin/bash
# The bundled node starts the daemon directly with the tsx loader —
# no npx, so no node from outside gets in between.
cd "$PROJECT" || exit 1
exec "\$(dirname "\$0")/node" --import tsx "$PROJECT/src/daemon.ts" \\
  >>"\$HOME/Library/Logs/amaran-daemon.log" 2>&1
RUN
chmod +x "$APP/Contents/MacOS/amaran-daemon"
codesign --force --sign - --identifier com.arnebrockmann.amaran.daemon "$APP" >/dev/null 2>&1 \
  && echo "built and ad-hoc signed: $APP" || echo "built (signing failed): $APP"

# ── Switch bundles ───────────────────────────────────────────────────────────
make_switch() {
  local cmd="$1" label="$2" app="$OUT/amaran-$1.app"
  rm -rf "$app"; mkdir -p "$app/Contents/MacOS"
  plist "com.arnebrockmann.amaran.$cmd" "$label" "amaran-$cmd" "" > "$app/Contents/Info.plist"
  cat > "$app/Contents/MacOS/amaran-$cmd" <<RUN
#!/bin/bash
export AMARAN_HOST="$AMARAN_HOST"${AMARAN_PORT:+
export AMARAN_PORT="$AMARAN_PORT"}
exec "$HERE/amaran-cmd.sh" "$LIGHT" "$cmd" "$PROJECT" "$OUT/amaran-daemon.app"
RUN
  chmod +x "$app/Contents/MacOS/amaran-$cmd"
  codesign --force --sign - --identifier "com.arnebrockmann.amaran.$cmd" "$app" >/dev/null 2>&1 || true
  echo "built: $app"
}
# The switch bundles were the Stream Deck keys while "System -> Open" was the
# only option. Since 2026-09-12 the plugin has its own on/off action that shows
# the real state — nobody needs the bundles anymore. If you still want them
# (e.g. for Shortcuts): WITH_SWITCH_APPS=1 ./build-apps.sh
if [ "${WITH_SWITCH_APPS:-0}" = "1" ]; then
  make_switch on     "amaran ON"
  make_switch off    "amaran OFF"
  make_switch toggle "amaran Light"
else
  for c in on off toggle; do
    [ -d "$OUT/amaran-$c.app" ] && rm -rf "$OUT/amaran-$c.app" && echo "removed: $OUT/amaran-$c.app"
  done
fi
echo
echo "First step: double-click amaran-daemon.app once and allow Bluetooth."
echo "The Stream Deck keys come from the plugin (action \"On/off\")."
