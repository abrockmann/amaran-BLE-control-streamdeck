# Amaran Light Controller

> **Built on [wesbos/amaran-BLE-control](https://github.com/wesbos/amaran-BLE-control)
> by Wes Bos (MIT).** This fork adds a Stream Deck plugin, reliability and latency
> work on the ESP32 bridge, and hardening of the Mac daemon. The fork's operating
> notes and measurements are in [HARDENING.md](HARDENING.md). What comes from whom
> is listed under [Credits](#credits).

Wifi + Direct Bluetooth Mesh control of Amaran studio lights — **no Amaran Desktop app required**.

Two ways to drive the lights:

- **macOS / Node.js** (this repo's `src/`) — control from your Mac over BLE. See below.
- **ESP32 standalone node** ([`esp32-firmware/`](esp32-firmware/README.md)) — a
  always-on hardware bridge that joins the mesh and exposes the lights to
  **Home Assistant (MQTT)** and an **HTTP API**, no Mac required at runtime.

## Stream Deck + ESP32 bridge (this fork)

What this fork is built around: an ESP32 joins the lights' mesh and stays on, and
a Stream Deck controls the lights over your network — brightness and color
temperature on the dials, on/off on a key, always showing the lights' real state.
No Mac has to hold a Bluetooth connection, and several computers can share the
bridge.

**You need**

- amaran lights set up in the amaran app, and **amaran Desktop on a Mac** that
  shows them: the config generator reads the mesh keys and light addresses from
  its database. After setup, neither app has to run.
- An ESP32 board (ESP-32S / WROOM-32, 4 MB flash) and a USB data cable.
- A 2.4 GHz Wi-Fi network. The Stream Deck only talks to the bridge's HTTP API;
  an MQTT broker is needed only for Home Assistant.
- Stream Deck 6.5 or later on macOS 12 or later. Brightness and color temperature
  need a model with dials (Stream Deck +); the on/off key works on any model.

**Steps**

1. Build and flash the bridge — short version under
   [Flashing the ESP32](#flashing-the-esp32), details in
   [`esp32-firmware/README.md`](esp32-firmware/README.md#one-time-setup). Check the
   generated light addresses before you build; the warning there explains why.
2. Check that it is up: `curl http://amaran-esp32.local/lights` lists your lights.
3. Install the plugin: `streamdeck/pack-plugin.sh` builds
   `streamdeck/dist/com.arnebrockmann.amaran.streamDeckPlugin` — double-click it.
   Or run `streamdeck/install-plugin.sh`, which copies the plugin directly and
   restarts Stream Deck.
4. In Stream Deck, drag **Brightness** or **Color temperature** onto a dial and
   **On/off** onto a key (category *amaran lights*). In the action's settings,
   leave **Bridge** empty for `amaran-esp32.local` (or enter its IP address) and
   pick the **Light**. Each action can control a different light.

Prefer to have an AI assistant walk you through it? [SETUP-PROMPT.md](SETUP-PROMPT.md)
is a prompt for Claude, ChatGPT or a coding agent, with rules that keep your
Wi-Fi password and mesh keys out of the chat. It covers both ways, with and
without an ESP32.

## Stream Deck without an ESP32 (Mac only)

No extra hardware: a daemon on your Mac holds the Bluetooth connection to the
lights instead of amaran Desktop, and the plugin talks to it on `127.0.0.1:2708`.

**Compared with the bridge**

- **No real state.** The lights report their state only to the app that set them
  up, so the plugin shows what it last sent. If a light is switched some other
  way, the display is off until the next command from the Stream Deck.
- **One mesh client at a time:** not alongside amaran Desktop, and not from two
  Macs — a light accepts one Bluetooth connection.
- **The Mac has to be awake and in Bluetooth range.** After sleep the daemon is
  replaced automatically, which takes about 12 s.

**Steps** — amaran Desktop with your lights on this Mac is needed once, as above.

1. Install: `npm ci --omit=optional`, then
   `npm rebuild @abandonware/noble --ignore-scripts=false` (why both:
   [Quick Start](#quick-start)).
2. `npm run setup` reads amaran Desktop's database and writes `lights.json`. It
   holds the mesh keys — never commit or share it. Then quit amaran Desktop.
3. `streamdeck/build-apps.sh` builds `~/Applications/amaran-daemon.app`, and
   `streamdeck/install-daemon-agent.sh` starts it now, at login and after sleep.
   On the first start macOS asks for Bluetooth access — allow it.
4. Check: `curl http://127.0.0.1:2708/lights` lists your lights.
5. `AMARAN_HOST=127.0.0.1 streamdeck/install-plugin.sh` — use this rather than the
   double-click package, which doesn't know where the daemon's guard lives.
6. In Stream Deck, add the actions as above; the **Light** list in their settings
   shows the daemon's lights.

Background and measurements: [HARDENING.md](HARDENING.md#operation-local-mac-daemon)
and its [limits](HARDENING.md#limits).

## Quick Start

```bash
npm ci --omit=optional                                  # lockfile install, no vulnerable build-only chain
npm rebuild @abandonware/noble --ignore-scripts=false   # builds the BLE addon
npm run setup        # reads your amaran.db and writes lights.json (one time)

# Slow path (~10s startup per command, no daemon needed):
npm run mesh:on
npm run mesh:off

# Fast path (~0.6s per command):
npm run daemon:start    # connect once, stay connected
npm run mesh:on         # instant
npm run mesh:brightness 75
npm run mesh:off
npm run daemon:stop
```

Why two install steps: `--omit=optional` keeps out a build-only dependency chain
with known vulnerabilities, and `npm rebuild` compiles the Bluetooth addon even
where npm's `ignore-scripts` is set (without it the daemon dies with *No native
build was found*). `--omit=optional` would also drop esbuild's platform binary,
which is why `package.json` lists `@esbuild/darwin-arm64` directly — on an Intel
Mac, change it to `@esbuild/darwin-x64`. Details: [HARDENING.md](HARDENING.md#installation).

## Setup

`npm run setup` reads `~/Library/Application Support/amaran Desktop/*/amaran.db`
and writes a `lights.json` config file. Run it once after installing.

If the Amaran app isn't installed, run setup and enter your mesh keys manually.

## Commands

```bash
npx tsx src/cli.ts on
npx tsx src/cli.ts off
npx tsx src/cli.ts brightness 75
npx tsx src/cli.ts cct 80 5600          # 80% brightness, 5600K
npx tsx src/cli.ts cct 80 5600 15       # with GM +15 (green bias)
npx tsx src/cli.ts hsi 80 45 60         # hue 45°, saturation 60%

# Target a specific light:
npx tsx src/cli.ts brightness 50 key
npx tsx src/cli.ts on back
npx tsx src/cli.ts off halo

# Daemon management:
npx tsx src/cli.ts start    # start background daemon
npx tsx src/cli.ts stop     # stop daemon
npx tsx src/cli.ts lights   # list configured lights

# Interactive REPL (keeps connection open):
npx tsx src/cli.ts
```

Or use the npm shortcuts: `mesh:on`, `mesh:off`, `mesh:brightness`, `mesh:cct`, `mesh:hsi`.

## npm Scripts

| Script                   | Description                                                |
| ------------------------ | ---------------------------------------------------------- |
| `setup`                  | Run setup wizard (one time)                                |
| `daemon:start`           | Start daemon (also serves HTTP + MQTT if configured)       |
| `daemon:stop`            | Stop daemon                                                |
| `mesh:on`                | Turn all lights on                                         |
| `mesh:off`               | Turn all lights off                                        |
| `mesh:brightness <n>`    | Set brightness 0-100                                       |
| `mesh:cct <b> <k> [gm]`  | CCT: brightness 0-100, kelvin 2500-7500, GM -10..+10       |
| `mesh:hsi <b> <h> <s>`   | HSI: brightness, hue 0-360°, saturation 0-100              |
| `scan`                   | Scan for BLE devices                                       |
| `discover <addr>`        | Inspect a device's services                                |
| `gen-config`             | Write `esp32-firmware/main/mesh_config.h` from `amaran.db` |
| `mqtt:watch`             | Subscribe to the `amaran/.../state` topics                 |
| `esp32:probe -- "<cmd>"` | Send a REPL command to the ESP32, decode its replies       |
| `esp32:capture`          | Trigger a refresh on the ESP32, dump raw serial            |

After any command, the controller now also sends a Telink status-request so
fixtures broadcast their new state immediately — the ESP32 bridge and the
desktop app pick up the change right away instead of on the next poll.

## How It Works

**`src/cli.ts`** — main entry point. If the daemon is running, sends commands
over a Unix socket (instant). Otherwise connects directly (slow startup).

**`src/daemon.ts`** — background process that stays BLE-connected and accepts
commands via `/tmp/amaran-light.sock`. Start with `npm run daemon:start`.

**`src/mesh-controller.ts`** — full BLE Mesh stack (crypto, proxy protocol,
command encoding). All physical controls use **Telink proprietary opcode `0x26`**,
originally reverse-engineered from the amaran Desktop app's PyMeshSDK and now
implemented natively here (and in the ESP32 firmware's `telink.c`).

**`src/setup.ts`** — reads `amaran.db` to extract mesh keys and light addresses,
writes `lights.json`.

Standard BLE Mesh models (Generic OnOff, Light Lightness) respond correctly but
are decoupled from the physical LED output. Opcode `0x26` is what actually
controls sleep/wake, brightness, CCT, and HSI.

## Config (`lights.json`)

Generated by `npm run setup`. Gitignored — each user generates their own.

```json
{
  "netKey": "...",
  "appKey": "...",
  "relayHub": "A4:C1:38:13:41:38",
  "lights": [
    {
      "key": "key",
      "name": "Key Light",
      "mac": "A4:C1:38:13:41:38",
      "address": 2
    },
    {
      "key": "back",
      "name": "Back Light",
      "mac": "A4:C1:38:13:30:86",
      "address": 4
    },
    {
      "key": "halo",
      "name": "Halo 100x",
      "mac": "A4:C1:38:56:8C:EF",
      "address": 6
    }
  ]
}
```

See `lights.example.json` for the template.

## Command Payload Format (opcode `0x26`)

All commands use a 10-byte payload: `[checksum, ...packed_bits..., cmd_value, cmd_type]`

| Mode       | `cmd_type` | Notes                                                           |
| ---------- | ---------- | --------------------------------------------------------------- |
| On/Off     | `0x8C`     | `byte[8]` = 0x01 on / 0x00 off                                  |
| Brightness | `0x8F`     | intensity 0-1000; low 2 bits at `byte[7]`, upper 8 at `byte[8]` |
| CCT        | `0x82`     | kelvin packed as `(k+24)&0x3FF` at bits 52-61; GM at bits 43-51 |
| HSI        | `0x81`     | hue (9 bits) at bits 53-61; saturation (7 bits) at bits 46-52   |

See `DIRECT-BLE-CONTROL.md` for full reverse-engineering notes.

---

## HTTP REST API

When the daemon is running, a REST API is available at `http://localhost:2708`
(configurable via `lights.json`).

```bash
# List lights / health check
curl http://localhost:2708/

# Turn all on/off
curl -X POST http://localhost:2708/lights/on
curl -X POST http://localhost:2708/lights/off

# Target a specific light (use key from lights.json)
curl -X POST http://localhost:2708/lights/keylight/on
curl -X POST http://localhost:2708/lights/keylight/brightness \
  -H 'Content-Type: application/json' -d '{"value": 75}'

curl -X POST http://localhost:2708/lights/all/cct \
  -H 'Content-Type: application/json' \
  -d '{"brightness": 80, "kelvin": 5600, "gm": 10}'

curl -X POST http://localhost:2708/lights/all/hsi \
  -H 'Content-Type: application/json' \
  -d '{"brightness": 80, "hue": 45, "saturation": 60}'
```

Configure port and optional API key in `lights.json`:

```json
{
  "http": { "port": 2708, "host": "0.0.0.0", "apiKey": "<output of: openssl rand -hex 32>" }
}
```

Without `apiKey` the daemon only listens on `127.0.0.1`. The key travels in plain
HTTP, so anyone who can watch your network traffic can read it — use a long random
key, and a different one from any other password.

## Home Assistant Integration

Via MQTT discovery. The Mac daemon needs the optional `mqtt` package for this —
install with plain `npm ci` instead of `npm ci --omit=optional`. (The ESP32
bridge below has MQTT built in and needs nothing from npm.)

1. Install Mosquitto on the HA host (Add-on store)
2. Add to `lights.json`:

```json
{
  "mqtt": {
    "broker": "mqtt://homeassistant.local:1883",
    "username": "...",
    "password": "..."
  }
}
```

3. Run `npm run daemon:start` — MQTT connects automatically when the config is present

Each light auto-appears in HA as a **Light entity** with on/off, brightness slider,
color temperature, and HSI color — no YAML config required. State stays in sync
regardless of whether you use HA, the CLI, HTTP, or the REPL.

> The always-on **ESP32 bridge** (below) is the recommended way to run this with
> Home Assistant — it doesn't need your Mac running and reflects changes made
> from the desktop/iOS app too. See [`esp32-firmware/`](esp32-firmware/README.md).

## Flashing the ESP32

The ESP32 firmware is a standalone, always-on bridge that joins the mesh and
exposes the lights to Home Assistant (MQTT) and an HTTP API — no Mac needed at
runtime. Full details in [`esp32-firmware/README.md`](esp32-firmware/README.md);
the short version:

```sh
# 1. Install ESP-IDF v5.3.x and source it into your shell.
. ~/esp/esp-idf/export.sh

# 2. Apply the required BLE Mesh core patch (enables inbound state sync).
cd ~/esp/esp-idf
git apply /path/to/amaran-light/esp32-firmware/patches/0001-amaran-net-recv-status-snoop.patch

# 3. Generate the mesh config from the amaran Desktop DB (run from repo root).
cd /path/to/amaran-light
npm ci --omit=optional
npm run gen-config

# 4. Fill in Wi-Fi + MQTT credentials.
cd esp32-firmware
cp main/wifi_config.h.example main/wifi_config.h
$EDITOR main/wifi_config.h

# 5. Build and flash (CP210x port shown; Ctrl+] exits the monitor).
idf.py set-target esp32                          # first build only
idf.py -p /dev/cu.SLAB_USBtoUART flash monitor
```

`main/mesh_config.h` and `main/wifi_config.h` are **key-bearing and
`.gitignore`'d — never commit them.**

## Credits

| Part | Author |
| --- | --- |
| Mac BLE mesh client (`src/`), config generator and ESP32 helper scripts (`scripts/generate-config.ts`, `scripts/esp32-*.ts`, `scripts/mqtt-watch.ts`), ESP32 mesh node firmware (`esp32-firmware/`: self-provisioning, BLE Mesh core patch, MQTT with Home Assistant discovery, HTTP API), `DIRECT-BLE-CONTROL.md` | **Wes Bos** — [wesbos/amaran-BLE-control](https://github.com/wesbos/amaran-BLE-control) |
| Reverse-engineered Telink `0x26` packet formats and the status-request refresh trick, reused by the ESP32 firmware | **Aaron's macOS amaran project**, as credited by Wes Bos in [esp32-firmware/README.md](esp32-firmware/README.md#relationship-to-aarons-amaran-cli) |
| MIT license file of the original project | **Kevin Schaich** (upstream commit `4658c64`) |
| ESP-IDF, whose BLE Mesh stack the core patch modifies | **Espressif Systems** (Apache-2.0) |
| Stream Deck plugin with its settings page, icons, test and scripts (`streamdeck/`), frame tools (`scripts/frame-*.ts`), `SETUP-PROMPT.md`; ESP32 changes: on/off delivery check, `/toggle`, `/status`, state and capabilities in `GET /lights`, the name `amaran-esp32.local` (mDNS), strongest access point selection, firmware updates over Wi-Fi (OTA); Mac daemon hardening and proxy selection by signal strength; `HARDENING.md` | **Arne Brockmann** (this fork) |

amaran, Aputure and Sidus Link are trademarks of their respective owners. This
project is not affiliated with or endorsed by them.
