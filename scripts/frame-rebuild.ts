/**
 * Rebuilds a mesh frame exactly as the daemon would send it — without
 * Bluetooth. That lets you compare a `payload=…` from ANOTHER computer's log
 * byte by byte: same keys, same sequence number, same frame. Any difference
 * points at the cause.
 *
 * Usage:  npx tsx scripts/frame-rebuild.ts <seq> [<dst>] [<ivIndex>] [on|off]
 */
import { readFileSync } from "fs";
import { MeshController, buildProxyPDU } from "../src/mesh-controller.js";
import * as telink from "../src/telink.js";

const cfg = JSON.parse(readFileSync(new URL("../lights.json", import.meta.url), "utf8"));
const c: any = new MeshController(cfg);

const seq = Number(process.argv[2]);
const dst = Number(process.argv[3] ?? 0x0028);
const iv = Number(process.argv[4] ?? 0);
const on = (process.argv[5] ?? "off") === "on";

const pdu = buildProxyPDU(c.appKey, c.aid, c.nid, c.encKey, c.privKey,
                          seq, 0x0001, dst, iv, 0x26, telink.onoff(on));
console.log(`seq=${seq} dst=0x${dst.toString(16).padStart(4, "0")} iv=${iv} ${on ? "on" : "off"}`);
console.log(pdu.toString("hex"));
