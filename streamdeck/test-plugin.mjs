#!/usr/bin/env node
// End-to-end test of the Stream Deck plugin without Stream Deck and without a
// light: a fake Stream Deck (WebSocket server) and a fake bridge (HTTP) around a
// copy of the real plugin. Nothing is switched for real.
//
//   node streamdeck/test-plugin.mjs      (or: npm run test:plugin)
import http from "node:http";
import net from "node:net";
import crypto from "node:crypto";
import os from "node:os";
import fs from "node:fs";
import path from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PLUGIN_DIR = path.join(HERE, "plugin/com.arnebrockmann.amaran.sdPlugin");
const ACT = {
  power: "com.arnebrockmann.amaran.power",
  bri: "com.arnebrockmann.amaran.brightness",
  cct: "com.arnebrockmann.amaran.cct",
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const results = [];
function check(name, ok, detail = "") {
  results.push(!!ok);
  console.log((ok ? "PASS " : "FAIL ") + name + (detail ? "  — " + detail : ""));
}

// ── Fake bridge ──────────────────────────────────────────────────────────────
// Light a is off and comes first, light b is on and has a narrower range.
let lightsList;
const resetLights = () => {
  lightsList = [
    { key: "a", name: "Light A", has_color: true, has_gm: false, cct_min: 2700, cct_max: 6500, on: false, brightness: 40, mode: "cct", kelvin: 4000, gm: 0 },
    { key: "b", name: "Light B", has_color: false, has_gm: false, cct_min: 3200, cct_max: 5600, on: true, brightness: 70, mode: "cct", kelvin: 5000, gm: 0 },
  ];
};
const bridgeRequests = [];
const bridge = http.createServer((req, res) => {
  let body = "";
  req.on("data", c => { body += c; });
  req.on("end", () => {
    bridgeRequests.push({ method: req.method, url: req.url, body });
    res.setHeader("Content-Type", "application/json");
    if (req.method === "GET" && req.url === "/lights") return res.end(JSON.stringify(lightsList));
    const m = req.url.match(/^\/lights\/(\w+)\/toggle$/);
    if (m) {
      const l = lightsList.find(x => x.key === m[1]);
      l.on = !l.on;
      return res.end(JSON.stringify({ ok: true, on: l.on }));
    }
    res.end(JSON.stringify({ ok: true }));
  });
});
// Loopback, because a sandbox or firewall may block the LAN interface. The host
// is "127.1": it resolves to 127.0.0.1, but the plugin treats only the literal
// 127.0.0.1/localhost as the local Mac daemon — so this counts as a bridge.
await new Promise(r => bridge.listen(0, "127.0.0.1", r));
const BRIDGE = { host: "127.1", port: bridge.address().port };
const postsSince = n => bridgeRequests.slice(n).filter(r => r.method === "POST");

// ── Fake Mac daemon ──────────────────────────────────────────────────────────
// Same routes, but GET /lights wraps the list and carries no state.
const daemonRequests = [];
const daemon = http.createServer((req, res) => {
  let body = "";
  req.on("data", c => { body += c; });
  req.on("end", () => {
    daemonRequests.push({ method: req.method, url: req.url, body });
    res.setHeader("Content-Type", "application/json");
    if (req.method === "GET" && req.url === "/lights") {
      return res.end(JSON.stringify({ ok: true, daemon: true, lights: [
        { key: "keylight", name: "Key light", mac: "aa:bb:cc:dd:ee:ff", address: 37 },
        { key: "backlight", name: "Back light", mac: "02:00:00:00:00:01", address: 38 },
      ] }));
    }
    res.end(JSON.stringify({ ok: true }));
  });
});
await new Promise(r => daemon.listen(0, "127.0.0.1", r));
// The literal 127.0.0.1 is what makes the plugin treat a target as the daemon.
const DAEMON = { host: "127.0.0.1", port: daemon.address().port };

// ── Fake Stream Deck around a copy of the plugin ─────────────────────────────
async function startPlugin({ language, paths, halfOpen = false }) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "amaran-plugin-test-"));
  fs.mkdirSync(path.join(home, "Library/Logs"), { recursive: true });
  // A copy, so a paths.json can sit next to the plugin without touching the repo.
  const dir = path.join(home, "plugin.sdPlugin");
  fs.cpSync(PLUGIN_DIR, dir, { recursive: true });
  fs.rmSync(path.join(dir, "paths.json"), { force: true });
  if (paths) fs.writeFileSync(path.join(dir, "paths.json"), JSON.stringify(paths));

  const messages = [];
  let sock = null;
  // halfOpen: keep our side writable after the plugin ends its side, so frames
  // can still reach it — as when a message is already on its way.
  const server = net.createServer({ allowHalfOpen: halfOpen }, s => {
    sock = s;
    let rx = Buffer.alloc(0), shook = false;
    s.on("data", chunk => {
      rx = Buffer.concat([rx, chunk]);
      if (!shook) {
        const i = rx.indexOf("\r\n\r\n");
        if (i < 0) return;
        const key = /Sec-WebSocket-Key: (.+)\r\n/.exec(rx.subarray(0, i).toString())[1].trim();
        const accept = crypto.createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
        s.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
        rx = rx.subarray(i + 4);
        shook = true;
      }
      for (;;) {   // client frames are masked; the plugin sends no frames over 64 KB
        if (rx.length < 2) return;
        const opcode = rx[0] & 0x0f;
        let len = rx[1] & 0x7f, off = 2;
        if (len === 126) { if (rx.length < 4) return; len = rx.readUInt16BE(2); off = 4; }
        if (rx.length < off + 4 + len) return;
        const mask = rx.subarray(off, off + 4);
        const data = Buffer.from(rx.subarray(off + 4, off + 4 + len)).map((b, j) => b ^ mask[j % 4]);
        rx = rx.subarray(off + 4 + len);
        if (opcode === 0x1) { try { messages.push(JSON.parse(data.toString())); } catch (_) {} }
      }
    });
  });
  await new Promise(r => server.listen(0, "127.0.0.1", r));

  const child = spawn(process.execPath, [path.join(dir, "bin/plugin.js"),
    "-port", String(server.address().port), "-pluginUUID", "PLUGIN-UUID", "-registerEvent", "registerPlugin",
    "-info", JSON.stringify({ application: { language } })],
    // AMARAN_DAEMON_APP: if the plugin ever tried to start the real daemon bundle,
    // it must hit nothing.
    { env: { ...process.env, HOME: home, AMARAN_HOST: "", AMARAN_PORT: "", AMARAN_LIGHT: "",
             AMARAN_DAEMON_APP: path.join(home, "no-such-daemon.app") }, stdio: "inherit" });

  const send = obj => {
    const p = Buffer.from(JSON.stringify(obj));
    const h = p.length < 126 ? Buffer.from([0x81, p.length]) : Buffer.from([0x81, 126, p.length >> 8, p.length & 0xff]);
    sock.write(Buffer.concat([h, p]));
  };
  const waitFor = async (pred, from = 0, ms = 3000) => {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) {
      const m = messages.slice(from).find(pred);
      if (m) return m;
      await sleep(25);
    }
    return null;
  };
  const latest = pred => [...messages].reverse().find(pred);
  const askLights = async (action, context, host = "") => {
    const n = messages.length;
    send({ event: "sendToPlugin", action, context, payload: { type: "getLights", host } });
    return waitFor(m => m.event === "sendToPropertyInspector" && m.context === context, n, 6000);
  };
  const stop = () => {
    child.kill();
    server.close();
    const logFile = path.join(home, "Library/Logs/amaran-streamdeck-plugin.log");
    const logText = fs.existsSync(logFile) ? fs.readFileSync(logFile, "utf8") : "(no log)";
    fs.rmSync(home, { recursive: true, force: true });
    return logText;
  };
  const exited = new Promise(r => child.on("exit", code => r(code)));
  const sendRaw = buf => sock.write(buf);
  const endSocket = () => sock.end();
  const readLog = () => {
    const logFile = path.join(home, "Library/Logs/amaran-streamdeck-plugin.log");
    return fs.existsSync(logFile) ? fs.readFileSync(logFile, "utf8") : "";
  };
  const homeFiles = () => {
    const out = [];
    const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const f = path.join(d, e.name);
      if (f === dir) continue;                         // the plugin copy itself
      if (e.isDirectory()) walk(f); else out.push(path.relative(home, f));
    } };
    walk(home);
    return out;
  };
  return { messages, send, sendRaw, endSocket, waitFor, latest, askLights, stop, exited, readLog, homeFiles };
}

const short = v => JSON.stringify(v && (v.payload || v)).slice(0, 160);
let failedLog = "";

// ── Scenario 1: settings only, German ────────────────────────────────────────
resetLights();
let sd = await startPlugin({ language: "de" });
try {
  check("plugin registers", await sd.waitFor(m => m.event === "registerPlugin" && m.uuid === "PLUGIN-UUID"));
  check("plugin asks for global settings", await sd.waitFor(m => m.event === "getGlobalSettings" && m.context === "PLUGIN-UUID"));
  sd.send({ event: "didReceiveGlobalSettings", payload: { settings: BRIDGE } });
  sd.send({ event: "willAppear", action: ACT.power, context: "POWER", payload: { settings: { light: "b" } } });
  sd.send({ event: "willAppear", action: ACT.bri, context: "BRI", payload: { settings: {} } });
  sd.send({ event: "willAppear", action: ACT.cct, context: "CCT", payload: { settings: { light: "b" } } });
  await sleep(1200);

  const st = sd.latest(m => m.event === "setState" && m.context === "POWER");
  check("power key shows the real state of light b (on)", st && st.payload.state === 1, short(st));
  const bri = sd.latest(m => m.event === "setFeedback" && m.context === "BRI");
  check("dial without a setting falls back to the first light (a, off), German texts",
    bri && bri.payload.title === "Helligkeit" && bri.payload.value === "aus", short(bri));

  const pi = await sd.askLights(ACT.power, "POWER");
  check("settings page gets the light list with capabilities and the current light",
    pi && pi.payload.ok && pi.payload.lights.length === 2 && pi.payload.lights[1].cct_max === 5600 && pi.payload.current === "b", short(pi));
  const piBri = await sd.askLights(ACT.bri, "BRI");
  check("settings page of an action without a setting learns its fallback light (a)",
    piBri && piBri.payload.current === "a", short(piBri));

  let n = bridgeRequests.length;
  sd.send({ event: "keyDown", action: ACT.power, context: "POWER", payload: {} });
  await sleep(500);
  const tog = postsSince(n)[0];
  check("power key toggles light b", tog && tog.url === "/lights/b/toggle", tog && tog.url);
  const st2 = sd.latest(m => m.event === "setState" && m.context === "POWER");
  check("power key shows the state from the toggle answer (off)", st2 && st2.payload.state === 0, short(st2));

  n = bridgeRequests.length;
  sd.send({ event: "dialRotate", action: ACT.cct, context: "CCT", payload: { ticks: 20 } });
  await sleep(500);
  const cct = postsSince(n).find(r => r.url === "/lights/b/cct");
  check("color temperature clamped to light b's range (max 5600 K)", cct && JSON.parse(cct.body).kelvin === 5600, cct && cct.body);

  n = bridgeRequests.length;
  sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: 3 } });
  await sleep(500);
  const b1 = postsSince(n)[0];
  check("dial without a setting controls light a", b1 && b1.url === "/lights/a/brightness", b1 && b1.url);

  sd.send({ event: "didReceiveSettings", action: ACT.bri, context: "BRI", payload: { settings: { light: "b" } } });
  await sleep(700);
  n = bridgeRequests.length;
  sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: 1 } });
  await sleep(500);
  const b2 = postsSince(n)[0];
  check("a changed setting takes effect immediately (dial now controls b)", b2 && b2.url === "/lights/b/brightness", b2 && b2.url);

  const piLocal = await sd.askLights(ACT.power, "POWER", "127.0.0.1:1");
  check("a typed Mac daemon address is recognized, and a daemon that is not running reported",
    piLocal && piLocal.payload.local === true && piLocal.payload.ok === false && piLocal.payload.error, short(piLocal));
  const piBad = await sd.askLights(ACT.power, "POWER", "bridge.invalid");
  check("an unreachable bridge is reported as an error", piBad && piBad.payload.ok === false && piBad.payload.error, short(piBad));
} finally {
  const log = sd.stop();
  if (results.includes(false)) failedLog += "\n--- plugin log, scenario 1 ---\n" + log;
}

// ── Scenario 2: older install with a default light in paths.json, English ────
resetLights();
sd = await startPlugin({ language: "en", paths: { wakeDriftMs: 10000, light: "b" } });
try {
  await sd.waitFor(m => m.event === "getGlobalSettings");
  sd.send({ event: "didReceiveGlobalSettings", payload: { settings: BRIDGE } });
  sd.send({ event: "willAppear", action: ACT.bri, context: "BRI", payload: { settings: {} } });
  await sleep(1200);

  const bri = sd.latest(m => m.event === "setFeedback" && m.context === "BRI");
  check("dial without a setting uses the light from paths.json (b, 70 %), English texts",
    bri && bri.payload.title === "Brightness" && bri.payload.value === "70 %", short(bri));
  const pi = await sd.askLights(ACT.bri, "BRI");
  check("settings page preselects that light, not the first one", pi && pi.payload.current === "b", short(pi));
  const n = bridgeRequests.length;
  sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: -1 } });
  await sleep(500);
  const b = postsSince(n)[0];
  check("the dial controls light b", b && b.url === "/lights/b/brightness", b && b.url);
} finally {
  const log = sd.stop();
  if (results.includes(false)) failedLog += "\n--- plugin log, scenario 2 ---\n" + log;
}

// ── Scenario 3: no ESP32, the local Mac daemon ───────────────────────────────
sd = await startPlugin({ language: "en" });
try {
  await sd.waitFor(m => m.event === "getGlobalSettings");
  sd.send({ event: "didReceiveGlobalSettings", payload: { settings: DAEMON } });
  sd.send({ event: "willAppear", action: ACT.bri, context: "BRI", payload: { settings: {} } });
  sd.send({ event: "willAppear", action: ACT.power, context: "POWER", payload: { settings: { light: "backlight" } } });
  await sleep(1200);

  const pi = await sd.askLights(ACT.bri, "BRI");
  check("settings page gets the Mac daemon's lights (no color temperature range)",
    pi && pi.payload.local === true && pi.payload.ok && pi.payload.lights.map(l => l.key).join() === "keylight,backlight" &&
    pi.payload.lights[0].cct_min === undefined, short(pi));
  check("dial without a setting falls back to the daemon's first light", pi && pi.payload.current === "keylight", short(pi));

  let n = daemonRequests.length;
  sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: 2 } });
  await sleep(500);
  const b = daemonRequests.slice(n).find(r => r.method === "POST");
  check("the dial sends brightness to the daemon", b && b.url === "/lights/keylight/brightness" && JSON.parse(b.body).value === 62, b && b.url + " " + b.body);

  n = daemonRequests.length;
  sd.send({ event: "keyDown", action: ACT.power, context: "POWER", payload: {} });
  await sleep(500);
  const k = daemonRequests.slice(n).find(r => r.method === "POST");
  check("the power key sends on/off, not toggle (the daemon has no state)", k && k.url === "/lights/backlight/on", k && k.url);
} finally {
  const log = sd.stop();
  if (results.includes(false)) failedLog += "\n--- plugin log, scenario 3 ---\n" + log;
}

// ── Scenario 4: the Mac daemon is not running ────────────────────────────────
// Until 2026-09-13 a refused connection was retried after 0 ms — about 40,000
// attempts a minute.
sd = await startPlugin({ language: "en" });
try {
  await sd.waitFor(m => m.event === "getGlobalSettings");
  sd.send({ event: "didReceiveGlobalSettings", payload: { settings: { host: "127.0.0.1", port: 1 } } });
  sd.send({ event: "willAppear", action: ACT.bri, context: "BRI", payload: { settings: { light: "keylight" } } });
  await sleep(300);
  sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: 1 } });
  await sleep(3200);
  const attempts = (sd.readLog().match(/send failed \(keylight\)/g) || []).length;
  check("a stopped daemon is retried with a pause, not in a loop (2–4 attempts in 3 s)", attempts >= 2 && attempts <= 4, attempts + " attempts");
} finally {
  const log = sd.stop();
  if (results.includes(false)) failedLog += "\n--- plugin log, scenario 4 ---\n" + log.split("\n").slice(0, 30).join("\n");
}

// ── Scenario 5b: a hostile bridge (security audit 2026-09-13) ─────────────────
// Light keys become file names; answers must not be able to stall or bloat.
const hostile = http.createServer((req, res) => {
  if (req.url === "/lights" && req.headers.host.startsWith("127.1:")) {
    res.setHeader("Content-Type", "application/json");
    return res.end(JSON.stringify([
      { key: "/../../../../pwned", name: "evil", on: true, brightness: 50 },
      { key: "ok_light", name: "OK", on: true, brightness: 50 },
    ]));
  }
  res.writeHead(200, { "Content-Type": "application/json" });
  if (req.url === "/lights") {                       // reached as localhost: trickle forever
    const t = setInterval(() => res.write(" "), 400);
    return req.on("close", () => clearInterval(t));
  }
  res.end(JSON.stringify({ ok: true }));
});
await new Promise(r => hostile.listen(0, "127.0.0.1", r));
const huge = http.createServer((req, res) => {
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end("[" + " ".repeat(400 * 1024) + "]");
});
await new Promise(r => huge.listen(0, "127.0.0.1", r));
{
  const hostileRequests = [];
  hostile.on("request", req => hostileRequests.push(req.method + " " + req.url));
  sd = await startPlugin({ language: "en" });
  try {
    await sd.waitFor(m => m.event === "getGlobalSettings");
    sd.send({ event: "didReceiveGlobalSettings", payload: { settings: { host: "127.1", port: hostile.address().port } } });
    sd.send({ event: "willAppear", action: ACT.bri, context: "BRI", payload: { settings: { light: "/../../../../pwned2" } } });
    await sleep(1200);
    const pi = await sd.askLights(ACT.bri, "BRI");
    check("keys that are not plain names are dropped from the light list",
      pi && pi.payload.ok && pi.payload.lights.map(l => l.key).join() === "ok_light", short(pi));
    const n = hostileRequests.length;
    sd.send({ event: "dialRotate", action: ACT.bri, context: "BRI", payload: { ticks: 1 } });
    await sleep(900);
    const posts = hostileRequests.slice(n).filter(r => r.startsWith("POST"));
    check("an invalid key in the action settings is ignored (dial controls the first valid light)",
      posts.length === 1 && posts[0] === "POST /lights/ok_light/brightness", posts.join(", "));
    const written = sd.homeFiles().filter(f => /pwned/.test(f));
    check("no file is written outside the plugin's state folder", written.length === 0, written.join(", ") || "none");

    const t0 = Date.now();
    const slow = await sd.askLights(ACT.bri, "BRI", "localhost.:" + hostile.address().port);
    check("an answer trickling in byte by byte times out", slow && slow.payload.ok === false && /timeout/.test(slow.payload.error || ""),
      short(slow) + " after " + (Date.now() - t0) + " ms");
    const big = await sd.askLights(ACT.bri, "BRI", "127.1:" + huge.address().port);
    check("an oversized answer is refused", big && big.payload.ok === false && /too large/.test(big.payload.error || ""), short(big));
  } finally {
    const log = sd.stop();
    if (results.includes(false)) failedLog += "\n--- plugin log, scenario 5b ---\n" + log.split("\n").slice(0, 40).join("\n");
    hostile.close();
    huge.close();
  }
}

// ── Scenario 5: Stream Deck closes the connection ────────────────────────────
// A ping right after the close frame used to be answered on the ended socket:
// "write after end", exit code 1 on a regular shutdown.
sd = await startPlugin({ language: "en", halfOpen: true });
try {
  await sd.waitFor(m => m.event === "getGlobalSettings");
  sd.send({ event: "willAppear", action: ACT.power, context: "POWER", payload: { settings: { light: "b" } } });
  await sleep(200);
  sd.sendRaw(Buffer.from([0x88, 0x00]));                   // close
  await sleep(30);
  sd.sendRaw(Buffer.from([0x89, 0x02, 0x68, 0x69]));       // ping "hi"
  await sleep(100);
  sd.endSocket();
  const code = await Promise.race([sd.exited, sleep(3000).then(() => "still running")]);
  const log = sd.readLog();
  check("a close from Stream Deck ends the plugin cleanly (exit 0, no socket error)",
    code === 0 && !/socket error/.test(log), "exit " + code + (/socket error.*/.exec(log) || [""])[0]);
} finally {
  const log = sd.stop();
  if (results.includes(false)) failedLog += "\n--- plugin log, scenario 5 ---\n" + log;
  bridge.close();
  daemon.close();
}

const failed = results.filter(ok => !ok).length;
if (failed) console.log(failedLog.split("\n").slice(0, 80).join("\n"));
console.log(`\n${results.length - failed} of ${results.length} checks passed`);
process.exit(failed ? 1 : 0);
