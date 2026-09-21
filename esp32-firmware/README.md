# amaran ESP32 mesh-control firmware

ESP-IDF firmware that turns an ESP-32S / ESP32 WROOM-32 module into a
permanent BLE Mesh node in your amaran studio mesh. It sends Telink
`0x26` vendor commands for on/off / brightness / CCT / color / green-
magenta, and exposes the fixtures to your LAN two ways:

- **MQTT with Home Assistant discovery** — fixtures auto-appear as HA
  `light` entities (+ a `number` entity for green/magenta).
- **HTTP REST API** — `curl` / scripts / your own UI.

Mesh keys and per-fixture capabilities are read at build time from the
**amaran Desktop** app's local files and self-injected into ESP-IDF's
BLE Mesh stack, so no Sidus provisioning of the ESP32 is required. The
desktop and iPad apps keep working alongside it.

> Status: working on real hardware (ESP-32S, 3× amaran fixtures) as of
> 2026-05-28. One known limitation — external-app → HA state sync — is
> documented under [Two-way sync](#two-way-sync).

> **Added in this fork:** the sections from here down to "Hardware", the macOS
> notes in "One-time setup", the endpoints `/toggle` and `/status` with the on/off
> delivery check in "HTTP REST API", and the `OTA_TOKEN` row below. The firmware
> itself and the rest of this document are by Wes Bos — see
> [Credits](../README.md#credits).

## Firmware updates over Wi-Fi (OTA, since 2026-09-12)

Only the **first** install needs the USB cable; it writes the OTA partition table
(`partitions.csv`: `ota_0` + `ota_1`, 1.94 MB each). After that:

Once, before the first install over USB:

```sh
cd esp32-firmware
openssl rand -hex 16    # the OTA token: put it into OTA_TOKEN in main/wifi_config.h
. ~/esp/esp-idf/export.sh
espsecure.py generate_signing_key --version 1 --scheme ecdsa256 secure_boot_signing_key.pem
```

Keep both in your password manager. Then, for every update:

```sh
cd esp32-firmware && . ~/esp/esp-idf/export.sh && idf.py build   # signs the image
read -rs -p "OTA token: " TOKEN; echo                            # keeps it out of shell history
curl -X POST -H "X-Amaran-Token: $TOKEN" --data-binary @build/amaran_esp32_mesh_control.bin \
  http://amaran-esp32.local/ota
```

- **Two locks.** The token keeps strangers from starting an upload; the
  signature decides what gets installed. The bridge only accepts images signed
  with `secure_boot_signing_key.pem` (ECDSA, `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`
  in `sdkconfig.defaults`); anything else is refused with `invalid image`, and
  the running firmware stays. The key is gitignored — whoever has it can sign
  firmware the bridge accepts. The bootloader is unchanged, so signing could be
  switched on over OTA, and a USB flash still accepts any image: the way back if
  token or key are lost.
- **Until 2026-09-13 the token was derived** from the MQTT password (HMAC with a
  fixed label) and images were not signed. A security audit showed the chain:
  MQTT runs unencrypted, so whoever can intercept traffic on the network reads the
  password at a reconnect, computes the token and installs any firmware. Now the
  token is random and a stolen token alone is not enough.
- **Rollback** (since 2026-09-13). A new image boots `pending_verify` and has to
  pass a check before it is marked valid (`image_state` and `image_check` in
  `/status`): its own HTTP server must answer `GET /status` with 200 over loopback,
  and the largest free heap block must be at least 16 KB. First check after a
  minute, then every 10 s. If it crashes before, the bootloader starts the previous
  image; if it hasn't passed after 5 minutes of uptime, the firmware marks itself
  invalid and restarts into the previous image.
  Tested with a signed image that aborted 20 s after boot: the serial log showed
  the crash, the bootloader loaded `0x20000` again, and `/status` reported
  `rolled_back_from: "ota_1"` with the previous firmware's ELF hash. The rollback
  logic lives in the bootloader, so switching it on took one USB flash
  (`idf.py -p <port> flash` — the mesh provisioning in NVS stays).
- **Until 2026-09-14 one minute of uptime was the whole check.** A review found
  the gap: the task watchdog only logs (`CONFIG_ESP_TASK_WDT_PANIC` is off), so an
  image with a hung HTTP task or leaking memory would have been confirmed — and
  then the next upload would have needed the cable. Checked on 2026-09-14: the
  update confirmed on the first check (`stack_free` 6212 of 8192 bytes); a test
  image built to fail the memory check reported `last_error: "heap"` 20 times and
  was back on the previous firmware at 5 minutes, with
  `rolled_back_from: "ota_0"`, MQTT over TLS reconnected and `/status` answering in
  0.15–0.3 s. A hang that starts after the image is confirmed is covered since
  2026-09-21 by the health watchdog (next point).
- **Health watchdog** (since 2026-09-21). A second task runs the same check once a
  minute for the whole uptime; three failures in a row restart the bridge. It gives
  no verdict while an upload holds the HTTP task (for at most 5 minutes), and it has
  to have run once before a new image is confirmed, so a fault in it still rolls
  back. Backoff: when a failure streak begins within an hour of a boot, each such
  restart in a row doubles the failures needed (3, 6, 12 … 96); a streak that begins
  after a healthy hour always restarts after 3. What failed, with the heap and socket
  figures of that moment, survives the restart in RTC memory (`health.last_restart`
  in `/status`; power loss clears it). Checked on the bridge on 2026-09-21 with two throwaway test images. One whose
  watchdog check always failed restarted after three checks (~4 minutes of uptime),
  `reset_reason: "software"`, record intact. One whose watchdog checked a closed
  port (the real failure path) failed while pending without holding up the
  confirmation, then restarted after three failures with `http_client:
  "ESP_ERR_HTTP_CONNECT"`, `errno` 104 and 1656 of 4096 bytes of stack left — the
  task has 5 KB since. The released build: watchdog's first check at 45 s, image
  confirmed on its first check at 60 s, about 2 KB of the 5 KB in use, ~66 KB free
  heap.
- **Socket pool** (since 2026-09-21). That morning the bridge reset every HTTP
  connection — `/status`, `/lights`, even unknown paths — until it was
  power-cycled. lwIP's socket pool (default 10) ran out while the HTTP server still
  had free session slots, and the server drops its least recently used session only
  when all 7 are taken, so `accept()` failed and lwIP reset each new connection.
  Holding 4 idle connections reproduced it. Now `CONFIG_LWIP_MAX_SOCKETS=16` with 7
  sessions (a compile-time check in `main/http.c` keeps the pool large enough), and
  TCP keep-alive closes sessions whose client vanished (after ~90 s). Checked on the
  bridge: with 10 idle connections held every new request still got 200; sessions
  stayed at 7 and sockets at 11 of 16.
- **No upload while `pending_verify`.** ESP-IDF refuses to start an update before
  the running image is confirmed, so `POST /ota` answers `409`
  `{"ok":false,"error":"pending_verify"}` — wait for `image_state: "valid"`. The
  bridge closes the connection instead of reading the body first, so curl reports
  `http=409` within about 2 s but usually ends with `Connection reset by peer`
  (exit 56), with or without `Expect: 100-continue`; the JSON body may not arrive.
  Until 2026-09-14 this was a 500 `ota begin` after the whole image had been sent.
  All other upload errors close the connection the same way.
- `esp_ota_end` verifies the image **before** the boot partition switches; an
  aborted upload never boots half an image. The device then starts from the other
  partition (`ota_0` <-> `ota_1`).
- First real OTA on 2026-09-12: 1,668,608 bytes in 16 s, restart from `0x210000`
  (`ota_1`), `already provisioned from NVS`, MQTT reconnected.
- **If the partition table itself changes, OTA cannot do it** — back to the cable.
- **Aborts on 2026-09-13:** two of three uploads failed, one of them with
  `OTA aborted after 932160 bytes`. The handler treated a pause in receiving (a
  socket timeout — the Wi-Fi shares its antenna with the mesh) as an abort. Since
  then it waits out up to five timeouts, like the ESP-IDF upload examples. An abort
  does no harm: the running firmware stays active, and `curl` reports
  `{"ok":false,"error":"recv"}`. Just upload again. The firmware with this fix went
  on over the cable, because the old one didn't have it yet. After that six of eight
  uploads went through (17–22 s, once 77 s on weak Wi-Fi). **Two failed after
  76–77 s with `Connection reset by peer`, and the device restarted each time** —
  with the old firmware, so no harm done; a second attempt went through each time.
- **Cause found the same day: a stack overflow.** `GET /status` then reported
  `reset_reason: "panic"`, and a trace kept in RTC memory (it survives software and
  panic resets) showed where: stage `verifying` — the whole image received, crash
  inside `esp_ota_end` — with only 616 bytes of stack left in the HTTP server task.
  A successful upload on the old firmware had just 40 bytes to spare. The server
  task now gets 8192 instead of 4096 bytes; three consecutive uploads went through
  in 18–19 s with about 4100 bytes left. The trace stays in `/status` as `last_ota`
  (`stage`, `bytes`, `stack_free_min`) for the next surprise.

## State over HTTP

Besides key, name, address and MAC, `GET /lights` now also returns `on`,
`brightness`, `mode` and `kelvin` or `hue`/`sat`, plus each light's capabilities:
`has_color`, `has_gm`, `cct_min` and `cct_max`. Careful when reading it: the
firmware takes brightness and color from the status reply **only for a light that is
on** (`amaran_mqtt_report_state`). After a bridge restart, a light that is off shows
the defaults (100 %, 4700 K) there until it is switched on once. For "off", only
`on` itself is reliable.

## Hardware

- ESP-32S (WROOM-32) module, 4 MB flash, 40 MHz crystal.
- USB-serial connection for flashing (CP210x → `/dev/cu.SLAB_USBtoUART`).
- A Wi-Fi network and an MQTT broker reachable on it (Home Assistant's
  Mosquitto add-on is the easy path).

## One-time setup

```sh
# 1. Install ESP-IDF v5.3.x and source it into your shell.
#    NOTE on macOS: install.sh fetches the compilers but NOT cmake/ninja —
#    it expects them from the system. Without them idf.py stops with
#    '"cmake" must be available on the PATH'. To add them without Homebrew:
#        python3 $IDF_PATH/tools/idf_tools.py install cmake ninja
#    Use exactly v5.3.2: the core patch in patches/ is tested against it
#    (applied cleanly on 2026-09-11).
. ~/esp/esp-idf/export.sh

# 2. Generate esp32-firmware/main/mesh_config.h from the amaran Desktop DB.
#    Pulls mesh keys, fixtures, and per-fixture capabilities (color / G-M /
#    CCT range) from the app's bundled fixture_config.json. Run from the
#    repo root (it's a TypeScript tool in the main project):
npm ci --omit=optional
npm run gen-config         # options: -- --db <path> --relay-hub <MAC>

# WARNING, learned the hard way: gen-config reads THIS machine's amaran Desktop
# database — and that can be stale. On 2026-09-11 it listed node address 37 for
# the Verge Max while the light had long been listening on 40; every command to
# 37 vanished silently, with "ok" from the API. The value was read correctly and
# was still wrong.
#
# So before building, check the generated addresses against a source that
# demonstrably switches (a working lights.json from another machine). If
# something differs: copy the database, fix it in the copy, and point the
# generator at the copy with --db — the original stays untouched and the
# generator does its normal job:
#     sqlite3 copy.db "UPDATE fixtures SET node_address=40 WHERE node_address=37;"
#     npm run gen-config -- --db copy.db

cd esp32-firmware

# 3. Copy the Wi-Fi/MQTT template and fill in your values.
cp main/wifi_config.h.example main/wifi_config.h
$EDITOR main/wifi_config.h
```

`main/wifi_config.h` fields:

| Field | Notes |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Your 2.4 GHz network |
| `MQTT_URI` | e.g. `mqtts://192.168.1.10:8883` with TLS, or `mqtt://192.168.1.10:1883` without (the broker's IP is more reliable than mDNS on ESP32) |
| `MQTT_CA_CERT` | CA certificate for `mqtts://`, see [MQTT over TLS](#mqtt-over-tls); `""` = plain MQTT. A CA with a plain `mqtt://` URI is refused: MQTT stays off and the serial log says why |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | Broker login, or `""` for anonymous |
| `HA_DISCOVERY_PREFIX` | Leave `homeassistant` unless changed in HA |
| `MQTT_TOPIC_PREFIX` | `amaran` → topics like `amaran/key_light/set` |
| `DEVICE_ID` | Change if you run more than one of these nodes |
| `OTA_TOKEN` | Token for firmware updates over Wi-Fi; `""` disables OTA (see top of this file) |

`main/mesh_config.h` and `main/wifi_config.h` are **key-bearing and
`.gitignore`'d — never commit them.**

## Build, flash, monitor

```sh
idf.py set-target esp32          # first build only
idf.py -p /dev/cu.SLAB_USBtoUART flash monitor   # Ctrl+] to exit
```

Healthy boot log:

```
amaran_node: NODE_PROV_COMPLETE net_idx=0x000 addr=0x0010
amaran_node: self-provisioned: addr=0x0010 net_idx=0x000 app_idx=0x000
amaran_wifi: got ip 192.168.x.x
amaran_mqtt: MQTT connected
amaran_mqtt: discovery published: homeassistant/light/amaran-esp32_key_light/config
amaran_http: http listening on :80
```

Provisioning persists in NVS, so later reboots come straight up ready.
The `AppKey already exists` / `Already bound` warnings on reboot are
expected (NVS already holds the keys) and harmless.

## Home Assistant

With HA's MQTT integration + discovery enabled (default), the fixtures
appear under **Settings → Devices & Services → MQTT** as device
**"amaran ESP32 mesh bridge"**:

- one `light` entity per fixture — on/off, brightness, CCT slider, and a
  color wheel **only on color-capable fixtures**;
- one `number` entity per fixture that supports green/magenta
  (`<name> G/M`, −10 … +10, 0 = neutral, negative greener, positive
  more magenta).

Capabilities are detected per fixture from the amaran app config
(`hsi_support`, `gm_support`, `product_cct_min/max`), so CCT-only
fixtures don't show a dead color wheel or G/M slider.

## MQTT over TLS

> Added in this fork (2026-09-14).

Without TLS the MQTT login travels in plain text through the network. With it, the
bridge checks the broker's certificate against a CA you put into
`main/wifi_config.h`, including the address in `MQTT_URI`.

1. **A small CA and a broker certificate.** Use EC P-256 — an RSA certificate
   costs the bridge noticeably more memory (numbers below). The broker certificate
   must name the address the bridge uses, e.g. `IP:192.168.1.10`. Best generate the
   broker's private key on the broker itself and only move the certificate request
   and the signed certificate around.
2. **Home Assistant's Mosquitto add-on** reads `/ssl/fullchain.pem` (broker
   certificate followed by the CA certificate) and `/ssl/privkey.pem`, and then
   opens 8883. Restart the add-on.
3. **The bridge:** `MQTT_URI "mqtts://<broker IP>:8883"` and the CA certificate
   in `MQTT_CA_CERT`, then build and update.
4. **Close the plain ports** 1883 and 1884 in the add-on's network settings.
   Home Assistant itself talks to the add-on internally and keeps working; other
   MQTT devices on your network would need TLS too.

**Memory** — measured with a test build that ran an extra TLS client next to the
normal one. mbedTLS's default buffers left 2.2 KB of heap during a firmware upload;
with dynamic buffers (`sdkconfig.defaults`) 31 KB, and in normal operation with
TLS the bridge keeps about 73 KB free. With an RSA-2048 broker certificate the
lowest value during an upload was 11 KB.

## HTTP REST API

The bridge answers as **`<DEVICE_ID>.local`** — `amaran-esp32.local` with the
default `DEVICE_ID` — so you don't need its IP address. (It also has an IPv6
link-local address; without one, macOS waited 5 s for an IPv6 answer on every name
lookup.) The IP is in the boot log too (`amaran_wifi: got ip ...`); a DHCP
reservation on your router keeps it stable.

```sh
curl http://<esp32-ip>/lights                     # list fixtures (JSON)

curl -X POST http://<esp32-ip>/lights/key_light/on
curl -X POST http://<esp32-ip>/lights/key_light/off
curl -X POST http://<esp32-ip>/lights/key_light/toggle  # bridge flips its known state
curl -X POST http://<esp32-ip>/lights/all/off      # target: key, "all", addr, or 0xNNNN

curl -X POST http://<esp32-ip>/lights/key_light/brightness \
     -H 'content-type: application/json' -d '{"value": 40}'
curl -X POST http://<esp32-ip>/lights/back_light/cct \
     -H 'content-type: application/json' -d '{"kelvin": 4700, "intensity": 30, "gm": 0}'
curl -X POST http://<esp32-ip>/lights/key_light/hsi \
     -H 'content-type: application/json' -d '{"hue": 200, "sat": 80, "intensity": 60}'

curl -X POST http://<esp32-ip>/refresh             # nudge desktop/iPad apps to re-sync
curl -X POST http://<esp32-ip>/lights/key_light/refresh
```

**Who may call it** (since 2026-09-13, from a security audit): the `Host` header
has to be an IP address, `<DEVICE_ID>` or `<DEVICE_ID>.local`, and write requests
must not carry an `Origin` header. That blocks web pages: without it any page open
in a browser on your network could switch the lights with a simple cross-site POST,
and a DNS-rebinding page could also read `/status` and `/lights`. curl, scripts and
the Stream Deck plugin send no `Origin`. If you reach the bridge under another
name (for example a router's DNS name), use its IP address instead — or add the
name in `host_allowed()` in `main/http.c`. Refused requests get `403`.

`GET /status` judges the bridge's placement without a USB cable:

```json
{"uptime_s":82,"reset_reason":"software","firmware":"514dda6-dirty","elf_sha256":"8ee6ffdf7",
 "partition":"ota_0","free_heap":88648,"min_free_heap":73500,"mqtt_connected":true,
 "mqtt_connects":1,"last_ota":{"stage":"done","bytes":1711824,"stack_free_min":4128},
 "wifi":{"rssi":-49,"channel":1,"bssid":"02:00:00:00:00:01","reconnects":0},
 "onoff":{"confirmed_in_round":[0,0,0],"not_confirmed":0},
 "fixtures":[{"key":"amaran_verge_max_1","asked":6,"heard":5,"heard_s_ago":10}, …]}
```

- `wifi.rssi` — signal to the access point. Measured 2026-09-13: −82 dBm gave
  HTTP round trips of 0.15–5 s, −61 dBm a median of 173 ms.
- `fixtures[].heard` / `asked` — share of status requests a fixture answered,
  i.e. Bluetooth reach. Counters start at boot; the poll asks every ~12 s, so
  `heard_s_ago` stays under ~13 s for a fixture in range. `heard` also counts
  replies triggered by other apps.
- `onoff` — in which round an on/off got confirmed. Many round-2/3 entries or
  `not_confirmed` mean the fixture hears the bridge badly; note that "off" to a
  lit Verge Max was the hard case.
- `image_state` / `image_check` / `rolled_back_from` — `pending_verify` after an
  update until the image check passes (at the earliest after a minute), then
  `valid` (`untracked` after a USB flash). `image_check` is there only when this
  boot started unconfirmed: `checks` so far, `last_error` (`null` when the last one
  passed; otherwise `http no answer`, `http status`, `heap`, `http client`,
  `watchdog not run`, `mark valid` or `rollback failed`), and `stack_free` once it
  is done. `rolled_back_from` names
  the partition the bootloader gave up on; it is read once at boot.
- `sockets` — open lwIP sockets (`open` of `max`) and HTTP sessions
  (`http_sessions` of `http_sessions_max`). With no other client: 4 sockets (the
  server's 3 and MQTT) plus one session for the request itself. `open` reaching
  `max` is the lockout of 2026-09-21.
- `health` — the watchdog: `checks` with a verdict, `fails_in_row`, `last_error`
  (values as for `image_check`), `fails_to_restart` (3, more during a backoff),
  `stack_free` of its task and `restarts` since power-on. After a watchdog restart
  (`reset_reason: "software"`) `last_restart` holds `uptime_s`, `reason`,
  `http_client` (the esp_http_client result, `null` if the client couldn't be
  created), `errno`, `http_status`, the heap figures, `sockets_open`,
  `http_sessions` and the task's `stack_free` at that moment.
- `largest_free_block` — the largest single allocation that would fit. TLS needs
  contiguous memory, so this matters more than `free_heap` alone.
- `wifi.reconnects` / `mqtt_connects` — Wi-Fi reconnects since boot and MQTT
  connects since boot. `mqtt_connects` should stay near `wifi.reconnects + 1`; if
  it keeps climbing on its own, the broker keeps dropping the bridge. Until
  2026-09-13 every Wi-Fi reconnect started a second MQTT client with the same
  client ID, and the two would have taken the session from each other in turn.
- `elf_sha256` matches `shasum -a 256 build/amaran_esp32_mesh_control.elf` —
  the way to confirm an OTA actually took.
- `last_ota` — how far the last upload got (`begin`, `receiving`, `verifying`,
  `done`, `failed`) and the least free stack of the HTTP task while it ran. Present
  after a software or panic restart, not after power loss. `reset_reason: "panic"`
  with `stage: "verifying"` is what the stack overflow looked like.

`on`, `off` and `toggle` answer with the resulting state, e.g.
`{"ok":true,"on":false}`; `toggle` needs a single fixture (400 for `all`). For a
single fixture, on/off is repeated until its status reply shows the wanted
state: per round three sends at 0/200/400 ms, a status request at 600 ms, a
verdict at 1 s, at most three rounds. The Telink protocol has no ack, and "off"
to a lit Verge Max was often lost without it (measurements in `HARDENING.md`).
The HTTP answer comes right away and does not wait for that confirmation; the
serial log shows `confirmed after … ms, round n` or `not confirmed after 3 rounds`.

## UART REPL

The serial monitor doubles as a control surface for debugging:

```
> list
> on            > off back_light
> brightness 25 key_light
> cct 4700 30            # cct <kelvin> <0-100%> [target] [gm -10..10]
> hsi 200 80 60          # hsi <hue> <sat> <int%> [target]
> refresh
> status
> help
```

## Two-way sync

State flows **both ways**: HA / curl / UART → lights, *and* changes made
from the **desktop / iOS app or a physical knob** now show up in Home
Assistant.

Outbound (HA → lights):
- The ESP32 publishes state to MQTT on every command, and after any
  command + every 12 s it sends a Telink status-request (`cmd_type=0x0e`).
  Fixtures reply, and those replies propagate through the mesh — which is
  what makes the **desktop app** re-sync live and lets the **iOS app**
  show correct state on pull-to-refresh.

Inbound (lights → HA) — the status snoop:
- The catch is that Telink fixtures route every `0x26` status reply to the
  **provisioner unicast `0x0001`**, never to whoever requested status.
  Our node (`0x0010`) network-decrypts the reply (we have the NetKey) but
  the mesh stack drops it before the access layer: `trans_unseg()` bails on
  any non-local message, and the model dispatch requires the destination to
  be one of our own element addresses. So no amount of model registration
  can catch it. (The earlier "1-byte `0x26` opcode" theory was a red
  herring — the opcode is fine; the *destination address* is the problem.)
- The fix is a small **patch to the ESP-IDF BLE Mesh core** (see
  [`patches/`](patches/)). It adds a passive snoop in `bt_mesh_net_recv`
  that decrypts a *copy* of the access payload with the studio AppKey and
  hands the plaintext to the firmware via the weak hook
  `amaran_mesh_access_rx()` (implemented in `main.c`). The relay path is
  untouched, so desktop/iOS re-sync still works.
- `amaran_telink_decode_status()` (`telink.c`) decodes the reply. The
  fixture reports its **current mode** in `command_type`: `0x02` = CCT
  (kelvin, intensity, G/M), `0x01` = HSI/color (hue, sat, intensity), with
  power in `(low64 >> 8) & 1`. `command_type 0x0a` is a constant diagnostic
  page the desktop polls and is ignored. The CCT layout matches Aaron's
  `decodePacket`; HSI uses the same bit-packing as the HSI setter (both
  confirmed against live replies).
- `amaran_mqtt_report_state()` (`mqtt.c`) updates HA **only when a reading
  differs materially** from what we last commanded, so the echo of our own
  commands doesn't churn HA while genuine external changes propagate.

> **Important:** the inbound path requires the `patches/` patch applied to
> your ESP-IDF checkout. Without it, the firmware still builds and the
> outbound direction works, but HA won't reflect external changes (the weak
> hook is never called).

### Debugging the snoop

TypeScript helpers in the **main project** (`../scripts/`, run from the repo
root) verify the reply format. For the probe, build the firmware with
`CONFIG_BLE_MESH_STACK_TRACE_LEVEL=3` so the patch logs the raw decrypted PDUs:
- `npm run esp32:probe -- "<repl command>"` — send a command, decode the
  resulting status replies per fixture using the shared `src/telink.ts`
  decoder (the same logic the firmware uses — a cross-check that they agree).
- `npm run mqtt:watch` — subscribe to the `amaran/.../state` topics to confirm
  what reached the broker (reads broker/creds from `lights.json`).
- `npm run esp32:capture` — trigger a refresh and dump raw serial.

## Layout

```
esp32-firmware/
├── CMakeLists.txt
├── sdkconfig.defaults          Bluedroid + BLE Mesh + Wi-Fi + HTTP + MQTT, 4 MB flash
├── partitions.csv              3 MB factory app partition
├── patches/                    ESP-IDF core patch for the inbound status snoop (required)
└── main/
    ├── main.c                  app_main, mesh self-provision, dispatch, UART REPL, poll
    ├── mesh_config.h           generated; KEY-BEARING; .gitignore'd
    ├── wifi_config.h.example   credentials template
    ├── wifi_config.h           you create; KEY-BEARING; .gitignore'd
    ├── telink.[ch]             Telink 0x26 packet builders (on/off/bri/CCT+G-M/HSI/status)
    ├── wifi.[ch]               Wi-Fi STA bring-up + reconnect
    ├── mqtt.[ch]               MQTT client, HA discovery, command parsing, debounced state
    └── http.[ch]               HTTP REST API
```

Build/debug tooling lives in the **main TS project** (`scripts/`, run from the
repo root): `gen-config` (writes `main/mesh_config.h`), `esp32:probe`,
`esp32:capture`, and `mqtt:watch`.

## How it works

**Self-provisioning** — ESP-IDF has no public API to import pre-known
mesh keys, so we use internal helpers (`mesh/main.h`, `local.h`):

1. `esp_ble_mesh_node_prov_enable(ADV|GATT)` — sets the `BLE_MESH_NODE` flag.
2. `bt_mesh_provision(net_key, …, addr, dev_key)` — sets `BLE_MESH_VALID`,
   loads NetKey + DeviceKey + unicast address `0x0010`.
3. In `NODE_PROV_COMPLETE_EVT`: `bt_mesh_node_local_app_key_add` then
   `bt_mesh_node_bind_app_key_to_model`.

The DeviceKey is randomized per boot — we never use Config Client traffic.

**Telink opcode** — control/status uses opcode `0x26` as a raw 1-byte
opcode (`0x26 < 0x80`), not the standard 3-byte vendor encoding. We send
it directly via `esp_ble_mesh_server_model_send_msg`; the fixtures'
Telink stack dispatches on the byte. The CCT/G-M/HSI bit-packing is
ported byte-for-byte from the amaran CLI's `native_telink_control.swift`
and cross-checked against it.

## Maintenance

- **New fixture or new mesh keys in Sidus:** re-run `npm run gen-config`
  (from the repo root), then `idf.py build flash`. If the mesh keys
  themselves changed, `idf.py -p <port> erase-flash` first to clear stale
  NVS provisioning.
- **Unknown fixture code:** capabilities come from the amaran app's
  `fixture_config.json`. Fixtures missing from it (e.g. the Halo / 401C5
  on older app versions) fall back to a small table in
  `scripts/generate-config.ts` — edit `FALLBACK_CAPS` there if a fixture's
  color/G-M/CCT is wrong.
- **Wi-Fi + BLE coexistence** is handled by the software-coex layer (on
  by default). A single reconnect retry at boot is normal.

## Relationship to Aaron's amaran CLI

This is an independent ESP32 firmware. It reuses two things from Aaron's
macOS amaran project (a clone of which lives under `../amaran/` here, if
present): the reverse-engineered Telink `0x26` packet formats (see
`native_telink_control.swift`) and the "status-request refreshes the
apps" trick (his TUI's Shift+R). Everything on the ESP32 — mesh node
self-provisioning, Wi-Fi/MQTT/HTTP, HA discovery — is new here. Aaron's
`firmware/sidus-join-probe/` is a different thing entirely: an nRF52840
key-capture probe, not a controller.
