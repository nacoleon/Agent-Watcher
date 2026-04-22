#!/usr/bin/env node
/**
 * Watcher Daemon — standalone background poller + message queue owner
 *
 * Runs 24/7 via LaunchAgent. Polls the Watcher every 5s for:
 * - Voice audio (audio_ready) → transcribe → send to OpenClaw
 * - Person arrived/left → notify OpenClaw
 * - Message dismissals → trigger next queued message
 * - Device reboots → re-send current message
 *
 * Exposes a localhost HTTP API on port 8378 for MCP servers to
 * enqueue messages and query queue state. This keeps the MCP servers
 * stateless (no poller, no timers) so zombie processes are harmless.
 *
 * Sends events to OpenClaw via `openclaw agent` CLI command.
 */

import http from "node:http";
import { writeFileSync, unlinkSync, mkdirSync, appendFileSync, existsSync } from "node:fs";
import { tmpdir, homedir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { WATCHER_URL, POLL_INTERVAL_MS, DEBOUNCE_COUNT, WHISPER_MODEL } from "./config.js";
import { OpenClawClient } from "./openclaw-client.js";

// --- Config ---
const DAEMON_API_PORT = 8378;

// --- Voice context (signals MCP tools that current response is to voice input) ---
let voiceContextActive = false;
let voiceContextTimer: ReturnType<typeof setTimeout> | null = null;
const VOICE_CONTEXT_TIMEOUT_MS = 120000;

function setVoiceContext(): void {
  voiceContextActive = true;
  if (voiceContextTimer) clearTimeout(voiceContextTimer);
  voiceContextTimer = setTimeout(() => {
    voiceContextActive = false;
    voiceContextTimer = null;
    log("voice-ctx", "Voice context expired (60s timeout)");
  }, VOICE_CONTEXT_TIMEOUT_MS);
  log("voice-ctx", "Voice context activated");
}

function clearVoiceContext(): void {
  voiceContextActive = false;
  if (voiceContextTimer) {
    clearTimeout(voiceContextTimer);
    voiceContextTimer = null;
  }
  log("voice-ctx", "Voice context cleared");
}

// --- Paths ---
const __dirname = dirname(fileURLToPath(import.meta.url));
const WHISPER_CPP_DIR = join(__dirname, "..", "node_modules", "whisper-node", "lib", "whisper.cpp");
const WHISPER_BIN = join(WHISPER_CPP_DIR, "main");
const WHISPER_MODEL_PATH = join(WHISPER_CPP_DIR, "models", `ggml-${WHISPER_MODEL}.bin`);
const LOG_DIR = join(homedir(), ".openclaw", "watcher-daemon-logs");

// --- Logging ---
function log(category: string, message: string, data?: any): void {
  const ts = new Date().toISOString();
  const line = data
    ? `${ts} [${category}] ${message} ${JSON.stringify(data)}`
    : `${ts} [${category}] ${message}`;
  console.log(line);
  try {
    mkdirSync(LOG_DIR, { recursive: true });
    const date = ts.slice(0, 10);
    appendFileSync(join(LOG_DIR, `daemon-${date}.log`), line + "\n");
  } catch {}
}

// --- HTTP helpers ---
function httpGet(path: string): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
    const req = http.request(
      { hostname: url.hostname, port: url.port || 80, path: url.pathname, method: "GET", timeout: 5000 },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try { resolve(JSON.parse(text)); } catch { resolve(text); }
        });
      }
    );
    req.on("error", reject);
    req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
    req.end();
  });
}

function httpGetBinary(path: string): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
    const req = http.request(
      { hostname: url.hostname, port: url.port || 80, path: url.pathname, method: "GET", timeout: 30000 },
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

function httpDelete(path: string): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
    const req = http.request(
      { hostname: url.hostname, port: url.port || 80, path: url.pathname, method: "DELETE", timeout: 5000 },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try { resolve(JSON.parse(text)); } catch { resolve(text); }
        });
      }
    );
    req.on("error", reject);
    req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
    req.end();
  });
}

function httpJsonRequest(method: string, path: string, body: object): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
    const data = JSON.stringify(body);
    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port || 80,
        path: url.pathname,
        method,
        headers: { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) },
        timeout: 5000,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try { resolve(JSON.parse(text)); } catch { resolve(text); }
        });
      }
    );
    req.on("error", reject);
    req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
    req.write(data);
    req.end();
  });
}

// --- Message Queue (owned by daemon, called by MCP via HTTP API) ---

interface QueuedMessage {
  text: string;
  level: string;
  state?: string;
}

const MAX_QUEUE_SIZE = 10;
let msgQueue: QueuedMessage[] = [];
let currentlyShowing = false;
let lastDismissCount = -1;
let lastMessage: QueuedMessage | null = null;

async function enqueue(
  text: string,
  level: string,
  state?: string
): Promise<{ sent: boolean; queued: boolean; position?: number; pending: number }> {
  if (msgQueue.length >= MAX_QUEUE_SIZE) {
    throw new Error(`queue full (${MAX_QUEUE_SIZE} messages pending)`);
  }

  if (!currentlyShowing) {
    if (state) await httpJsonRequest("PUT", "/api/agent-state", { state });
    await httpJsonRequest("POST", "/api/message", { text, level });
    currentlyShowing = true;
    lastMessage = { text, level, state };
    log("queue", `sent immediately: "${text.slice(0, 80)}"`, { state, level });
    return { sent: true, queued: false, pending: msgQueue.length };
  }

  msgQueue.push({ text, level, state });
  log("queue", `queued at position ${msgQueue.length}: "${text.slice(0, 80)}"`);
  return { sent: false, queued: true, position: msgQueue.length, pending: msgQueue.length };
}

async function onDismiss(dismissCount: number): Promise<void> {
  // First poll — just record the baseline
  if (lastDismissCount < 0) {
    lastDismissCount = dismissCount;
    return;
  }

  // No new dismissals
  if (dismissCount === lastDismissCount) return;

  // One or more dismissals happened since last poll
  const dismissals = dismissCount - lastDismissCount;
  lastDismissCount = dismissCount;
  currentlyShowing = false;
  log("queue", `dismiss detected (${dismissals} new, total=${dismissCount}, pending=${msgQueue.length})`);

  if (msgQueue.length > 0) {
    const next = msgQueue.shift()!;
    try {
      if (next.state) await httpJsonRequest("PUT", "/api/agent-state", { state: next.state });
      await httpJsonRequest("POST", "/api/message", { text: next.text, level: next.level });
      currentlyShowing = true;
      lastMessage = next;
      log("queue", `sent next from queue: "${next.text.slice(0, 80)}"`);
    } catch {
      msgQueue.unshift(next);
    }
  } else {
    log("queue", "queue empty after dismiss");
  }
}

async function onReboot(): Promise<void> {
  lastDismissCount = 0;

  // Resend the message that was showing when the device died
  if (currentlyShowing && lastMessage) {
    log("queue", "resending current message after reboot");
    try {
      if (lastMessage.state) await httpJsonRequest("PUT", "/api/agent-state", { state: lastMessage.state });
      await httpJsonRequest("POST", "/api/message", { text: lastMessage.text, level: lastMessage.level });
    } catch {
      // Device may still be booting — put it back in queue front
      msgQueue.unshift(lastMessage);
      currentlyShowing = false;
    }
  }
}

function getQueueState(): {
  currently_showing: boolean;
  pending: QueuedMessage[];
  count: number;
} {
  return {
    currently_showing: currentlyShowing,
    pending: [...msgQueue],
    count: msgQueue.length,
  };
}

// --- Whisper ---
function transcribe(wavPath: string): string {
  const output = execFileSync(WHISPER_BIN, [
    "-m", WHISPER_MODEL_PATH, "-f", wavPath, "-nt",
  ], { encoding: "utf-8", timeout: 30000, stdio: ["pipe", "pipe", "pipe"] });
  return output
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith("whisper_") && !l.startsWith("ggml_"))
    .map((l) => l.replace(/\[BLANK_AUDIO\]/g, "").trim())
    .filter((l) => l.length > 0)
    .join(" ")
    .trim();
}

// --- Send to OpenClaw (via persistent WebSocket) ---
const openclawClient = new OpenClawClient(log);
openclawClient.connect();

async function sendToZidane(message: string): Promise<void> {
  try {
    await openclawClient.sendToAgent(message, "main");
    log("openclaw", `Sent to Zidane: "${message}"`);
  } catch (err: any) {
    log("error", "Failed to send to OpenClaw", { error: err.message });
  }
}

// --- Main poller ---
let lastPresent: boolean | null = null;
let lastUptime: number | null = null;
let debounceCounter = 0;

let pollCount = 0;

async function poll(): Promise<void> {
  try {
    const status = await httpGet("/api/status");
    pollCount++;

    // Periodic status log every 60 polls (~5 min)
    if (pollCount % 300 === 1) {
      log("poll", `alive — uptime=${status.uptime_seconds}s audio_ready=${status.audio_ready} person=${status.person_present}`);
    }

    // Reboot detection — require a significant drop to avoid false positives
    // from 1s polling jitter (ESP32 uptime_seconds can flicker by ±1-2s)
    const REBOOT_THRESHOLD = 30;
    if (lastUptime !== null && status.uptime_seconds < lastUptime - REBOOT_THRESHOLD) {
      log("reboot", `Watcher rebooted (uptime ${lastUptime}s → ${status.uptime_seconds}s)`);
      await onReboot();
    }
    lastUptime = status.uptime_seconds;

    // Message queue dismiss detection
    await onDismiss(status.dismiss_count);

    // Voice audio pickup
    if (status.audio_ready) {
      try {
        log("audio", "Audio ready — fetching from Watcher");
        const audioBuffer = await httpGetBinary("/api/audio");
        await httpDelete("/api/audio");

        if (audioBuffer.length > 44) {
          const tmpDir = join(tmpdir(), "watcher-audio");
          mkdirSync(tmpDir, { recursive: true });
          const tmpFile = join(tmpDir, `voice-${Date.now()}.wav`);

          try {
            writeFileSync(tmpFile, audioBuffer);
            log("audio", `Transcribing ${audioBuffer.length} bytes`);
            const text = transcribe(tmpFile);
            log("audio", `Transcribed: "${text}"`);

            if (text) {
              setVoiceContext();
              sendToZidane(`[Voice from Watcher] ${text}`);
            }
          } finally {
            try { unlinkSync(tmpFile); } catch {}
          }
        }
      } catch (err: any) {
        log("error", "Audio pickup/transcription failed", { error: err.message });
      }
    }

    // Presence change detection
    if (lastPresent === null) {
      lastPresent = status.person_present;
      return;
    }

    if (status.person_present !== lastPresent) {
      debounceCounter++;
      if (debounceCounter >= DEBOUNCE_COUNT) {
        lastPresent = status.person_present;
        debounceCounter = 0;
        const event = status.person_present ? "person_arrived" : "person_left";
        log("presence", event);
        sendToZidane(`[Watcher event] ${event}`);
      }
    } else {
      debounceCounter = 0;
    }
  } catch (err: any) {
    // Only log occasionally to avoid spam
    if (Math.random() < 0.1) {
      log("poll", "Watcher unreachable", { error: err.message });
    }
  }
}

// --- Start ---
log("system", "Watcher daemon started");
log("system", `Polling ${WATCHER_URL} every ${POLL_INTERVAL_MS}ms`);
log("system", `Whisper model: ${WHISPER_MODEL_PATH}`);

if (!existsSync(WHISPER_BIN)) {
  log("error", `whisper.cpp binary not found at ${WHISPER_BIN}`);
  process.exit(1);
}
if (!existsSync(WHISPER_MODEL_PATH)) {
  log("error", `Whisper model not found at ${WHISPER_MODEL_PATH}`);
  process.exit(1);
}

// --- Daemon HTTP API (for MCP servers to enqueue messages) ---

function readBody(req: http.IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: string[] = [];
    req.on("data", (c: Buffer) => chunks.push(c.toString()));
    req.on("end", () => resolve(chunks.join("")));
    req.on("error", reject);
  });
}

const apiServer = http.createServer(async (req, res) => {
  try {
    if (req.method === "POST" && req.url === "/queue") {
      const body = await readBody(req);
      const { text, level, state } = JSON.parse(body);
      if (!text || !level) {
        res.writeHead(400, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: "text and level are required" }));
        return;
      }
      const result = await enqueue(text, level, state);
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(result));
    } else if (req.method === "GET" && req.url === "/queue") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify(getQueueState()));
    } else if (req.method === "GET" && req.url === "/voice-context") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ active: voiceContextActive }));
    } else if (req.method === "DELETE" && req.url === "/voice-context") {
      clearVoiceContext();
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
    } else {
      res.writeHead(404, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ error: "not found" }));
    }
  } catch (err: any) {
    res.writeHead(500, { "Content-Type": "application/json" });
    res.end(JSON.stringify({ error: err.message }));
  }
});

apiServer.listen(DAEMON_API_PORT, "127.0.0.1", () => {
  log("system", `Daemon API listening on http://127.0.0.1:${DAEMON_API_PORT}`);
});

apiServer.on("error", (err: any) => {
  log("error", `Daemon API server failed to start: ${err.message}`);
  if (err.code === "EADDRINUSE") {
    log("error", `Port ${DAEMON_API_PORT} already in use — another daemon instance running?`);
  }
});

// --- Start polling ---
setInterval(poll, POLL_INTERVAL_MS);
poll(); // first poll immediately
