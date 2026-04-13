import { readFileSync } from "fs";
import { resolve } from "path";
import { homedir } from "os";

export interface BridgeConfig {
  watcher_url: string;
  bridge_port: number;
  poll_interval_ms: number;
  debounce_count: number;
  events_file: string;
  context_file: string;
}

export function loadConfig(): BridgeConfig {
  const raw = readFileSync(resolve(__dirname, "../config.json"), "utf-8");
  const config: BridgeConfig = JSON.parse(raw);
  config.events_file = config.events_file.replace("~", homedir());
  config.context_file = config.context_file.replace("~", homedir());
  return config;
}
