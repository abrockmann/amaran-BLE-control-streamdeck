# Hardening and changes relative to upstream

This fork builds on [`wesbos/amaran-BLE-control`](https://github.com/wesbos/amaran-BLE-control)
by Wes Bos, as of `bc3a176` (2026-07-22). This document describes what the fork
changes and why — together with the measurements that led to each change. The
difference from the original is always `git diff bc3a176..main`; who contributed
what is listed in the README under "Credits".

## Review of the upstream state (2026-09-09)

Fine as it is:

- No binaries in the repo; the `vendor/PyMeshSDK.so` mentioned in `.gitignore`
  does not exist in the current state.
- No outbound network traffic in the source: no `fetch`, no HTTP client, no
  telemetry. MQTT only if you configure a broker yourself.
- Lockfile: all packages from `registry.npmjs.org`, each with an integrity hash.
- Mesh keys stay local in `lights.json` (gitignored) and are never logged.

Fixed in the fork:

| Finding | Change |
|---|---|
| HTTP daemon binds to `0.0.0.0:2708`, API key optional, CORS `*` | Default is now `127.0.0.1`; binding to a reachable address without `http.apiKey` is refused; requests with an `Origin` header (i.e. from browsers) are rejected with 403 without an API key |
| `lights.json` with netKey/appKey was written with mode 0644 | Written with `mode 0600` plus `chmodSync` |
| `execSync(\`sqlite3 "${dbPath}" …\`)` — shell interpolation | `execFileSync("sqlite3", [dbPath, sql])` |
| `serialport` (25 packages, native build) only used by the ESP32 scripts | Removed from `dependencies`; for `esp32:capture`/`esp32:probe` install `npm i serialport@12` when needed |
| `mqtt` pulled in the only runtime-relevant vulnerable packages at the time (`ws`, `ip-address`, `socks`) | Moved to `optionalDependencies`; the import in the daemon was already dynamic |

## Installation

```bash
npm ci --omit=optional
npm rebuild @abandonware/noble --ignore-scripts=false
```

Result on macOS (Apple silicon): **14 packages, 0 known vulnerabilities in the
installed tree**, no `mqtt`, no `node-gyp`.

Both lines are needed. An earlier version listed only the first one and claimed
that was enough — reproduced on a fresh machine on 2026-09-11, the whole
installation failed on it:

- **`--omit=optional` also dropped `tsx`.** The platform binary
  `@esbuild/darwin-arm64` is an optionalDependency of esbuild; without it tsx
  does not start (`The package "@esbuild/darwin-arm64" could not be found`),
  and with it nothing in this repo. It is therefore listed directly in
  `devDependencies`, pinned exactly to the esbuild version the lockfile resolves
  for tsx — it has to move along with any tsx update. On an Intel Mac the package
  is `@esbuild/darwin-x64`.
- **noble ships no prebuilds.** The npm package has no `prebuilds/` directory; an
  earlier note "noble loads anyway (bundled prebuilds)" was wrong. With npm set
  to `ignore-scripts=true`, `node-gyp-build` never runs at install time, and the
  daemon dies on load with *No native build was found for platform=darwin
  arch=arm64 … abi=137*. `npm rebuild` compiles the addon from noble's own source
  and enables install scripts **for this one package only**; the global setting
  stays untouched. Needed once per machine and again after every Node upgrade
  that changes the ABI (here Node 24, abi=137). `esbuild` stays blocked — its
  script only downloads what is now in the lockfile anyway.

Without `--omit=optional` it is 59 packages and 7 findings (6 high, 1 critical):
the `tar`/`node-gyp` chain under `@abandonware/bluetooth-hci-socket` — the Linux
path, unused at runtime on macOS, but contrary to an earlier note installed here
all the same. (An audit on 2026-09-13 no longer flagged `mqtt` itself.)

## The ESP32 bridge is the main path since 2026-09-12

The Mac daemon was never built for two computers, and it could not become that:
a BLE device accepts **one** connection, and both Macs speak as the same mesh
source `0x0001`. Whoever came first won; the other silently fell back to a
different node (see the section below).

That is not solved in code but by a device that holds the one slot
**permanently**: `esp32-firmware/` as a permanent mesh node with its own address
`0x0010`. Both Macs then talk to it over the LAN, and LAN clients don't exclude
each other.

Measured on the first run: from power-on to ready in about **6 s**; a command
over HTTP acknowledged in **127 ms**; `already provisioned from NVS` after a
restart, so the mesh keys survive a power cut. And the return channel the Mac
daemon never had works — the core patch snoops the status reply the light sends
to the provisioner, and Home Assistant shows the real state (`on`, brightness,
kelvin).

### Pointing the Stream Deck at the bridge

Since 2026-09-13 in the plugin's settings: the bridge address is a global setting
shared by all actions, the light a setting per action, picked from the list the
bridge reports. An empty address means `amaran-esp32.local`. The plugin looks in
this order: settings → `AMARAN_HOST`/`AMARAN_PORT`/`AMARAN_LIGHT` in its
environment → `paths.json` → the default name and the first light.

The switch bundles have no settings page and still take the environment:

```bash
export AMARAN_HOST=192.0.2.61 AMARAN_PORT=80 AMARAN_LIGHT=amaran_verge_max_1
LIGHT="$AMARAN_LIGHT" ./streamdeck/build-apps.sh    # bakes the host into the bundles
./streamdeck/install-plugin.sh                      # writes it to paths.json as a fallback
```

`install-plugin.sh` keeps host, port and light from the previous install when they
are not set, so a plain reinstall does not move an existing setup to the default
name.

Then retire the Mac daemon, otherwise it keeps transmitting into the mesh:

```bash
launchctl bootout gui/$UID/com.arnebrockmann.amaran-daemon
launchctl disable gui/$UID/com.arnebrockmann.amaran-daemon
pkill -9 -f amaran-daemon        # SIGTERM is not enough: it gets stuck in its shutdown path
```

Things to keep in mind:

- **The light keys differ.** The daemon derives them from `lights.json`
  (`amaranve`), the ESP32 from the device names (`amaran_verge_max_1`). Changing
  only the host switches into the void.
- **The color temperature command reads a different field on each side:** the
  daemon reads `brightness`, the ESP32 `intensity`. The plugin therefore sends
  **both** fields; each side ignores the one it doesn't know. Sending only one
  would make the dial set the color and silently drop the brightness — with
  `200 ok` from both sides.
- **The guard is not needed for a target on the network.** It replaces a stale
  *local* daemon; the ESP32 does not sleep with the Mac. Plugin and
  `amaran-cmd.sh` only call it when the host is `127.0.0.1`/`localhost`.
- **Not every `127.0.0.1` in the plugin is the daemon.** One of them is the
  WebSocket connection to Stream Deck itself and has to stay local.

### State is no longer guessed (2026-09-12)

After the switch, the Stream Deck showed two bugs with the same root: dials and
key **kept a record** of the state instead of knowing it. The record lived in
`~/Library/Application Support/amaran-streamdeck/` and went wrong as soon as
someone else switched the light — Home Assistant, the app, the button on the
fixture. It then said "off", the light was on, and a press sent `on`. To the
user: **the dial never switches off.**

The bridge knows the real state anyway — it snoops the light's status replies.
It just didn't expose it over HTTP. Now it does, and both the plugin and
`amaran-cmd.sh` ask it: when an action appears and every ~4 s. Since
2026-09-13 the bridge also does the toggling itself (`/toggle`, see next
section). Against the local daemon (which has no state query) the old record
remains.

Three details, all measured:

- **Tolerances.** The light does not report exactly what was sent: 5600 K comes
  back as 5617 K, 60 % as 59 %. Without a tolerance the display would keep
  drifting.
- **Quiet window while turning (2.5 s).** Otherwise a query that left just before
  our own command overwrites the value mid-turn.
- **Brightness and color only when "on".** The firmware takes them from the status
  reply only for lights that are on; after a restart it reports the defaults
  100 % / 4700 K for a light that is off. If the plugin took those over, the
  first turn would jump the light from 12 % to nearly 100 %.

The Stream Deck key used to be an Elgato "System → Open" action — which cannot
show any state. The plugin now has its own **"On/off"** action with two states
(gray/warm), set from the bridge's report. Stream Deck advances multi-state keys
by itself on release; the plugin resets them to the real state right away.

Don't forget: give the bridge a **fixed IP** in the router — the Stream Deck
points at the address, and a new DHCP lease would look like a defect.

### Switching latency: weak Wi-Fi and lost commands (2026-09-13)

"Switching is quite slow" had three causes. Measured with the bridge's serial
log (ESP32 on USB), `curl` timings and `ping`:

- **The bridge was on the wrong access point.** It connected at −82 dBm although a
  repeater at −61 dBm was in range. Without further settings ESP-IDF scans fast
  and takes the *first* access point with a matching name. HTTP to the bridge:
  median 613 ms, 90 % under 2.0 s, peak 5 s; Home Assistant and the router
  answered the wired Mac in under 1 ms at the same time.
- **Commands to a light that was on often got lost.** The protocol has no
  acknowledgement; `ok` means "sent". To the user a lost command looks like a
  sluggish key: nothing happens until you press again. With the old firmware,
  two of four commands never reached the Verge Max (its status reply 10 s later
  still showed the old state). Both were "off", and that stayed the pattern in
  every later series: "on" almost always arrives, "off" noticeably less often.
  The obvious explanation (not proven): a light that is on listens in intervals.
- **Two round trips per press.** Before every toggle the plugin first queried
  `/lights` and then sent `on`/`off`, each time over that Wi-Fi.

Implemented:

1. **Pick the strongest access point** (`wifi.c`: `WIFI_ALL_CHANNEL_SCAN`,
   `WIFI_CONNECT_AP_BY_SIGNAL`). Afterwards −61 dBm and HTTP median 173 ms, 90 %
   under 0.4 s, peak 0.9 s (60 requests). Cost: connecting at boot takes ~10 s
   instead of ~3 s. **Placement matters:** at its final spot (no longer next to
   the Mac) −49 dBm and HTTP median 88 ms, 90 % under 0.2 s; the Verge Max
   answered 5 of 6 status requests there. To check without a cable: `GET /status`
   reports the signal, Bluetooth reach per light and in which round on/off was
   confirmed (fields in `esp32-firmware/README.md`).
2. **Delivery check in the firmware** (`main.c`, `disp_on_off`). On/off to a
   single light is repeated until its status reply shows the wanted state. While
   that runs, the bridge does not pass replies with the old state on to Home
   Assistant and `/lights`, otherwise the display would flip back and forth.
   On/off is absolute, so a repeat after a late success changes nothing. How the
   repeats need to be spaced was shown by four variants (Verge Max, 6–10 toggles
   each, small samples):

   | Variant | "off" on 1st attempt | not confirmed |
   | --- | --- | --- |
   | command + status request at the same time | not usable: reply after 150–260 ms often showed the old state | 0 / 10 |
   | one send, status request after 200 ms | 1 / 3 | 1 / 6 (after 4 sends) |
   | three sends back to back (~270 ms) | 4 / 10 | 0 / 20 |
   | **three sends at 0/200/400 ms, status request at 600 ms** | **4 / 5** | **0 / 10** |

   Bunching them doesn't help; the repeats that worked were a few hundred
   milliseconds apart. The last variant is in use: per round three spread-out
   sends, verdict after 1 s, at most three rounds. "on" was 5/5 on the first
   attempt with it. The confirmation shows up in the log after ~0.7–0.8 s; the
   light switches at the first send that arrives. More transmissions per message
   (`net_transmit`) would be the wrong lever: the radio task sends one message
   after the other, so every message would take longer, including each dial step.
3. **`POST /lights/<key>/toggle`.** The bridge flips the light itself and answers
   with the new state (`{"ok":true,"on":false}`). The plugin needs one call instead
   of two, shows the new state immediately and takes over the answer. `on`/`off`
   over HTTP now record the state immediately as well; before, it only arrived with
   the snooped status reply ~1 s later, and a second press in that window read the
   old value.
4. **Follow-up status request only to the affected light.** After every dial step
   the bridge queried all seven lights (six of them not present here), 80 ms
   apart, blocking the timer task for over 0.5 s.

**Deliberately left alone: the coexistence preference** (`esp_coex_preference_set`,
default balanced). Preferring Wi-Fi would smooth the remaining ~50–100 ms of
power-save wait, but takes receive time away from the mesh — and that is where the
commands got lost. Only change it with a before/after measurement.

Against the local Mac daemon nothing changes: it knows neither state nor
`/toggle`, so there the plugin keeps toggling against its record.

## Device selection — three bugs that masked each other

On 2026-09-11 a second Mac could not switch the Verge Max while the first one did
so flawlessly. The search took a day, because **three** causes acted at once and
each one fixed on its own was masked again by the others:

1. **Stale node address.** `npm run setup` read address **37** from this Mac's
   amaran Desktop database; the light had long been listening on **40**. The value
   was read correctly and was still wrong — the database was old.
2. **Foreign proxy node.** The selection took the *first* candidate. So a foreign
   mesh device at **−94 dBm** won against our own light at **−42 dBm**, and every
   command silently went into a foreign network.
3. **The second Mac** occupied the right node at times (see "Limits").

On top of that, as a fourth layer, the protocol has **no acknowledgement**: all
three cases produce exactly the same picture — `{"ok":true}` from the API,
"ready" in the log, and nothing happens.

**Fixed:** the selection now sorts by **signal strength** (on a tie the proxy
service wins) and discards everything below `AMARAN_MIN_RSSI` (−85 dBm) — a node at
the edge of reception is no good as a permanent connection anyway. The choice and
its reason go to the log (`Chosen: … (−46 dBm, proxy service)`). Also, the old
version decided **once** after five seconds and gave up if nothing was there by
then; devices showing up later were still logged but never chosen — hence the
message "No Amaran lights found" right below a "Found" line. Now it decides again
on every further find once the collection time is over.

Connecting has a time limit (`AMARAN_CONNECT_MS`, 12 s). Before, it could hang
indefinitely: on 2026-09-11 for **28 minutes**, without the guard noticing.

**What was NOT the cause** — measured, not assumed, so nobody chases these again:
the sequence number (the working machine switches with *lower* values than the
failing one), the network key and the IV index (both machines produce
**byte-identical** frames for the same sequence number), and the missing Filter
Status reply (it is missing on **both** machines, including the one that
switches). Tools for this are `scripts/frame-rebuild.ts` and
`scripts/frame-decode.ts`.

## Bluetooth permission on macOS (the real stumbling block)

CoreBluetooth kills every process whose executing binary has no
`NSBluetoothAlwaysUsageDescription` with SIGABRT (exit 134) — **without** asking
first. On this Mac the system writes neither an error on stderr nor a crash
report; the process is simply gone.

What counts is the binary actually running, not the app bundle around it:

- A bundle that runs `npx tsx …` fails. The program touching Bluetooth is then the
  version manager's `node` **outside** the bundle. Tested: double-click, no
  dialog, silent exit.
- A bundle with a **copied `node` in `Contents/MacOS/`** that starts the daemon
  directly via `--import tsx` works. That is exactly what
  `streamdeck/build-apps.sh` builds; the bundle is ad-hoc signed so TCC sees a
  stable identity.

## Operation (local Mac daemon)

```
streamdeck/build-apps.sh          # builds the bundles into ~/Applications
```

| Bundle | Job | Needs Bluetooth |
|---|---|---|
| `amaran-daemon.app` | holds the mesh connection and the HTTP API on 127.0.0.1:2708 | yes, grant once |
| `amaran-on.app` / `amaran-off.app` | only send HTTP to the daemon (built with `WITH_SWITCH_APPS=1`) | no |
| `amaran-toggle.app` | toggles, state recorded locally (built with `WITH_SWITCH_APPS=1`) | no |
| Plugin `com.arnebrockmann.amaran` | dials for brightness and color temperature, on/off key | no |

The switch bundles used to be added in Stream Deck via "System → Open"; the plugin
has its own on/off action now. A switch command takes about 0.4 s, because the
daemon keeps the BLE connection open; without the daemon it would be about 10 s
per command.

`streamdeck/com.arnebrockmann.amaran-daemon.plist.in` runs
`streamdeck/daemon-guard.sh` at login and every minute after. The guard has three
jobs:

1. **Daemon missing** → start it.
2. **Daemon older than the Mac's last wake-up** → restart it.
3. **Daemon running but not answering on its API** → replace it as well.

Two runs can no longer overtake each other: the one-minute timer and the call from
`amaran-cmd.sh` (before every key press) occasionally coincide, and a repair run
can take up to a minute — on 2026-09-11 one run was still waiting for the API while
the other had already started a second daemon. A lock (`mkdir`, atomic, with the
PID inside for orphaned cases) makes the second run step back quietly. The start
window grew from 30 to 60 s (`AMARAN_START_WAIT`), because a start after about 40 s
was wrongly reported as a failure; conversely, the guard now gives up immediately
when the daemon has exited again while connecting — 3 s instead of 60.

Job 2 is the more important one and happened in the wild on 2026-09-10: after a
night asleep the daemon was still running, the API answered in 14 ms, the light
was still listed as a connected Bluetooth device — and yet it reacted to nothing.
The GATT connection had survived the sleep, the **mesh session on top of it had
not**: proxy filter and sequence numbers dated from before the sleep. Commands were
still sent, none were accepted anymore, and because the protocol has no
acknowledgement, the API dutifully reported `ok`.

The daemon cannot notice this by itself. The only reliable hint is the wake-up
time from `sysctl kern.waketime`; if the process is older, it is replaced. It is
always started through `open -a`, because only LaunchServices attributes the
Bluetooth permission to the bundle.

Job 3 was added on 2026-09-11, after exactly the situation the guard is meant for
had occurred — and it did not see it: the daemon hung in a BLE connection attempt
to a foreign device that had matched the scan by name only. The process was alive,
so the guard was satisfied; socket and API never came up. For 28 minutes no
command got through, and the guard log had not a single line about it. Until then
the API probe only ran right after a start the guard had triggered itself.

Two safeguards against false alarms, both reproduced:

- **Grace period `AMARAN_API_GRACE` (90 s).** A fresh daemon first scans and
  connects — 13 s in the good case, seen at over 30 s too. Its API is rightly
  missing during that time. Without the grace period the guard would kill every
  start on its next run. Tested with a silent process 2 s old: left alone, no log
  entry. With `AMARAN_API_GRACE=0` the same case triggers immediately — that is
  how the branch can be tested.
- **Second probe after three seconds.** The daemon is single-threaded; an ongoing
  mesh send can block the answer briefly. Only silence twice counts as dead, so a
  false alarm doesn't throw away a working connection.

Cost: the probe itself 8.1 ms (median of seven runs), the whole guard script with
it 31–79 ms. Against 409 ms per BLE command that does not show, not even in the key
path, which calls the guard beforehand anyway.

In addition, `amaran-cmd.sh` asks the guard before every key press. That normally
costs about a tenth of a second (0.50 s instead of 0.41 s per press) and keeps the
first press after opening the lid from fizzling out.

For the dials the same call would be too expensive. Measured per call:

| Variant | Cost | Context |
|---|---|---|
| whole guard script (bash + ps + sysctl + API probe) | 39.8 ms | in 1 % steps, 99 ticks are a full sweep → four seconds of process spawning on the plugin's only thread |
| only `sysctl kern.waketime` | 1.9 ms | |
| time-jump detection in Node | ~0 ms | for comparison: a BLE command takes 409 ms |

The plugin therefore uses the third variant: its two-second timer, which runs
anyway, compares the elapsed wall-clock time with the expected one. A jump of more
than ten seconds means sleep — then, and only then, the guard is called once (path
from `paths.json`, which `install-plugin.sh` puts next to the installed plugin).
While it runs, the latest value waits in the queue instead of going into the void.
That way the plugin notices the wake-up after about two seconds instead of up to a
minute.

Tested with `WAKE_DRIFT_MS=-1`, which makes every timer round trigger: the guard is
found, started and exits (~70 ms including process start). With the regular
threshold there are zero triggers while idle.

**About the installer:** `install-plugin.sh` now quits Stream Deck reliably. Stream
Deck acknowledges the Apple event with error −128 and sometimes quits anyway,
sometimes not; the return value is useless. The script therefore waits for the
process to disappear and otherwise falls back to SIGTERM. And it reports success
only once a **new** plugin process is really running — otherwise you get the state
that looks like "installed" and isn't: new file on disk, old code in memory.

Tested, all three paths: no action without a reason; simulated wake-up
(`WAKE_OVERRIDE`) → restart including a rebuilt mesh session in 12 s; killed
daemon → back in 9 s.

To disable:

```
launchctl bootout gui/$UID/com.arnebrockmann.amaran-daemon
rm ~/Library/LaunchAgents/com.arnebrockmann.amaran-daemon.plist
```

## Pairing without the amaran app (looked into, not pursued)

Question from 2026-09-13: can new lights be added to a mesh without the amaran or
Sidus Link app? Not pursued, because the app is only needed once when setting up
— that is how Sidus Link on the iPhone works as well. What was found:

- **The lights speak standard Bluetooth Mesh** (proven by upstream: proxy service
  `0x1828`, standard key derivation, secure network beacons). Only the `0x26`
  command on top is proprietary.
- **Official pairing** is a "Bluetooth reset" on the fixture, after which the app
  adds it. After the reset the light is a regular unprovisioned mesh device.
- **amaran Desktop's database** (`fixtures` table) stores per light `device_key`,
  `device_uuid` and `composition_data` — the results of standard provisioning and
  configuration — plus `fast_provision_supported` (Telink's optional bulk
  provisioning). There is no column for a per-device OOB secret.
- **The open question** is the provisioning authentication: with "No OOB" any
  standard provisioner (the ESP32 itself, or Nordic's nRF Mesh app) should work; a
  fixed static OOB value known only to the app would block that. Not tested.
- **Trade-off:** a light belongs to one mesh. In a mesh of your own, the amaran app
  no longer sees it (control, firmware updates) until it is reset and re-added.
- **How to test it:** Bluetooth-reset one light, check in nRF Mesh whether it shows
  up as unprovisioned and which authentication it offers, provision without OOB,
  bind an AppKey to the Telink vendor model, export the network and try `0x26`
  on/off through the bridge. nRF Mesh only provisions over GATT; if the light only
  offers PB-ADV, the ESP32 would have to do it.

## Security audit (2026-09-13)

Three independent reviews (firmware; Stream Deck plugin and scripts; Mac daemon,
publishing tools and repo hygiene), every finding checked before it was acted on.
No secret was ever committed — neither in the current tree nor in the history.

**Fixed** (each with a test that fails against the previous code, where one was
possible):

| Where | Finding | Fix |
| --- | --- | --- |
| Plugin | Light keys went unchecked into state file names: a fake bridge or a shared Stream Deck profile with the key `/../../../../.ssh/id_ed25519` made the next press overwrite that file | keys must match `^[A-Za-z0-9_-]{1,64}$`; test reproduces the write with the old code |
| Plugin | A trickled answer kept a light busy forever; answers had no size limit | hard deadline, 256 KB cap |
| Bridge | Any web page could switch the lights with a simple cross-site POST (no preflight) | writes with an `Origin` header get 403 |
| Bridge | DNS rebinding could read `/status` (BSSID locates a home) and `/lights` | `Host` must be an IP, `DEVICE_ID` or `DEVICE_ID.local` |
| Bridge | Refused OTA requests still read the whole announced body; idle sockets could lock everyone out | refusals close the socket; LRU purge |
| Mac daemon | HTTP ran any socket command, including `stop`; no Host check; unlimited body; key compared with `!==`; README example key `my-secret` | command allowlist, loopback Host check, 8 KB cap, constant-time compare, generated key in the docs |
| Guard | Lock in `$TMPDIR` (not shared between launchd and plugin), `pgrep` matched other accounts, any answer on the port counted as the daemon | lock in `~/Library/Caches`, `pgrep -U`, probe requires the daemon's own answer |
| Pack script | `npx @elgato/cli@1.9.0` pinned only the CLI, not its 68 dependencies | lockfile in `streamdeck/pack/`, `npm ci --ignore-scripts` |
| Publishing gate | Seven ways past it (missing config files → silent "clear", hex values after `_`/other separators/raw bytes, non-UTF-8 files, marker case, IPv4 at sentence end, symlinks) | all blocked, each bypass covered by the gate's selftest |
| Repo | Copies like `lights.json.old`, `wifi_config.h~`, `sdkconfig.bak` were not ignored; firmware image and `sdkconfig` readable by other accounts on the Mac | broad ignore patterns; repo folder `chmod 700` (local, not in git — repeat on a new machine) |

**Accepted risks** — documented rather than fixed, with the reason:

- **The daemon bundle runs code from the repo** with the Bluetooth permission. A
  process running as the same user could change `src/` or start the bundle with
  `NODE_OPTIONS`. Whoever can do that already runs code as that user; the extra
  gain is Bluetooth access.
- **Light control on the LAN has no authentication** (bridge and upstream design).
  Web pages are blocked now; a device on the network is not.
- **Shared state in the firmware is not locked** between the MQTT, HTTP, mesh and
  timer tasks. Worst case a briefly mixed state in Home Assistant; no memory
  safety impact.
- **A fake Bluetooth proxy nearby** could be chosen by the Mac daemon and swallow
  commands (partly upstream). No keys are exposed.
- **Physical access**: flash encryption and secure boot are off, so the chip's
  flash holds the Wi-Fi password and mesh keys in plain form.

**Done after the audit, same day — OTA hardening.** MQTT runs unencrypted
(`mqtt://`), and the OTA token was derived from the MQTT password: someone
intercepting traffic on the LAN could read the password at a reconnect, compute
the token and flash any firmware, because images were not signed. Now:

- the OTA token is a random value of its own;
- images must be signed (ECDSA, `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`,
  checked on update). The switch went on over OTA: the signed firmware was
  uploaded with the old token, and from then on only the new token and signed
  images are accepted.

Token and signing key are kept in the password manager as well; restoring the key
from there reproduces the file byte for byte. Losing both would not be fatal — a
USB flash accepts any image.

Checked on the bridge after the switch: old token 403, random token 403, new token
with the previous unsigned image `invalid image` (400, firmware unchanged), new
token with the signed image accepted and booted. Locally, `espsecure.py
verify_signature` accepts the signed image and rejects the unsigned one.

**OTA rollback — done 2026-09-13**, with one USB flash for the bootloader. A new
image is confirmed only after a minute with HTTP up. Tested with a signed image
that aborted 20 s after boot: the bootloader went back to the previous image
(serial log and `/status` agree), and a normal update went from `pending_verify`
to `valid` after a minute. Before the flash the whole 4 MB were read out as a
backup.

**MQTT over TLS — done 2026-09-14.** A test build ran an extra MQTT-over-TLS
client next to the normal plain one, against a test broker on the Mac:

| mbedTLS buffers | free with TLS | lowest after handshake | lowest during an upload |
| --- | --- | --- | --- |
| default | 36 KB | 18.5 KB | **2.2 KB** |
| dynamic, P-256 certificate | 59 KB | 39.5 KB | 31 KB |
| dynamic, RSA-2048 certificate | 58 KB | 27 KB | 11 KB |

So: dynamic buffers (`sdkconfig.defaults`) and an EC certificate. The broker's key
was generated on Home Assistant and never left it; only the certificate request
and the signed certificate moved. The bridge verifies the broker against its own
CA, including the IP address. The plain ports 1883 and 1884 are closed on the
network — only Home Assistant itself (internally) and the bridge used them. In
normal operation with TLS the bridge keeps about 73 KB free.

Checked: certificate verification (and a wrong IP refused), MQTT login over TLS
(accepted; a wrong password refused), the broker logging the bridge's TLS session,
Home Assistant receiving the bridge's state messages, the closed ports from the
LAN, and the bridge reconnecting by itself after a broker restart.

**New MQTT password — done 2026-09-14.** The old one had travelled in plain text
until the switch. The new one is 256 random bits; the broker's add-on hashes it
itself. Checked: the broker refused the bridge 24 times while it still had the old
password and accepted it right after the change, a wrong password is refused, and
Home Assistant receives the bridge's state messages again. The old password itself
could not be tried — it was overwritten everywhere it was stored.

## Limits

- **Only one mesh client at a time.** If amaran Desktop runs in parallel, both
  fight over the BLE connection to the relay node. Either — or. Measured on
  2026-09-11, and it applies just as much to **two Macs**: a BLE device accepts one
  connection. When the second Mac held the node, the first could no longer get
  through — and silently fell back to another candidate. If the daemon is killed
  hard, the light doesn't know and keeps the connection until its own supervision
  timeout; new connection attempts fail until then. (The ESP32 bridge avoids this:
  it is the one permanent client.)
- **No state feedback to the Mac daemon.** The lights send their status reply to
  the original provisioner (`0x0001`), never to the asker. `"result":"ok"` means
  "sent", not "arrived"; the `retry` lines in the log are blind repeats, not an error
  signal. That is why the daemon setup used two keys (ON/OFF) instead of one toggle
  key. `amaran-toggle.app` exists anyway, because the key pad was full; it remembers
  the last state sent under `~/Library/Application Support/amaran-streamdeck/`, and
  the fixed ON/OFF keys write the same record. It only gets out of step when the
  light is switched some other way (buttons on the fixture, power cut) — then a press
  on ON or OFF brings both back in line. The ESP32 bridge does know the real state.
- Tested with the **amaran Verge Max** (address 0x0028): switches via opcode `0x26`.
  The other lights in the mesh are configured but not tested.

## Stream Deck plugin

`streamdeck/plugin/com.arnebrockmann.amaran.sdPlugin` replaces the two dials of the
amaran plugin, which are dead without amaran Desktop, and adds an on/off key.
Install with `streamdeck/install-plugin.sh`, or build a double-click package with
`streamdeck/pack-plugin.sh`. Test without Stream Deck and without a light:
`npm run test:plugin`.

- **No third-party dependencies.** Stream Deck's Node runtime (v20) has no global
  `WebSocket`, so the client in `bin/plugin.js` is implemented by hand (RFC 6455,
  only the part needed). No `npm install`, no `node_modules`, nothing to audit. The
  icons are generated by `plugin/make-icons.py` from code instead of being taken
  from somewhere.
- **No Bluetooth.** The plugin only talks HTTP — to the ESP32 bridge, or to the
  local daemon on `127.0.0.1:2708`, which then stays the only process with mesh
  access. If the local daemon is unreachable, the plugin starts the daemon bundle
  via `open -a` (at most once a minute).
- **Controls.** Turning changes brightness (±1 %, adjustable via
  `AMARAN_BRIGHTNESS_STEP`) or color temperature (±100 K, range **2700–6500 K** per
  the Verge Max manual — not the 2500–7500 K mentioned elsewhere in the project,
  which applies to other models; adjustable via `AMARAN_CCT_MIN`/`AMARAN_CCT_MAX`).
  Pressing the brightness dial switches on/off, pressing the color dial goes back to
  5600 K. The touch strip shows title, value and a bar. Names and touch-strip texts
  follow Stream Deck's language (`en.json` and `de.json`; other languages fall back
  to the English texts in `manifest.json`).
- **Settings page** (`ui/settings.html`, since 2026-09-13). Bridge address and
  light are picked there instead of in environment variables. The page cannot
  fetch the light list itself — the bridge sends no CORS headers — so it asks the
  plugin (`sendToPlugin`), which answers with the list, the capabilities and the
  light the action uses right now (`sendToPropertyInspector`). The local daemon
  (`127.0.0.1:2708`) lists its lights too — `GET /lights` answers
  `{ ok, lights: [{ key, name, mac, address }] }`, without state or color
  temperature range. Only when the daemon is not running does a text field for
  the light key take the place of the list. (The first version of the page
  claimed the daemon had no list and always showed the text field; against the
  daemon, a dial without a saved light then sent nothing at all.)
- **Several lights.** The plugin keeps state per light: actions on the same light
  share it, different lights never block each other. The color temperature range
  comes per light from `GET /lights` (`cct_min`/`cct_max`); the fixed range above
  is only the fallback for the local daemon.
- **Pitfall, caught before it shipped:** the first version of the settings page
  preselected the *first* light when an action had no light saved. An action from
  an older install, which controlled a light through `paths.json`, would have
  switched to a different light just by opening its settings — the bridge here
  lists seven lights, and the Verge Max is the last. The plugin now sends the
  light it resolved, and the page preselects that one; `test-plugin.mjs` covers
  it, and against the old code exactly those checks fail.
- **Packaging.** `pack-plugin.sh` runs the Elgato CLI pinned to 1.9.0. Its
  validator requires a four-part version (`1.1.0.0`) and a category matching the
  plugin name. A package installs without `paths.json`, so it only knows the
  guard's default path — fine for the bridge, and for the local daemon
  `install-plugin.sh` is the right way.
- **Debouncing.** A mesh command takes about 0.4 s, and turning produces many
  events. The display follows immediately, sending is debounced (90 ms) and never
  overtakes itself: while a request runs, only the latest value waits.
- **In step with the keys (local daemon).** On/off is kept in the same file the
  switch bundles write (`~/Library/Application Support/amaran-streamdeck/`); the
  plugin watches it on its two-second timer and updates the display. Brightness and
  color temperature sit next to it in `model-<light>.json`. Against the bridge the
  plugin reads the real state instead.
- **Verge Max values** (manufacturer's manual, as of 2026-09-09): CCT 2700–6500 K,
  brightness 0–100 % continuous. An extended CCT range, a kelvin step size and a
  green/magenta correction are **not documented** for this model — so the `gm` value
  in the API call stays 0. The plugin regulates brightness from 1 %, not 0 %, so
  "off" clearly goes through the key press and the recorded state doesn't fall apart.
  Sources: https://amarancreators.com/pages/amaran-verge-max and the manual PDF
  (pages 5 and 7).
- **Limit (local daemon).** Because the light does not report its state to the
  daemon, the plugin keeps the model there. If someone turns the knob on the fixture,
  it shows wrong values briefly until the next command from here.

**Pitfall, learned the hard way:** Stream Deck sends WebSocket pings and expects a
pong **echoing the payload**. An empty pong is not enough — the connection is then
considered dead (`Plugin disconnected with reason 'Socket unresponsive'`), the process
is restarted every ten seconds and the plugin is disabled as unstable after a few
rounds. Anyone rebuilding this client has to send `encodeRaw(0xa, payload)`.

**Pitfall: a retry loop against a stopped daemon** (fixed 2026-09-13). A refused
connection was queued again and retried after 0 ms. With the daemon stopped that
made about 40,000 attempts a minute — 81 minutes of it on 2026-09-11, 2.9 million
log lines, 239 MB. Now the latest value waits 1.5 s between attempts and is
dropped after 20; the log is rotated at 5 MB, keeping one previous file.
`test-plugin.mjs` counts the attempts: 3 in three seconds, against 2093 with the
old code.

**Pitfall: exit code 1 on every shutdown** (fixed 2026-09-13). Each time Stream
Deck quit, the plugin logged `socket error: write after end`. The pong for a ping
that arrived after Stream Deck's close frame went straight to the already ended
socket — the one write that bypassed the check in `sd.send`. All writes now go
through one guarded function, and frames after a close are ignored. Verified on
the Stream Deck +: the log now ends with `connection to Stream Deck closed`. Socket
errors also name the calling code now, because the message alone could not be
placed.

Verified on a Stream Deck +: both actions register at start (`action active: …`), and
a change made through the switch bundles is picked up (`state changed from outside:
on`).

## Moving to another Mac

```bash
git clone https://github.com/abrockmann/amaran-BLE-control-streamdeck.git
cd amaran-BLE-control-streamdeck
npm ci --omit=optional
npm rebuild @abandonware/noble --ignore-scripts=false   # builds the BLE addon, see "Installation"
```

Then get `lights.json` — it is gitignored and **contains the mesh keys**:

- either `npm run setup` on the target machine; that requires amaran Desktop to be
  installed there and to know the mesh,
- or copy the file over a secure channel (e.g. a password manager, not by mail or
  chat) and then `chmod 600 lights.json`.

Then:

```bash
./streamdeck/build-apps.sh           # copies THIS machine's node into the bundle
./streamdeck/install-plugin.sh       # Stream Deck plugin
./streamdeck/install-daemon-agent.sh # autostart and revival
```

Finally, double-click `~/Applications/amaran-daemon.app` once and confirm the
Bluetooth prompt.

**What cannot be copied along:**

- **The Bluetooth permission.** It belongs to the bundle on each machine and has to be
  granted there once.
- **The Stream Deck layout.** Either export the profile in the Stream Deck app and
  import it on the target machine — the plugin has to be installed there **first**,
  otherwise the actions show as missing — or set up the keys and dials there again.
- **The LaunchAgent paths.** That is why the plists live in the repo as `.plist.in`
  with `__HOME__`; the install scripts fill in the real path. launchd expands neither
  `$HOME` nor the tilde.

**Two constraints:** only **one** mesh client may run at a time — not two Macs with
the daemon at once, and not the daemon and amaran Desktop in parallel. And the
controlling machine has to be within Bluetooth range of the light.
