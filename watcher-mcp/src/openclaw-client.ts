/**
 * Persistent WebSocket client for the OpenClaw gateway.
 *
 * Connects once, stays connected, auto-reconnects on failure.
 * Uses Ed25519 device identity signing for auth (required for write scope).
 * Returns as soon as gateway accepts the message (~100ms vs ~10s for CLI).
 */

import WebSocket from "ws";
import { randomUUID } from "node:crypto";
import crypto from "node:crypto";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";

const GATEWAY_URL = "ws://127.0.0.1:18789";
const MAX_BACKOFF_MS = 30000;
const ACCEPTED_TIMEOUT_MS = 15000;

interface PendingRequest {
  resolve: (payload: any) => void;
  reject: (err: Error) => void;
  acceptOnly: boolean;
}

interface DeviceIdentity {
  deviceId: string;
  publicKeyPem: string;
  privateKeyPem: string;
}

// --- Crypto helpers (matching gateway protocol) ---

function base64UrlEncode(buf: Buffer): string {
  return buf.toString("base64").replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/g, "");
}

function derivePublicKeyRaw(publicKeyPem: string): Buffer {
  const spki = crypto.createPublicKey(publicKeyPem).export({ type: "spki", format: "der" });
  // Ed25519 SPKI prefix is 12 bytes, raw key is 32 bytes
  const ED25519_SPKI_PREFIX_LEN = 12;
  if (spki.length === ED25519_SPKI_PREFIX_LEN + 32) {
    return spki.subarray(ED25519_SPKI_PREFIX_LEN);
  }
  return spki;
}

function signPayload(privateKeyPem: string, payload: string): string {
  const key = crypto.createPrivateKey(privateKeyPem);
  return base64UrlEncode(crypto.sign(null, Buffer.from(payload, "utf8"), key));
}

function buildPayloadV3(opts: {
  deviceId: string; clientId: string; clientMode: string;
  role: string; scopes: string[]; signedAtMs: number;
  token: string | null; nonce: string; platform: string;
  deviceFamily: string;
}): string {
  return [
    "v3", opts.deviceId, opts.clientId, opts.clientMode,
    opts.role, opts.scopes.join(","), String(opts.signedAtMs),
    opts.token ?? "", opts.nonce,
    opts.platform.toLowerCase(), opts.deviceFamily.toLowerCase(),
  ].join("|");
}

// --- Config loading ---

function loadDeviceIdentity(): DeviceIdentity {
  const idPath = join(homedir(), ".openclaw", "identity", "device.json");
  const data = JSON.parse(readFileSync(idPath, "utf-8"));
  if (!data.deviceId || !data.privateKeyPem || !data.publicKeyPem) {
    throw new Error("incomplete device identity");
  }
  return data;
}

function loadGatewayToken(): string | null {
  try {
    const plistPath = join(homedir(), "Library", "LaunchAgents", "ai.openclaw.gateway.plist");
    const plist = readFileSync(plistPath, "utf-8");
    const match = plist.match(/<key>OPENCLAW_GATEWAY_TOKEN<\/key>\s*<string>([^<]+)<\/string>/);
    return match?.[1] ?? null;
  } catch {
    return null;
  }
}

// --- Client ---

export class OpenClawClient {
  private ws: WebSocket | null = null;
  private device: DeviceIdentity;
  private gatewayToken: string | null;
  private pending = new Map<string, PendingRequest>();
  private connected = false;
  private backoffMs = 1000;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private log: (category: string, message: string, data?: any) => void;

  constructor(logFn?: (category: string, message: string, data?: any) => void) {
    this.device = loadDeviceIdentity();
    this.gatewayToken = loadGatewayToken();
    this.log = logFn || ((_c, msg) => console.log(`[openclaw-ws] ${msg}`));
  }

  connect(): void {
    if (this.ws) return;

    this.ws = new WebSocket(GATEWAY_URL, { maxPayload: 25 * 1024 * 1024 });

    this.ws.on("open", () => {
      this.log("openclaw-ws", "WebSocket opened, waiting for challenge");
    });

    this.ws.on("message", (data) => {
      let msg: any;
      try { msg = JSON.parse(data.toString()); } catch { return; }

      if (msg.event === "connect.challenge") {
        this.handleChallenge(msg.payload.nonce);
        return;
      }

      if (msg.event) return; // skip ticks etc.

      if (msg.id && this.pending.has(msg.id)) {
        const req = this.pending.get(msg.id)!;

        if (req.acceptOnly && msg.ok && msg.payload?.status === "accepted") {
          this.pending.delete(msg.id);
          req.resolve(msg.payload);
          return;
        }

        if (req.acceptOnly && msg.ok && msg.payload?.status !== "ok") return;

        this.pending.delete(msg.id);
        if (msg.ok) req.resolve(msg.payload);
        else req.reject(new Error(msg.error?.message || "gateway error"));
      }
    });

    this.ws.on("close", () => {
      this.log("openclaw-ws", "WebSocket closed");
      this.cleanup();
      this.scheduleReconnect();
    });

    this.ws.on("error", (err) => {
      this.log("openclaw-ws", "WebSocket error", { error: err.message });
      this.cleanup();
      this.scheduleReconnect();
    });
  }

  async sendToAgent(message: string, agentId: string = "main"): Promise<void> {
    if (!this.connected || !this.ws) {
      throw new Error("Not connected to gateway");
    }

    const id = randomUUID();
    const frame = {
      type: "req", id, method: "agent",
      params: {
        message, agentId,
        deliver: true,
        idempotencyKey: randomUUID(),
      },
    };

    return new Promise<void>((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("Timed out waiting for accepted"));
      }, ACCEPTED_TIMEOUT_MS);

      this.pending.set(id, {
        resolve: () => { clearTimeout(timeout); resolve(); },
        reject: (err) => { clearTimeout(timeout); reject(err); },
        acceptOnly: true,
      });

      this.ws!.send(JSON.stringify(frame));
    });
  }

  private handleChallenge(nonce: string): void {
    const id = randomUUID();
    const role = "operator";
    const scopes = ["operator.admin", "operator.read", "operator.write"];
    const signedAtMs = Date.now();

    const payload = buildPayloadV3({
      deviceId: this.device.deviceId,
      clientId: "gateway-client",
      clientMode: "backend",
      role,
      scopes,
      signedAtMs,
      token: this.gatewayToken,
      nonce,
      platform: process.platform,
      deviceFamily: "",
    });

    const signature = signPayload(this.device.privateKeyPem, payload);
    const publicKey = base64UrlEncode(derivePublicKeyRaw(this.device.publicKeyPem));

    const frame = {
      type: "req", id, method: "connect",
      params: {
        minProtocol: 3,
        maxProtocol: 3,
        client: { id: "gateway-client", version: "1.0", mode: "backend", platform: process.platform },
        auth: { token: this.gatewayToken },
        role,
        scopes,
        caps: [],
        device: {
          id: this.device.deviceId,
          publicKey,
          signature,
          signedAt: signedAtMs,
          nonce,
        },
      },
    };

    this.pending.set(id, {
      resolve: (payload: any) => {
        this.connected = true;
        this.backoffMs = 1000;
        this.log("openclaw-ws", "Connected to gateway", {
          scopes: payload?.auth?.scopes,
          role: payload?.auth?.role,
        });
      },
      reject: (err) => {
        this.log("openclaw-ws", "Auth failed", { error: err.message });
        this.ws?.close();
      },
      acceptOnly: false,
    });

    this.ws!.send(JSON.stringify(frame));
  }

  private cleanup(): void {
    this.connected = false;
    this.ws = null;
    for (const [, req] of this.pending) {
      req.reject(new Error("connection lost"));
    }
    this.pending.clear();
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) return;
    this.log("openclaw-ws", `Reconnecting in ${this.backoffMs}ms`);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, this.backoffMs);
    this.backoffMs = Math.min(this.backoffMs * 2, MAX_BACKOFF_MS);
  }
}
