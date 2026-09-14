#!/usr/bin/env npx tsx
/**
 * Amaran Light CLI
 *
 * If the daemon is running: sends commands over the Unix socket (instant).
 * Otherwise: connects directly to the lights (slower, ~10s startup).
 *
 * Usage:
 *   amaran                      Interactive REPL
 *   amaran on [light]
 *   amaran off [light]
 *   amaran brightness 75 [light]
 *   amaran cct 80 5600 [+10] [light]
 *   amaran hsi 80 45 60 [light]
 *   amaran start                Start background daemon
 *   amaran stop                 Stop background daemon
 *   amaran setup                Run setup wizard
 *   amaran lights               List configured lights
 */

import * as net from "net";
import * as fs from "fs";
import * as readline from "readline";
import { spawn } from "child_process";
import * as path from "path";
import { fileURLToPath } from "url";
import { loadConfig, configExists } from "./config.js";
import { SOCKET_PATH, PID_PATH } from "./daemon-paths.js";
// Type-only: erased at runtime, so noble is still not loaded until needed.
import type { MeshController } from "./mesh-controller.js";
// MeshController and daemon are imported dynamically to avoid noble initialization
// interfering with the socket event loop when using the daemon.

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// ── Daemon client ─────────────────────────────────────────────────────────────

function isDaemonRunning(): boolean {
  return fs.existsSync(SOCKET_PATH);
}

function sendToDaemon(req: object): Promise<{ ok: boolean; result?: string; error?: string }> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection(SOCKET_PATH);
    let buf = "";
    let settled = false;
    // Must clear the timeout when done/fail fires — otherwise the timer keeps
    // the Node.js event loop alive until it fires, delaying process exit.
    let timer: ReturnType<typeof setTimeout>;
    const done = (v: any) => { if (!settled) { settled = true; clearTimeout(timer); socket.destroy(); resolve(v); } };
    const fail = (e: Error) => { if (!settled) { settled = true; clearTimeout(timer); socket.destroy(); reject(e); } };

    socket.on("connect", () => socket.write(JSON.stringify(req) + "\n"));
    socket.on("data", chunk => {
      buf += chunk.toString();
      const lines = buf.split("\n");
      buf = lines.pop() ?? "";
      for (const line of lines) {
        if (!line.trim()) continue;
        try { done(JSON.parse(line)); } catch { fail(new Error("Bad daemon response")); }
      }
    });
    socket.on("error", fail);
    socket.on("close", () => fail(new Error("Daemon closed connection unexpectedly")));
    timer = setTimeout(() => fail(new Error("Daemon timeout")), 15000);
  });
}

// ── Parse command from args array ─────────────────────────────────────────────

function parseCommand(args: string[]): { cmd: string; cmdArgs: string[]; light?: string } {
  const config = configExists() ? loadConfig() : null;
  const lightKeys = config?.lights.map(l => l.key) ?? [];

  // Last arg might be a light key
  let light: string | undefined;
  const last = args[args.length - 1];
  if (last && (lightKeys.includes(last) || last === "all")) {
    light = last;
    args = args.slice(0, -1);
  }

  const [cmd, ...cmdArgs] = args;
  return { cmd, cmdArgs, light };
}

// ── Direct connection (no daemon) ─────────────────────────────────────────────

async function runDirect(cmd: string, cmdArgs: string[], light?: string): Promise<void> {
  const config = loadConfig();
  const { MeshController } = await import("./mesh-controller.js");
  const ctrl = new MeshController(config);

  if (!(await ctrl.connect())) throw new Error("Could not connect to lights");
  await ctrl.waitForBeacon(4000);
  await ctrl.setupProxyFilter();

  try {
    await runCommandOnController(ctrl, cmd, cmdArgs, light);
    await new Promise(r => setTimeout(r, 1500));
  } finally {
    await ctrl.disconnect();
  }
}

async function runCommandOnController(
  ctrl: MeshController,
  cmd: string,
  cmdArgs: string[],
  lightKey?: string
): Promise<void> {
  const targets: number[] = (() => {
    if (!lightKey || lightKey === "all") return [0xffff];
    const l = ctrl.lights.find(l => l.key === lightKey);
    if (!l) throw new Error(`Unknown light: "${lightKey}". Known: ${ctrl.lights.map(l => l.key).join(", ")}`);
    return [l.address];
  })();

  for (const addr of targets) {
    const name = ctrl.lights.find(l => l.address === addr)?.name ?? (addr === 0xffff ? "all" : `0x${addr.toString(16)}`);
    switch (cmd) {
      case "on":
        console.log(`Turning ${name} ON`);
        await ctrl.setOnOffBlast(addr, true);
        break;
      case "off":
        console.log(`Turning ${name} OFF`);
        await ctrl.setOnOffBlast(addr, false);
        break;
      case "brightness": {
        const pct = parseFloat(cmdArgs[0]);
        if (isNaN(pct)) throw new Error("brightness requires a number 0-100");
        console.log(`${name} brightness → ${pct}%`);
        await ctrl.setBrightness(addr, pct);
        break;
      }
      case "cct": {
        const b = parseFloat(cmdArgs[0]), k = parseFloat(cmdArgs[1]);
        const gm = cmdArgs[2] ? parseFloat(cmdArgs[2]) : 0;
        if (isNaN(b) || isNaN(k)) throw new Error("cct requires brightness and kelvin");
        console.log(`${name} CCT → ${b}%, ${k}K, GM ${gm >= 0 ? "+" : ""}${gm}`);
        await ctrl.setCCT(addr, b, k, gm);
        break;
      }
      case "hsi":
      case "hsl": {
        const b = parseFloat(cmdArgs[0]), h = parseFloat(cmdArgs[1]), s = parseFloat(cmdArgs[2]);
        if (isNaN(b) || isNaN(h) || isNaN(s)) throw new Error("hsi requires brightness, hue, saturation");
        console.log(`${name} HSI → ${b}%, hue ${h}°, sat ${s}%`);
        await ctrl.setHSL(addr, b, h, s);
        break;
      }
      default:
        throw new Error(`Unknown command: ${cmd}. Try: on, off, brightness, cct, hsi`);
    }
  }

  // Nudge fixtures to broadcast their new state so the ESP32 bridge / desktop
  // app pick up the change immediately rather than on the next poll. Mirrors
  // the firmware's schedule_refresh().
  const refreshAddrs = targets.includes(0xffff)
    ? ctrl.lights.map((l: any) => l.address)
    : targets;
  for (const addr of refreshAddrs) {
    await ctrl.statusRequest(addr);
    await new Promise(r => setTimeout(r, 80));
  }
}

// ── REPL mode ─────────────────────────────────────────────────────────────────

async function runREPL(): Promise<void> {
  const config = loadConfig();

  if (isDaemonRunning()) {
    // Daemon is up — just use it, each command is instant
    console.log("Daemon running — commands are instant. Type 'exit' or Ctrl+C to quit.");
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: "amaran> " });
    rl.prompt();
    rl.on("line", async line => {
      const trimmed = line.trim();
      if (!trimmed || trimmed === "exit" || trimmed === "quit") { rl.close(); return; }
      const parts = trimmed.split(/\s+/);
      const { cmd, cmdArgs, light } = parseCommand(parts);
      try {
        const res = await sendToDaemon({ cmd, args: cmdArgs, light });
        if (!res.ok) console.error("Error:", res.error);
        else if (res.result && res.result !== "ok") console.log(res.result);
      } catch (e: any) {
        console.error("Error:", e.message);
      }
      rl.prompt();
    });
    rl.on("close", () => process.exit(0));
    return;
  }

  // No daemon — maintain a persistent direct connection for the REPL
  console.log("Connecting to lights (start daemon for instant commands)...");
  const { MeshController } = await import("./mesh-controller.js");
  const ctrl = new MeshController(config);
  if (!(await ctrl.connect())) throw new Error("Could not connect");
  await ctrl.waitForBeacon(4000);
  await ctrl.setupProxyFilter();
  console.log("Connected. Type 'exit' or Ctrl+C to quit.\n");

  const rl = readline.createInterface({ input: process.stdin, output: process.stdout, prompt: "amaran> " });
  rl.prompt();
  rl.on("line", async line => {
    const trimmed = line.trim();
    if (!trimmed) { rl.prompt(); return; }
    if (trimmed === "exit" || trimmed === "quit") { rl.close(); return; }
    const parts = trimmed.split(/\s+/);
    const { cmd, cmdArgs, light } = parseCommand(parts);
    try {
      await runCommandOnController(ctrl, cmd, cmdArgs, light);
    } catch (e: any) {
      console.error("Error:", e.message);
    }
    rl.prompt();
  });
  rl.on("close", async () => {
    await ctrl.disconnect();
    process.exit(0);
  });
}

// ── Daemon management ─────────────────────────────────────────────────────────

function startDaemon(): void {
  if (isDaemonRunning()) {
    console.log("Daemon already running. Stop it first: amaran stop");
    return;
  }
  const daemonScript = path.join(__dirname, "daemon.ts");
  const child = spawn("npx", ["tsx", daemonScript], {
    detached: true,
    stdio: "inherit",
  });
  child.unref();
  console.log(`Daemon starting (PID: ${child.pid})...`);
  console.log("Run 'amaran stop' to stop it.");
}

async function stopDaemon(): Promise<void> {
  if (!isDaemonRunning()) {
    console.log("Daemon is not running.");
    return;
  }
  try {
    const res = await sendToDaemon({ cmd: "stop", args: [] });
    if (res.ok) console.log("Daemon stopped.");
  } catch {
    // Daemon may have exited already
    if (fs.existsSync(SOCKET_PATH)) fs.unlinkSync(SOCKET_PATH);
    if (fs.existsSync(PID_PATH)) fs.unlinkSync(PID_PATH);
    console.log("Daemon stopped.");
  }
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
  const argv = process.argv.slice(2);

  // No args → REPL
  if (argv.length === 0) {
    if (!configExists()) {
      console.error("No lights.json found. Run: npm run setup");
      process.exit(1);
    }
    await runREPL();
    return;
  }

  const firstArg = argv[0];

  // Meta commands
  if (firstArg === "setup") {
    await import("./setup.js");   // setup.ts runs itself on import; it has no default export
    return;
  }

  if (firstArg === "start") { startDaemon(); return; }
  if (firstArg === "stop")  { await stopDaemon(); return; }

  if (firstArg === "lights") {
    const config = loadConfig();
    console.log("Configured lights:");
    for (const l of config.lights) {
      const hub = l.mac.toUpperCase() === config.relayHub.toUpperCase();
      console.log(`  ${l.key.padEnd(8)} ${l.name}  (addr ${l.address}, MAC ${l.mac})${hub ? "  ← relay hub" : ""}`);
    }
    return;
  }

  if (firstArg === "help" || firstArg === "--help" || firstArg === "-h") {
    const config = configExists() ? loadConfig() : null;
    const lightKeys = config?.lights.map(l => l.key).join("|") ?? "key|back|halo";
    console.log(`
Amaran Light Controller

Usage:  amaran [command] [args] [light]

Commands:
  on                       Turn light on
  off                      Turn light off
  brightness <0-100>       Set brightness
  cct <b> <kelvin> [gm]    Set CCT — brightness 0-100, kelvin 2500-7500, GM -50..+50
  hsi <b> <hue> <sat>      Set color — brightness 0-100, hue 0-360, saturation 0-100

  start                    Start background daemon (instant commands)
  stop                     Stop background daemon
  lights                   List configured lights
  setup                    Run setup wizard

Light target (optional, default = all):  ${lightKeys}

Examples:
  amaran on
  amaran brightness 75
  amaran cct 80 5600 +10 key
  amaran hsi 60 120 80
  amaran              (interactive REPL)
  amaran start        (then all commands are instant)
`);
    return;
  }

  // Light command — send to daemon if running, else direct
  if (!configExists()) {
    console.error("No lights.json found. Run: npm run setup");
    process.exit(1);
  }

  const { cmd, cmdArgs, light } = parseCommand([...argv]);

  if (isDaemonRunning()) {
    try {
      const res = await sendToDaemon({ cmd, args: cmdArgs, light });
      if (!res.ok) {
        console.error(res.error);
        process.exit(1);
      }
    } catch (e: any) {
      console.error("Daemon error:", e.message, "— falling back to direct connection");
      await runDirect(cmd, cmdArgs, light);
    }
  } else {
    await runDirect(cmd, cmdArgs, light);
  }
}

main().catch(err => {
  console.error(err.message);
  process.exit(1);
});
