import http from "node:http";
import { WATCHER_URL, REQUEST_TIMEOUT_MS } from "./config.js";

export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
  dialog_visible: boolean;
  dismiss_count: number;
  audio_ready?: boolean;
}

function request(method: string, path: string, body?: object): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
    const data = body ? JSON.stringify(body) : undefined;

    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port || 80,
        path: url.pathname,
        method,
        headers: data
          ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) }
          : {},
        timeout: REQUEST_TIMEOUT_MS,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try {
            resolve(JSON.parse(text));
          } catch {
            resolve(text);
          }
        });
      }
    );
    req.on("error", reject);
    req.on("timeout", () => {
      req.destroy();
      reject(new Error("timeout"));
    });
    if (data) req.write(data);
    req.end();
  });
}

export async function getStatus(): Promise<WatcherStatus> {
  return request("GET", "/api/status");
}

export async function setState(state: string): Promise<any> {
  return request("PUT", "/api/agent-state", { state });
}

export async function sendMessage(text: string, level: string = "info"): Promise<any> {
  return request("POST", "/api/message", { text, level });
}

export async function reboot(): Promise<any> {
  return request("POST", "/api/reboot");
}

export async function heartbeat(): Promise<any> {
  return request("POST", "/api/heartbeat");
}

export function getAudio(): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const url = new URL("/api/audio", WATCHER_URL);
    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port || 80,
        path: url.pathname,
        method: "GET",
        timeout: 30000,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => resolve(Buffer.concat(chunks)));
      }
    );
    req.on("error", reject);
    req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
    req.end();
  });
}

export async function clearAudio(): Promise<any> {
  return request("DELETE", "/api/audio");
}
