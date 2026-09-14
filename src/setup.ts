#!/usr/bin/env npx tsx
/**
 * Amaran Light Setup
 *
 * Reads mesh keys and light addresses from the Amaran Desktop app's SQLite
 * database, then writes lights.json for use by the other tools.
 *
 * Run: npm run setup
 */

import * as readline from "readline";
import { execFileSync } from "child_process";
import { findAmaranDB, saveConfig, type Config, type LightConfig } from "./config.js";

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
const ask = (q: string): Promise<string> =>
  new Promise(resolve => rl.question(q, a => resolve(a.trim())));

function sqliteQuery(dbPath: string, sql: string): string {
  // execFileSync instead of execSync: no shell parsing, so special characters in
  // the path cannot be interpreted as a command.
  return execFileSync("sqlite3", [dbPath, sql], { encoding: "utf-8" }).trim();
}

function extractFromDB(dbPath: string): Partial<Config> & { lights: LightConfig[] } {
  console.log(`Reading ${dbPath}...`);

  // Net key and App key are in the `mesh` table
  const meshRow = sqliteQuery(dbPath, "SELECT net_key, app_key FROM mesh LIMIT 1;");
  const [netKey, appKey] = meshRow.split("|").map(s => s.trim().toUpperCase());
  if (!netKey || !appKey) throw new Error("Could not read net_key/app_key from mesh table");

  // Nodes (lights) are in the `fixtures` table
  const nodesRaw = sqliteQuery(dbPath,
    "SELECT mac_address, node_address, name FROM fixtures WHERE node_address > 1 ORDER BY node_address;");

  const lights: LightConfig[] = [];
  for (const row of nodesRaw.split("\n").filter(Boolean)) {
    const [mac, addrStr, name] = row.split("|");
    const address = parseInt(addrStr, 10);
    if (isNaN(address) || address < 2 || !mac) continue;
    const displayName = name?.trim() || `Light ${address}`;
    const key = displayName.toLowerCase().replace(/[^a-z0-9]/g, "").slice(0, 8) || `light${address}`;
    lights.push({ key, name: displayName, mac: mac.trim(), address });
  }

  return { netKey, appKey, lights };
}

async function pickRelayHub(lights: LightConfig[]): Promise<string> {
  console.log("\nWhich light should be used as the BLE relay hub (the one your computer connects to)?");
  lights.forEach((l, i) => console.log(`  ${i + 1}. ${l.name}  (address ${l.address}, MAC ${l.mac})`));
  const ans = await ask(`Enter number [1]: `);
  const idx = (parseInt(ans, 10) || 1) - 1;
  return lights[Math.max(0, Math.min(idx, lights.length - 1))].mac;
}

async function manualSetup(): Promise<Config> {
  console.log("\nManual setup — enter your mesh config values.");
  console.log("(Find these in ~/Library/Application Support/amaran Desktop/*/amaran.db)\n");

  const netKey = (await ask("Net Key (hex, 32 chars): ")).toUpperCase();
  const appKey = (await ask("App Key (hex, 32 chars): ")).toUpperCase();

  const lights: LightConfig[] = [];
  let adding = true;
  while (adding) {
    console.log(`\nLight ${lights.length + 1}:`);
    const name = await ask("  Name (e.g. Key Light): ");
    const mac = (await ask("  MAC address (e.g. A4:C1:38:13:41:38): ")).toUpperCase();
    const address = parseInt(await ask("  Mesh address (e.g. 2): "), 10);
    const key = name.toLowerCase().replace(/[^a-z0-9]/g, "").slice(0, 8) || `light${address}`;
    lights.push({ key, name, mac, address });
    const more = await ask("Add another light? [y/N]: ");
    adding = more.toLowerCase().startsWith("y");
  }

  const relayHub = await pickRelayHub(lights);
  return { netKey, appKey, relayHub, lights };
}

async function main() {
  console.log("═══════════════════════════════════════");
  console.log("  Amaran Light Setup");
  console.log("═══════════════════════════════════════\n");

  let config: Config | null = null;

  // Try to auto-read from amaran.db
  const dbPath = findAmaranDB();
  if (dbPath) {
    console.log(`Found Amaran database at:\n  ${dbPath}\n`);
    const useDB = await ask("Auto-import from this database? [Y/n]: ");
    if (!useDB.toLowerCase().startsWith("n")) {
      try {
        const extracted = extractFromDB(dbPath);
        if (extracted.lights.length === 0) {
          console.log("No lights found in database. Falling back to manual setup.");
        } else {
          console.log(`\nFound ${extracted.lights.length} light(s):`);
          extracted.lights.forEach(l =>
            console.log(`  • ${l.name}  (address ${l.address})`)
          );
          const relayHub = await pickRelayHub(extracted.lights);
          config = { ...extracted, relayHub } as Config;
        }
      } catch (e: any) {
        console.error(`Failed to read database: ${e.message}`);
        console.log("Falling back to manual setup.\n");
      }
    }
  } else {
    console.log("Amaran Desktop database not found (is the app installed?).");
    console.log("Proceeding with manual setup.\n");
  }

  if (!config) {
    config = await manualSetup();
  }

  // Let user rename light keys
  console.log("\nLight shorthand keys (used as CLI targets, e.g. `amaran brightness 50 key`):");
  const renamed: LightConfig[] = [];
  for (const light of config.lights) {
    const newKey = await ask(`  ${light.name} → key [${light.key}]: `);
    renamed.push({ ...light, key: newKey || light.key });
  }
  config.lights = renamed;

  saveConfig(config);

  console.log(`\n✓ Saved to lights.json`);
  console.log(`  Relay hub: ${config.lights.find(l => l.mac.toLowerCase() === config!.relayHub.toLowerCase())?.name ?? config.relayHub}`);
  console.log(`  ${config.lights.length} light(s) configured`);
  console.log("\nTry it:  npm run mesh:on");

  rl.close();
}

main().catch(e => {
  console.error("Setup failed:", e.message);
  rl.close();
  process.exit(1);
});
