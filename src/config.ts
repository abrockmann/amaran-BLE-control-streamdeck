import * as fs from "fs";
import * as path from "path";

export interface LightConfig {
  key: string;     // short name used as CLI target (e.g. "key", "back", "halo")
  name: string;    // display name
  mac: string;     // BLE MAC address (e.g. "A4:C1:38:13:41:38")
  address: number; // mesh unicast address
}

export interface HttpConfig {
  port: number;   // default 2708
  host: string;   // default "127.0.0.1"; any other address requires apiKey
  apiKey?: string; // if set, requests must include Authorization: Bearer <apiKey>
}

export interface MqttConfig {
  broker: string;          // e.g. "mqtt://localhost:1883"
  username?: string;
  password?: string;
  discoveryPrefix?: string; // default "homeassistant"
  topicPrefix?: string;     // default "amaran"
}

export interface Config {
  netKey: string;        // hex string, 32 chars
  appKey: string;        // hex string, 32 chars
  relayHub: string;      // MAC address of the light to connect through as BLE Mesh Proxy
  lights: LightConfig[];
  http?: HttpConfig;
  mqtt?: MqttConfig;
}

const CONFIG_PATH = path.join(process.cwd(), "lights.json");

export function loadConfig(): Config {
  if (!fs.existsSync(CONFIG_PATH)) {
    throw new Error(
      "lights.json not found.\n" +
      "Run setup first:  npm run setup\n" +
      "Or copy the example: cp lights.example.json lights.json"
    );
  }
  try {
    return JSON.parse(fs.readFileSync(CONFIG_PATH, "utf-8")) as Config;
  } catch (e: any) {
    throw new Error(`lights.json is invalid: ${e.message}`);
  }
}

export function saveConfig(config: Config): void {
  // Holds the mesh netKey/appKey — readable by the owner only (0600).
  fs.writeFileSync(CONFIG_PATH, JSON.stringify(config, null, 2) + "\n", { encoding: "utf-8", mode: 0o600 });
  try { fs.chmodSync(CONFIG_PATH, 0o600); } catch { /* e.g. on non-POSIX file systems */ }
}

export function configExists(): boolean {
  return fs.existsSync(CONFIG_PATH);
}

export function findAmaranDB(): string | null {
  const base = path.join(
    process.env.HOME ?? "",
    "Library", "Application Support", "amaran Desktop"
  );
  if (!fs.existsSync(base)) return null;
  try {
    const hits = fs.readdirSync(base)
      .map(d => path.join(base, d, "amaran.db"))
      .filter(p => fs.existsSync(p));
    return hits[0] ?? null;
  } catch {
    return null;
  }
}
