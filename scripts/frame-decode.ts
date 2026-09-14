/**
 * Reads SEQ, SRC, TTL and NID from a sent mesh frame.
 *
 * The network header is only OBFUSCATED with PECB (AES-ECB with the privacy
 * key), not encrypted — with the key from lights.json it can be reversed
 * exactly. That makes a `payload=…` line from the daemon log readable without
 * guessing the sequence number.
 *
 * Usage:  npx tsx scripts/frame-decode.ts <hex> [<hex> …]
 */
import { readFileSync } from "fs";
import { createCipheriv } from "crypto";
import { MeshController } from "../src/mesh-controller.js";

const cfg = JSON.parse(readFileSync(new URL("../lights.json", import.meta.url), "utf8"));
const c: any = new MeshController(cfg);

function aesEcbEncrypt(key: Buffer, block: Buffer): Buffer {
  const ci = createCipheriv("aes-128-ecb", key, null);
  ci.setAutoPadding(false);
  return Buffer.concat([ci.update(block), ci.final()]);
}

for (const hex of process.argv.slice(2)) {
  const net = Buffer.from(hex, "hex").subarray(1);   // byte 0 = proxy header
  const iviNid = net[0];
  const obfuscated = net.subarray(1, 7);             // CTL|TTL, SEQ(3), SRC(2)
  const privacyRandom = net.subarray(7, 14);
  const iv = (c.ivIndex ?? 0) >>> 0;
  const pecb = aesEcbEncrypt(c.privKey, Buffer.concat([
    Buffer.alloc(5, 0),
    Buffer.from([(iv >>> 24) & 0xff, (iv >>> 16) & 0xff, (iv >>> 8) & 0xff, iv & 0xff]),
    privacyRandom,
  ]));
  const plain = Buffer.from(obfuscated.map((b, i) => b ^ pecb[i]));
  const seq = (plain[1] << 16) | (plain[2] << 8) | plain[3];
  const src = (plain[4] << 8) | plain[5];
  console.log(
    `${hex.slice(0, 12)}…  NID=0x${(iviNid & 0x7f).toString(16)} IVI=${iviNid >> 7}` +
    `  CTL=${plain[0] >> 7} TTL=${plain[0] & 0x7f}  SEQ=${seq}  SRC=0x${src.toString(16).padStart(4, "0")}`
  );
}
