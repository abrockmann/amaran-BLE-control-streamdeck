# Setup prompt for an AI assistant

Copy the block below into a coding agent that can run commands on your Mac
(Claude Code, Codex, …). A chat assistant without a terminal (Claude, ChatGPT)
works too — it then gives you the commands one at a time and you run them.

It covers both ways: with an ESP32 bridge, or on the Mac alone. The prompt makes
the assistant ask before anything that installs, flashes or restarts, check
every step before moving on, and keep your Wi-Fi password and the mesh keys out
of the chat.

````text
Help me set up control of my amaran lights from a Stream Deck on my Mac, with
the plugin from https://github.com/abrockmann/amaran-BLE-control-streamdeck —
either through an ESP32 bridge or with the Mac alone. Before you start, read
README.md (sections "Stream Deck + ESP32 bridge" and "Stream Deck without an
ESP32"), HARDENING.md and, for the bridge, esp32-firmware/README.md. Where they
disagree with your own assumptions, follow them: they record problems that
actually happened.

RULES — these come before everything else:
1. Secrets stay out of this chat. My Wi-Fi password, an MQTT password and the
   mesh keys must never appear in the conversation, in commands you show, or in
   output you print. I type the Wi-Fi password into
   esp32-firmware/main/wifi_config.h myself, in my own editor. Never print
   main/wifi_config.h, main/mesh_config.h or lights.json. To check them, use
   commands that print only counts or the non-secret fields named below.
2. Never commit, upload or copy those three files elsewhere in the repository.
   They are in .gitignore; don't work around that.
3. Ask me before you install software, apply the ESP-IDF patch, flash the ESP32
   (name the serial port you picked and why), install a LaunchAgent, install the
   Stream Deck plugin (it quits and restarts Stream Deck), or switch a light.
4. Don't change macOS security or privacy settings. If a permission is missing,
   tell me what to allow and where.
5. Use exactly ESP-IDF v5.3.2 (the core patch is tested against it). Install npm
   dependencies only with `npm ci --omit=optional`, never `npm install`.
6. Run the check after each step. If a check fails, stop and show me what you
   saw instead of guessing past it.

FIRST, ASK ME:
- Is amaran Desktop installed on this Mac, and does it show all my lights? Both
  ways read the mesh keys from its database. The lights must already be set up
  in the amaran app; this project does not pair lights.
- With an ESP32 bridge or on the Mac alone? Explain the difference briefly: the
  bridge shows the lights' real state, stays on without the Mac and serves
  several computers and Home Assistant, but needs an ESP32 board and flashing.
  The Mac alone needs no hardware, but shows only what it last sent, works only
  while the Mac is awake and in Bluetooth range, and cannot run alongside
  amaran Desktop or a second Mac.
- For the bridge only: is an ESP32 board (ESP-32S / WROOM-32) connected with a
  USB data cable? Is my Wi-Fi a 2.4 GHz network (or has a 2.4 GHz band)? Do I
  use Home Assistant? Only then does the bridge need an MQTT broker address.

Then follow either "WITH AN ESP32 BRIDGE" or "MAC ONLY".

WITH AN ESP32 BRIDGE:
1. Clone the repository and run `npm ci --omit=optional` in it.
   Check: finishes without errors.
2. Install ESP-IDF v5.3.2 as described in esp32-firmware/README.md, "One-time
   setup" (on macOS including cmake and ninja via idf_tools.py).
   Check: `idf.py --version` reports v5.3.2.
3. Apply esp32-firmware/patches/0001-amaran-net-recv-status-snoop.patch inside
   the ESP-IDF directory.
   Check: `git apply --check <patch>` passes first; afterwards
   `git diff --stat` in the ESP-IDF directory shows the patched file.
4. Generate the mesh config from the repository root: `npm run gen-config`.
   Check: it lists every light with its capabilities. Then show me only names
   and mesh addresses:
   grep -oE '\.name = "[^"]*"|\.address = 0x[0-9a-f]{4}' esp32-firmware/main/mesh_config.h | paste - -
   Ask me whether those are current: amaran Desktop's database can be stale (see
   the warning in esp32-firmware/README.md). If I have a lights.json from a setup
   that demonstrably switches, compare the addresses with it.
5. In esp32-firmware: `cp main/wifi_config.h.example main/wifi_config.h`. I fill
   in WIFI_SSID and WIFI_PASSWORD myself, and OTA_TOKEN with the output of
   `openssl rand -hex 16` (I run it and paste it into the file; keep it in my
   password manager). DEVICE_ID stays "amaran-esp32" unless I run several
   bridges. Without Home Assistant, leave the MQTT values unchanged.
   Check, without printing values:
   grep -c '"your-ssid"\|"your-wifi-password"' main/wifi_config.h
   must print 0, and
   grep -cE '^#define[[:space:]]+OTA_TOKEN[[:space:]]+"[0-9a-f]{32}"' main/wifi_config.h
   must print 1.
5b. Generate the key that signs firmware updates (once):
   `espsecure.py generate_signing_key --version 1 --scheme ecdsa256 secure_boot_signing_key.pem`
   in esp32-firmware, with ESP-IDF sourced. Never print it; tell me to keep a
   copy in my password manager. Check: `ls -l secure_boot_signing_key.pem` shows
   the file, and `git check-ignore secure_boot_signing_key.pem` confirms it is
   ignored.
6. Build in esp32-firmware: `idf.py set-target esp32` (first build only), then
   `idf.py build`.
   Check: the build ends without errors.
7. Flash (ask first): `idf.py -p <port> flash monitor`, Ctrl+] leaves the
   monitor. If you have no interactive terminal, run `idf.py -p <port> flash`
   and use step 8 as the check.
   Check: the log shows "self-provisioned", "got ip",
   "mdns: amaran-esp32.local" and "http listening on :80".
8. Check the bridge over the network:
   `curl http://amaran-esp32.local/status` and
   `curl http://amaran-esp32.local/lights`
   Check: /lights lists my lights. If the name does not resolve, take the IP
   address from the boot log or my router and use it instead.
9. First real switch (ask first; I watch the light):
   `curl -X POST http://amaran-esp32.local/lights/<key>/toggle`, twice.
   Check: the light goes off and on, and "on" in each answer matches what I see.
   If the answer is ok but the light does not react, the mesh address is probably
   stale — back to step 4.
10. Install the Stream Deck plugin (ask first): `streamdeck/install-plugin.sh`,
    or `streamdeck/pack-plugin.sh` and I double-click the file it puts in
    streamdeck/dist/.
    Check: install-plugin.sh ends with "Plugin running", or the category
    "amaran lights" shows up in Stream Deck.
11. Tell me how to finish in Stream Deck: drag Brightness or Color temperature
    onto a dial and On/off onto a key, open each action's settings, leave Bridge
    empty (or enter the IP address) and pick the light.
    Check: the key shows the light's real on/off state, and turning a dial
    changes the light.

MAC ONLY:
1. Clone the repository and install:
   `npm ci --omit=optional`, then
   `npm rebuild @abandonware/noble --ignore-scripts=false` (the Bluetooth addon
   is compiled from source; see "Installation" in HARDENING.md).
   Check: both finish without errors. On an Intel Mac, first read the esbuild
   note in README.md "Quick Start".
2. `npm run setup` — it is interactive, so I answer its questions. It reads
   amaran Desktop's database and writes lights.json with the mesh keys.
   Check, printing only light keys and names:
   python3 -c 'import json; [print(l["key"], "-", l["name"]) for l in json.load(open("lights.json"))["lights"]]'
   Then ask me to quit amaran Desktop: only one app can hold the connection.
3. `streamdeck/build-apps.sh` builds ~/Applications/amaran-daemon.app.
   Check: the bundle exists.
4. `streamdeck/install-daemon-agent.sh` (ask first — it installs a LaunchAgent
   that starts the daemon now, at login and after sleep). On the first start
   macOS asks for Bluetooth access; I allow it myself.
   Check: after up to a minute, `curl http://127.0.0.1:2708/lights` lists my
   lights. If not, read ~/Library/Logs for the guard's and daemon's logs; a
   missing Bluetooth permission is the usual cause (HARDENING.md, "Bluetooth
   permission on macOS").
5. First real switch (ask first; I watch the light):
   `curl -X POST http://127.0.0.1:2708/lights/<key>/on`, then `.../off`.
   Check: the light follows. "ok" in the answer only means sent — the lights
   send no acknowledgement to the Mac, so what I see is the check.
6. Install the Stream Deck plugin (ask first):
   `AMARAN_HOST=127.0.0.1 streamdeck/install-plugin.sh` — not the double-click
   package, which does not know where the daemon's guard lives.
   Check: the script ends with "Plugin running".
7. Tell me how to finish in Stream Deck: drag Brightness or Color temperature
   onto a dial and On/off onto a key, open each action's settings and pick the
   light; the Bridge field shows 127.0.0.1:2708.
   Check: turning a dial and pressing the key change the light.

IF SOMETHING FAILS: search HARDENING.md and esp32-firmware/README.md first. They
cover the known traps — weak Wi-Fi to the bridge, stale mesh addresses, 2.4 GHz
only, the Bluetooth permission, a daemon that is stale after sleep, amaran
Desktop holding the connection — before you invent a fix.

AT THE END: a short summary of what was set up, which values you verified and
how, and anything you skipped.
````
