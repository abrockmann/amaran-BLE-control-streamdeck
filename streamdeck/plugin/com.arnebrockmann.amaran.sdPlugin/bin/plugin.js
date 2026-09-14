#!/usr/bin/env node
"use strict";
/**
 * Stream Deck plugin: brightness, color temperature and on/off of amaran lights,
 * over HTTP — either the ESP32 bridge on the LAN or the local mesh daemon
 * (127.0.0.1). No amaran Desktop and no Bluetooth access from this process, so
 * the plugin needs no system permission.
 *
 * No third-party dependencies: Stream Deck's Node runtime (v20) has no global
 * WebSocket, so the client below is implemented by hand.
 *
 * Settings: the bridge address is a global setting, the light a per-action
 * setting — both edited in the property inspector (ui/settings.html). Older
 * installs without settings fall back to paths.json from install-plugin.sh.
 *
 * State: against the ESP32 bridge the plugin reads the real state (the bridge
 * snoops the lights' status replies). The local daemon cannot report state —
 * the lights answer the original provisioner, not the asker — so there the
 * plugin keeps its own model and shares the on/off file with the switch
 * bundles to keep dials and keys in step.
 */
const net = require("net");
const http = require("http");
const fs = require("fs");
const os = require("os");
const path = require("path");
const crypto = require("crypto");
const { spawn } = require("child_process");

const DAEMON_APP = process.env.AMARAN_DAEMON_APP ||
  path.join(os.homedir(), "Applications/amaran-daemon.app");

const BRI = { min: 1, max: 100, step: Number(process.env.AMARAN_BRIGHTNESS_STEP || 1) };
// Fallback color temperature range, used when the target does not report one
// (the local daemon). The bridge reports cct_min/cct_max per light in /lights.
// 2700–6500 K is the amaran Verge Max per the manufacturer's manual.
const CCT = {
  min: Number(process.env.AMARAN_CCT_MIN || 2700),
  max: Number(process.env.AMARAN_CCT_MAX || 6500),
  step: Number(process.env.AMARAN_CCT_STEP || 100),
  default: 5600,
};

const ACT_BRI = "com.arnebrockmann.amaran.brightness";
const ACT_CCT = "com.arnebrockmann.amaran.cct";
const ACT_POWER = "com.arnebrockmann.amaran.power";

// Texts drawn on the Stream Deck + touch strip. Action names and tooltips are
// localized through en.json/de.json; these are set at runtime, so they follow
// the language Stream Deck passes in -info.
const TEXTS = {
  en: { brightness: "Brightness", cct: "Color temp", off: "off", nolight: "no light" },
  de: { brightness: "Helligkeit", cct: "Farbtemperatur", off: "aus", nolight: "keine Leuchte" },
};
let T = TEXTS.en;

// install-plugin.sh puts paths.json next to the plugin, because the installed
// plugin is a copy and would not otherwise know the repo path. It may also
// carry host/port/light from older installs.
let PATHS = {};
try { PATHS = JSON.parse(fs.readFileSync(path.join(__dirname, "..", "paths.json"), "utf8")); } catch (_) {}
const GUARD = PATHS.guard || path.join(os.homedir(), "Code/amaran-ble-control/streamdeck/daemon-guard.sh");
const DEFAULT_HOST = "amaran-esp32.local";

// Global settings from the property inspector; they win over env and paths.json.
let GLOBAL = {};

// Target of the commands. The ESP32 bridge and the local daemon use the same API
// shape (POST /lights/<key>/<action>) — only the light keys differ: the ESP32
// derives them from device names (amaran_verge_max_1), the daemon from
// lights.json (amaranve).
function target() {
  const host = GLOBAL.host || process.env.AMARAN_HOST || PATHS.host || DEFAULT_HOST;
  // The guard replaces a stale LOCAL daemon. Against a device on the network it
  // makes no sense: the ESP32 does not sleep with the Mac.
  const local = host === "127.0.0.1" || host === "localhost";
  const port = Number(GLOBAL.port || process.env.AMARAN_PORT || PATHS.port || (local ? 2708 : 80));
  return { host, port, local };
}

// How many milliseconds of unexpected wall-clock time count as a sleep cycle.
// Negative only for tests: then every round triggers.
const WAKE_DRIFT_MS = Number.isFinite(PATHS.wakeDriftMs) ? PATHS.wakeDriftMs : 10000;
const WATCH_MS = 2000;

const STATE_DIR = path.join(os.homedir(), "Library/Application Support/amaran-streamdeck");
const LOG_FILE = path.join(os.homedir(), "Library/Logs/amaran-streamdeck-plugin.log");

const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

// The log is capped: on 2026-09-11 a retry loop (see runSend) wrote 2.9 million
// lines, 239 MB. Checked at start and every 1000 lines; one previous file is kept.
const LOG_MAX_BYTES = 5 * 1024 * 1024;
let logLines = 0;
function rotateLog() {
  try { if (fs.statSync(LOG_FILE).size > LOG_MAX_BYTES) fs.renameSync(LOG_FILE, LOG_FILE + ".1"); } catch (_) {}
}
rotateLog();

function log(msg) {
  const line = new Date().toISOString() + "  " + msg + "\n";
  if (++logLines % 1000 === 0) rotateLog();
  try { fs.appendFileSync(LOG_FILE, line); } catch (_) {}
  if (sd.ready) sd.send({ event: "logMessage", payload: { message: "[amaran] " + msg } });
}

// ── State per light ──────────────────────────────────────────────────────────
// Several actions can point at the same light (a brightness dial and a power
// key) and share its state; different lights never block each other.
const lights = new Map();
let known = [];   // last /lights answer from the bridge

// Light keys end up in file names (state-<key>) and request paths. They come from
// the network and from action settings, which a shared Stream Deck profile can
// carry — so only plain names are accepted. Found in the audit of 2026-09-13: a
// fake bridge answering with the key "/../../../../.ssh/id_ed25519" made the next
// press overwrite that file with "on".
const KEY_RE = /^[A-Za-z0-9_-]{1,64}$/;
const validKey = k => typeof k === "string" && KEY_RE.test(k);

function light(key) {
  if (!validKey(key)) return null;
  let s = lights.get(key);
  if (s) return s;
  s = {
    key, power: "off", brightness: 60, kelvin: CCT.default,
    cctMin: CCT.min, cctMax: CCT.max,
    // What the on/off file contains, or what this process last wrote to it.
    // Without that distinction the watcher would take its own write for an
    // outside change and redraw for no reason.
    powerSeen: null,
    lastLocalChange: 0,
    queued: null, inFlight: false, sendTimer: null, lastSend: 0, saveTimer: null,
    retries: 0, retryAt: 0,
  };
  lights.set(key, s);
  loadState(s);
  return s;
}

const modelFile = s => path.join(STATE_DIR, "model-" + s.key + ".json");
const powerFile = s => path.join(STATE_DIR, "state-" + s.key);

function loadState(s) {
  try { fs.mkdirSync(STATE_DIR, { recursive: true }); } catch (_) {}
  try {
    const m = JSON.parse(fs.readFileSync(modelFile(s), "utf8"));
    if (Number.isFinite(m.brightness)) s.brightness = clamp(m.brightness, BRI.min, BRI.max);
    if (Number.isFinite(m.kelvin)) s.kelvin = clamp(m.kelvin, CCT.min, CCT.max);
  } catch (_) {}
  try {
    const p = fs.readFileSync(powerFile(s), "utf8").trim();
    if (p === "on" || p === "off") { s.power = p; s.powerSeen = p; }
  } catch (_) {}
}

function saveState(s) {
  if (s.saveTimer) return;
  s.saveTimer = setTimeout(() => {
    s.saveTimer = null;
    try { fs.writeFileSync(modelFile(s), JSON.stringify({ brightness: s.brightness, kelvin: s.kelvin }) + "\n"); }
    catch (e) { log("model not saved: " + e.message); }
    try { fs.writeFileSync(powerFile(s), s.power); s.powerSeen = s.power; }
    catch (e) { log("state not saved: " + e.message); }
  }, 400);
}

// Apply the capabilities the bridge reports for a light.
function applyCaps(s, l) {
  if (Number.isFinite(l.cct_min) && Number.isFinite(l.cct_max) && l.cct_min < l.cct_max) {
    s.cctMin = l.cct_min; s.cctMax = l.cct_max;
    s.kelvin = clamp(s.kelvin, s.cctMin, s.cctMax);
  }
}

// ── HTTP ─────────────────────────────────────────────────────────────────────
// family 4: macOS answers <name>.local with an IPv6 link-local address too, and
// Node drops its interface scope, so an IPv6 attempt could only fail or stall.
// Answers are small (the light list is a few KB). A cap and a hard deadline keep a
// misbehaving or hostile target from growing memory without end or from keeping a
// light "in flight" forever by trickling its answer: req.setTimeout alone only
// fires when the socket goes quiet.
const MAX_ANSWER = 256 * 1024;
function request(method, pathname, body, t = target(), timeoutMs = 6000) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : "";
    const req = http.request({
      host: t.host, port: t.port, path: pathname, method, family: 4,
      headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) },
    }, res => {
      // The body is read: for on/off/toggle the bridge reports the new state,
      // GET /lights returns the list. No JSON (Mac daemon) resolves to null.
      let text = "";
      res.setEncoding("utf8");
      res.on("error", reject);
      res.on("data", c => {
        text += c;
        if (text.length > MAX_ANSWER) req.destroy(new Error("answer too large"));
      });
      res.on("end", () => {
        if (res.statusCode !== 200) {
          return reject(Object.assign(new Error("HTTP " + res.statusCode), { status: res.statusCode }));
        }
        try { resolve(JSON.parse(text)); } catch (_) { resolve(null); }
      });
    });
    const deadline = setTimeout(() => req.destroy(new Error("timeout")), timeoutMs);
    req.on("close", () => clearTimeout(deadline));
    req.on("error", reject);
    req.end(data);
  });
}
const post = (pathname, body) => request("POST", pathname, body);

// ── Real state from the bridge ──────────────────────────────────────────────
// Until 2026-09-12 the plugin only kept its own record of the state. That broke
// as soon as someone else switched the light (Home Assistant, the app, the
// button on the fixture): the record said "off", the light was on, and a press
// sent "on" — to the user the dial never switched off. The bridge knows the real
// state (it snoops the lights' status replies), so the plugin no longer guesses
// against it. The local daemon has no state query; there the record remains.
async function fetchLights(t = target()) {
  const answer = await request("GET", "/lights", null, t, 3000);
  // The bridge answers with an array that carries state and capabilities, the
  // Mac daemon with { ok, lights: [{ key, name, mac, address }] } — keys and
  // names only, no state.
  const list = Array.isArray(answer) ? answer
    : answer && Array.isArray(answer.lights) ? answer.lights : null;
  if (!list) throw new Error("unexpected answer from " + t.host);
  return list.filter(l => l && validKey(l.key));
}

// Don't sync a light for this long after a local change: a query that left just
// before our own command would otherwise overwrite the value mid-turn.
const SYNC_QUIET_MS = 2500;
function applyTruth(s, l) {
  applyCaps(s, l);
  if (typeof l.on !== "boolean" || Date.now() - s.lastLocalChange < SYNC_QUIET_MS) return;
  s.power = l.on ? "on" : "off";
  // Take brightness and color ONLY from a light that is on. The firmware uses
  // the same rule (mqtt.c, amaran_mqtt_report_state): for "off" it updates just
  // the switch, brightness and kelvin keep their values from the last "on" —
  // after a bridge restart that means the defaults 100 % / 4700 K. Taking those
  // over would make the first turn of a dial jump an off light to near full
  // brightness.
  if (!l.on) return;
  // The light does not report exactly what was sent (5600 K -> 5617 K,
  // 60 % -> 59 %). Without a tolerance the display would keep drifting.
  if (Number.isFinite(l.brightness) && l.brightness > 0 && Math.abs(l.brightness - s.brightness) > 1) {
    s.brightness = clamp(l.brightness, BRI.min, BRI.max);
  }
  if (Number.isFinite(l.kelvin) && l.kelvin > 0) {
    s.kelvin = clamp(Math.round(l.kelvin / CCT.step) * CCT.step, s.cctMin, s.cctMax);
  }
}

let syncing = false;
async function syncFromBridge() {
  if (syncing || contexts.size === 0) return;
  syncing = true;
  try {
    const list = await fetchLights();
    if (!list) return;
    known = list;
    for (const ctx of contexts.values()) {
      if (!ctx.light) ctx.light = resolveLight(ctx.settings);   // first light, now that the list is known
      const l = list.find(x => x.key === ctx.light);
      const s = light(ctx.light);
      if (l && s) applyTruth(s, l);
    }
    refreshAll();
  } catch (_) {
    // Bridge unreachable: keep the last known state; the next tick tries again.
  } finally {
    syncing = false;
  }
}

function togglePower(s) {
  // Flip the display immediately so the key reacts under the finger.
  s.power = s.power === "on" ? "off" : "on";
  s.lastLocalChange = Date.now();
  refreshAll(); saveState(s);
  if (target().local) { sendPower(s); return; }   // Mac daemon: no state, no /toggle

  // The bridge flips the light itself and reports the new state. Until
  // 2026-09-13 the plugin first queried /lights and then sent on/off — two
  // round trips over the ESP32's Wi-Fi, which shares its antenna with the mesh.
  requestSend(s, () => post("/lights/" + encodeURIComponent(s.key) + "/toggle", null).then(r => {
    if (s.queued || !r || typeof r.on !== "boolean") return;   // another press is on its way
    const p = r.on ? "on" : "off";
    if (p !== s.power) { s.power = p; refreshAll(); saveState(s); }
  }));
}

// After a sleep cycle the GATT connection survives but the mesh session on top
// of it does not: the daemon then sends into the void and still reports
// success, because the protocol has no acknowledgement. The only visible sign is
// the jump in wall-clock time — free to detect, since the timer runs anyway.
// The guard then replaces the stale daemon.
let guardRunning = false;
let lastTick = Date.now();
function runGuard(reason) {
  if (!target().local) return;   // no local daemon, nothing to guard
  if (guardRunning) return;
  guardRunning = true;
  log(reason + " — starting " + GUARD);
  try {
    const child = spawn(GUARD, [], { stdio: "ignore" });
    child.on("exit", code => { guardRunning = false; log("guard finished (code " + code + ")"); });
    child.on("error", err => { guardRunning = false; log("guard not executable: " + err.message); });
  } catch (e) { guardRunning = false; log("guard could not start: " + e.message); }
}

let lastWake = 0;
function wakeDaemon() {
  if (!target().local || Date.now() - lastWake < 60000) return;
  lastWake = Date.now();
  log("daemon unreachable — starting " + DAEMON_APP);
  try { spawn("/usr/bin/open", ["-a", DAEMON_APP], { detached: true, stdio: "ignore" }).unref(); }
  catch (e) { log("could not start daemon: " + e.message); }
}

// Turning a dial produces many events, and a BLE command takes ~0.4 s. So: update
// the display immediately, debounce sending, and never run two requests for the
// same light at once — the latest value always wins.
const DEBOUNCE_MS = 90;
// A refused connection means the local daemon is not running (yet). The latest
// value waits and is sent again after a pause. Until 2026-09-13 the retry ran
// after 0 ms: with the daemon stopped that made about 40,000 attempts a minute —
// 81 minutes of it on 2026-09-11, 2.9 million log lines. After RETRY_MAX attempts the
// value is dropped — a light switching half a minute after the turn would only
// surprise.
const RETRY_MS = 1500;
const RETRY_MAX = 20;
function requestSend(s, fn) {
  // The timer below sets lastTick every two seconds. If it is overdue, we are
  // between two rounds after waking up — check before sending.
  if (WAKE_DRIFT_MS > 0 && Date.now() - lastTick > WATCH_MS + WAKE_DRIFT_MS) {
    runGuard("timer overdue while sending");
  }
  s.queued = fn;
  s.retries = 0;
  if (s.sendTimer || s.inFlight) return;
  // Leading edge: the first tick of a turn goes out without debouncing,
  // otherwise 90 ms of waiting would add to ~0.4 s of BLE latency and the light
  // would visibly lag behind the hand. Only the following ticks are collected.
  const idle = Date.now() - s.lastSend > 400;
  if (idle) runSend(s);
  else s.sendTimer = setTimeout(() => runSend(s), DEBOUNCE_MS);
}
function runSend(s) {
  s.sendTimer = null;
  if (s.inFlight || !s.queued) return;
  if (guardRunning) { s.sendTimer = setTimeout(() => runSend(s), 1500); return; }  // daemon is restarting
  const fn = s.queued; s.queued = null; s.inFlight = true; s.lastSend = Date.now();
  fn()
    .catch(err => {
      log("send failed (" + s.key + "): " + err.message);
      if (err.code !== "ECONNREFUSED") return;
      wakeDaemon();
      s.retryAt = Date.now() + RETRY_MS;
      if (s.queued) return;   // a newer value is already waiting
      if (++s.retries <= RETRY_MAX) s.queued = fn;
      else log("target still unreachable after " + RETRY_MAX + " attempts — command for " + s.key + " dropped");
    })
    .finally(() => {
      s.inFlight = false;
      if (s.queued) s.sendTimer = setTimeout(() => runSend(s), Math.max(0, s.retryAt - Date.now()));
    });
}

const lightPath = (s, cmd) => "/lights/" + encodeURIComponent(s.key) + "/" + cmd;
const sendBrightness = s => requestSend(s, () => post(lightPath(s, "brightness"), { value: s.brightness }));
// The Mac daemon reads `brightness`, the ESP32 reads `intensity` — each ignores
// the other's field. Sending only one would make the dial set the color and
// silently drop the brightness on the other target, with both answering 200.
const sendCct = s => requestSend(s, () => post(lightPath(s, "cct"),
  { brightness: s.brightness, intensity: s.brightness, kelvin: s.kelvin, gm: 0 }));
const sendPower = s => requestSend(s, () => post(lightPath(s, s.power), null));

// ── WebSocket client (RFC 6455, only what is needed here) ────────────────────
const sd = {
  ws: null, ready: false, closing: false, buf: Buffer.alloc(0), frag: [], fragOp: 0, uuid: null,
  send(obj) {
    this.raw(encodeFrame(JSON.stringify(obj)));
  },
  // Every write goes through here. writableEnded: after Stream Deck's close frame
  // the socket is ended but not yet destroyed; a write then raises "write after
  // end", and the error handler would exit with code 1 on a regular shutdown.
  raw(buf) {
    if (!this.ws || this.ws.destroyed || this.ws.writableEnded) return;
    this.ws.write(buf);
  },
};

function encodeFrame(str) {
  return encodeRaw(0x1, Buffer.from(str, "utf8"));
}

// A pong MUST echo the ping's payload. Without it Stream Deck considers the
// connection dead ("Socket unresponsive"), kills the process and, after a few
// attempts, disables the plugin as unstable.
function encodeRaw(opcode, payload) {
  const len = payload.length;
  const mask = crypto.randomBytes(4);
  let header;
  if (len < 126) { header = Buffer.alloc(2); header[1] = 0x80 | len; }
  else if (len < 65536) { header = Buffer.alloc(4); header[1] = 0x80 | 126; header.writeUInt16BE(len, 2); }
  else { header = Buffer.alloc(10); header[1] = 0x80 | 127; header.writeBigUInt64BE(BigInt(len), 2); }
  header[0] = 0x80 | opcode; // FIN + opcode
  const masked = Buffer.alloc(len);
  for (let i = 0; i < len; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

function parseFrames() {
  for (;;) {
    if (sd.closing) return;   // nothing after a close frame is meant for us
    if (sd.buf.length < 2) return;
    const b0 = sd.buf[0], b1 = sd.buf[1];
    const fin = (b0 & 0x80) !== 0, opcode = b0 & 0x0f;
    let len = b1 & 0x7f, off = 2;
    if (len === 126) { if (sd.buf.length < 4) return; len = sd.buf.readUInt16BE(2); off = 4; }
    else if (len === 127) { if (sd.buf.length < 10) return; len = Number(sd.buf.readBigUInt64BE(2)); off = 10; }
    // Stream Deck's messages are small; a huge announced length would only make
    // the buffer grow until the process dies.
    if (len > 1024 * 1024) { log("frame of " + len + " bytes refused"); sd.ws.destroy(); process.exit(1); }
    if ((b1 & 0x80) !== 0) { if (sd.buf.length < off + 4 + len) return; off += 4; } // the server does not mask
    if (sd.buf.length < off + len) return;
    const payload = sd.buf.subarray(off, off + len);
    sd.buf = sd.buf.subarray(off + len);

    if (opcode === 0x8) { sd.closing = true; sd.ws.end(); return; }
    if (opcode === 0x9) { sd.raw(encodeRaw(0xa, Buffer.from(payload))); continue; }
    if (opcode === 0xa) continue;
    if (opcode === 0x0) { sd.frag.push(Buffer.from(payload)); }
    else { sd.frag = [Buffer.from(payload)]; sd.fragOp = opcode; }
    if (!fin) continue;
    const full = Buffer.concat(sd.frag); sd.frag = [];
    if (sd.fragOp === 0x1) {
      let msg;
      try { msg = JSON.parse(full.toString("utf8")); } catch (e) { log("unreadable message: " + e.message); continue; }
      if (!msg || typeof msg !== "object") { log("message is not an object"); continue; }
      handleEvent(msg);
    }
  }
}

function connect({ port, uuid, registerEvent }) {
  const key = crypto.randomBytes(16).toString("base64");
  const sock = net.createConnection({ host: "127.0.0.1", port }, () => {
    sock.write(
      "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + port + "\r\n" +
      "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
      "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n"
    );
  });
  sd.ws = sock;
  sd.uuid = uuid;
  let handshakeDone = false;
  sock.on("data", chunk => {
    if (!handshakeDone) {
      sd.buf = Buffer.concat([sd.buf, chunk]);
      const idx = sd.buf.indexOf("\r\n\r\n");
      if (idx === -1) return;
      const head = sd.buf.subarray(0, idx).toString("utf8");
      sd.buf = sd.buf.subarray(idx + 4);
      if (!/^HTTP\/1\.1 101/.test(head)) { log("handshake rejected: " + head.split("\r\n")[0]); process.exit(1); }
      handshakeDone = true;
      sd.ready = true;
      sd.send({ event: registerEvent, uuid });
      sd.send({ event: "getGlobalSettings", context: uuid });
      log("registered (port " + port + ")");
      startTimer();
      if (sd.buf.length) parseFrames();
      return;
    }
    sd.buf = Buffer.concat([sd.buf, chunk]);
    parseFrames();
  });
  sock.on("error", err => {
    // Where the failing call came from: "write after end" at shutdown could not
    // be placed from the message alone.
    const ours = (err.stack || "").split("\n").slice(1).map(l => l.trim()).filter(l => !/\(node:|^at node:/.test(l));
    log("socket error: " + err.message + (ours.length ? " (" + ours.slice(0, 3).join(" < ") + ")" : ""));
    process.exit(1);
  });
  sock.on("close", () => { log("connection to Stream Deck closed"); process.exit(0); });
}

// ── Events ───────────────────────────────────────────────────────────────────
const contexts = new Map(); // context -> { action, light, settings }

// Which light an action controls: its own setting, else the default from older
// installs, else the first light the bridge or the daemon reported.
function resolveLight(settings) {
  const candidates = [settings && settings.light, process.env.AMARAN_LIGHT, PATHS.light, known[0] && known[0].key];
  for (const k of candidates) {
    if (!k) continue;
    if (validKey(k)) return k;
    log("ignored invalid light key " + JSON.stringify(String(k).slice(0, 80)));
  }
  return "";
}

function feedback(context) {
  const ctx = contexts.get(context);
  if (!ctx) return;
  const s = ctx.light ? light(ctx.light) : null;
  if (ctx.action === ACT_POWER) {
    // State 0 = off, 1 = on (order of the states in the manifest).
    sd.send({ event: "setState", context, payload: { state: s && s.power === "on" ? 1 : 0 } });
    return;
  }
  if (ctx.action === ACT_BRI) {
    sd.send({ event: "setFeedback", context, payload: {
      title: T.brightness,
      value: !s ? T.nolight : s.power === "off" ? T.off : s.brightness + " %",
      indicator: { value: !s || s.power === "off" ? 0 : s.brightness },
    }});
  } else if (ctx.action === ACT_CCT) {
    const pct = s ? Math.round((s.kelvin - s.cctMin) / (s.cctMax - s.cctMin) * 100) : 0;
    sd.send({ event: "setFeedback", context, payload: {
      title: T.cct,
      value: s ? s.kelvin + " K" : T.nolight,
      indicator: { value: clamp(pct, 0, 100) },
    }});
  }
}

function refreshAll() {
  for (const context of contexts.keys()) feedback(context);
}

// Every second tick (~4 s) fetch the real state from the bridge. Against the
// local daemon, watch the on/off files the switch bundles share instead
// (amaran-on.app / amaran-off.app / amaran-toggle.app).
let syncTick = 0;
function startTimer() {
  lastTick = Date.now();
  setInterval(() => {
    const now = Date.now();
    const drift = now - lastTick - WATCH_MS;
    lastTick = now;
    if (drift > WAKE_DRIFT_MS) {
      runGuard("time jump of " + Math.round(drift / 1000) + " s detected (sleep)");
    }
    if (contexts.size === 0) return;
    if (!target().local) {
      if (++syncTick % 2 === 0) syncFromBridge();
      return;
    }
    for (const s of lights.values()) {
      if (s.powerSeen === null) s.powerSeen = s.power;
      let p;
      try { p = fs.readFileSync(powerFile(s), "utf8").trim(); } catch (_) { continue; }
      if ((p === "on" || p === "off") && p !== s.powerSeen) {
        s.powerSeen = p; s.power = p;
        log("state changed from outside (" + s.key + "): " + p);
        refreshAll();
      }
    }
  }, WATCH_MS).unref();
}

// The property inspector asks for the lights at a (possibly not yet saved)
// address; it cannot fetch them itself because the bridge sends no CORS headers.
async function answerLights(action, context, payload) {
  const typed = String((payload && payload.host) || "").trim();
  let t = target();
  if (typed) {
    const [h, p] = typed.split(":");
    const local = h === "127.0.0.1" || h === "localhost";
    t = { host: h, port: Number(p) || (local ? 2708 : 80), local };
  }
  // `current` is the light the action controls right now, possibly from the
  // fallbacks in resolveLight. Without it the property inspector would preselect
  // the first light — and opening the settings of an older action would silently
  // switch it to a different light.
  const ctx = contexts.get(context);
  const reply = { type: "lights", host: t.host, port: t.port, local: t.local, lights: [], ok: false,
                  current: (ctx && ctx.light) || "" };
  try {
    // Bridge and Mac daemon both list their lights; only the bridge reports a
    // color temperature range, so cct_min/cct_max stay undefined for the daemon.
    const list = await fetchLights(t);
    reply.lights = list.map(l => ({ key: l.key, name: l.name, has_color: !!l.has_color,
                                    cct_min: l.cct_min, cct_max: l.cct_max }));
    reply.ok = true;
  } catch (e) {
    reply.error = e.message;
  }
  sd.send({ event: "sendToPropertyInspector", action, context, payload: reply });
}

function handleEvent(msg) {
  const { event, action, context, payload } = msg;
  if (event !== "didReceiveGlobalSettings") {
    log("event " + event + (payload && payload.ticks !== undefined ? " ticks=" + payload.ticks : ""));
  }
  const ctx = contexts.get(context);
  const s = ctx && ctx.light ? light(ctx.light) : null;
  switch (event) {
    case "didReceiveGlobalSettings":
      GLOBAL = (payload && payload.settings) || {};
      log("target: " + target().host + ":" + target().port);
      syncFromBridge();
      break;
    case "willAppear":
    case "didReceiveSettings": {
      const settings = (payload && payload.settings) || {};
      contexts.set(context, { action, light: resolveLight(settings), settings });
      log("action active: " + action + " → " + (contexts.get(context).light || "(no light yet)"));
      feedback(context);
      // Fetch the truth right away so the display is correct before the next
      // tick.
      syncFromBridge();
      break;
    }
    case "willDisappear":
      contexts.delete(context);
      break;
    case "sendToPlugin":
      if (payload && payload.type === "getLights") answerLights(action, context, payload);
      if (payload && payload.type === "settingsSaved") sd.send({ event: "getGlobalSettings", context: sd.uuid });
      break;
    case "dialRotate": {
      const ticks = Number(payload && payload.ticks) || 0;
      if (!ticks || !s) break;
      s.lastLocalChange = Date.now();
      if (ctx.action === ACT_BRI) {
        s.brightness = clamp(s.brightness + ticks * BRI.step, BRI.min, BRI.max);
        // Turning the dial of a light that is off switches it on — otherwise
        // you turn into nothing and the state drifts apart.
        if (s.power === "off") s.power = "on";
        refreshAll(); saveState(s); sendBrightness(s);
      } else if (ctx.action === ACT_CCT) {
        s.kelvin = clamp(s.kelvin + ticks * CCT.step, s.cctMin, s.cctMax);
        if (s.power === "off") s.power = "on";
        refreshAll(); saveState(s); sendCct(s);
      }
      break;
    }
    case "dialDown":
    case "touchTap":
      if (!s) break;
      if (ctx.action === ACT_BRI) {
        togglePower(s);
      } else if (ctx.action === ACT_CCT) {
        s.lastLocalChange = Date.now();
        s.kelvin = clamp(CCT.default, s.cctMin, s.cctMax);
        refreshAll(); saveState(s); sendCct(s);
      }
      break;
    case "keyDown":
      if (ctx && ctx.action === ACT_POWER && s) togglePower(s);
      break;
    case "keyUp":
      // Stream Deck advances multi-state keys by itself on release. That would
      // be guessing again — so reset to the real state right away.
      if (ctx && ctx.action === ACT_POWER) feedback(context);
      break;
    default:
      break;
  }
}

// ── Start ────────────────────────────────────────────────────────────────────
const argv = process.argv.slice(2);
const arg = name => { const i = argv.indexOf("-" + name); return i >= 0 ? argv[i + 1] : null; };
const port = Number(arg("port"));
const uuid = arg("pluginUUID");
const registerEvent = arg("registerEvent") || "registerPlugin";
try {
  const lang = String(JSON.parse(arg("info")).application.language || "").slice(0, 2);
  if (TEXTS[lang]) T = TEXTS[lang];
} catch (_) {}
if (!port || !uuid) { log("started without -port/-pluginUUID — exiting"); process.exit(1); }
connect({ port, uuid, registerEvent });
