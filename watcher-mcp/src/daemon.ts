#!/usr/bin/env node
/**
 * Watcher Daemon — standalone background poller
 *
 * Runs 24/7 via LaunchAgent. Polls the Watcher every 5s for:
 * - Voice audio (audio_ready) → transcribe → send to OpenClaw
 * - Person arrived/left → notify OpenClaw
 * - Message dismissals → trigger next queued message
 * - Device reboots → re-send current message
 *
 * Sends events to OpenClaw via `openclaw agent` CLI command.
 * Independent of the MCP server (which only runs during active conversations).
 */

import http from "node:http";
import { writeFileSync, unlinkSync, mkdirSync, appendFileSync, existsSync } from "node:fs";
import { tmpdir, homedir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync, execSync } from "node:child_process";

// --- Config ---
const WATCHER_URL = "http://10.0.0.40";
const POLL_INTERVAL_MS = 5000;
const DEBOUNCE_COUNT = 2;
const WHISPER_MODEL = "base.en";

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

// --- Send to OpenClaw ---
function sendToZidane(message: string): void {
  try {
    execSync(
      `openclaw agent --agent main -m ${JSON.stringify(message)} --deliver --timeout 60`,
      { encoding: "utf-8", timeout: 70000, stdio: ["pipe", "pipe", "pipe"] }
    );
    log("openclaw", `Sent to Zidane: "${message}"`);
  } catch (err: any) {
    log("error", "Failed to send to OpenClaw", { error: err.message?.slice(0, 200) });
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
    if (pollCount % 60 === 1) {
      log("poll", `alive — uptime=${status.uptime_seconds}s audio_ready=${status.audio_ready} person=${status.person_present}`);
    }

    // Reboot detection
    if (lastUptime !== null && status.uptime_seconds < lastUptime) {
      log("reboot", `Watcher rebooted (uptime ${lastUptime}s → ${status.uptime_seconds}s)`);
    }
    lastUptime = status.uptime_seconds;

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

setInterval(poll, POLL_INTERVAL_MS);
poll(); // first poll immediately
