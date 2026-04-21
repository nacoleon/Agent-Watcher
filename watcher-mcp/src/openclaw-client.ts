/**
 * Direct WebSocket client for the OpenClaw gateway.
 *
 * Maintains a persistent connection to ws://127.0.0.1:18789 and provides
 * a fast `sendToAgent()` method that waits only for "accepted" (not the
 * full agent response). Replaces the slow `openclaw agent` CLI spawn.
 */

import WebSocket from "ws";
import { randomUUID } from "node:crypto";
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

interface GatewayAuth {
  deviceToken: string;
  deviceId: string;
  scopes: string[];
}

function readGatewayAuth(): GatewayAuth {
  try {
    const authPath = join(homedir(), ".openclaw", "identity", "device-auth.json");
    const auth = JSON.parse(readFileSync(authPath, "utf-8"));
    const operator = auth?.tokens?.operator;
    if (!operator?.token) throw new Error("no operator token");

    const devicePath = join(homedir(), ".openclaw", "identity", "device.json");
    const device = JSON.parse(readFileSync(devicePath, "utf-8"));

    return {
      deviceToken: operator.token,
      deviceId: device.deviceId,
      scopes: operator.scopes || ["operator.admin"],
    };
  } catch (err: any) {
    throw new Error(`Failed to read device auth from ~/.openclaw/identity/: ${err.message}`);
  }
}

export class OpenClawClient {
  private ws: WebSocket | null = null;
  private auth: GatewayAuth;
  private pending = new Map<string, PendingRequest>();
  private connected = false;
  private backoffMs = 1000;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private log: (category: string, message: string, data?: any) => void;

  constructor(logFn?: (category: string, message: string, data?: any) => void) {
    this.auth = readGatewayAuth();
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
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return;
      }

      // Handle connect challenge
      if (msg.event === "connect.challenge") {
        this.sendConnectAuth();
        return;
      }

      // Skip keepalive ticks and other events
      if (msg.event) return;

      // Handle RPC responses
      if (msg.id && this.pending.has(msg.id)) {
        const req = this.pending.get(msg.id)!;

        // For acceptOnly requests, resolve on "accepted" and ignore later responses
        if (req.acceptOnly && msg.ok && msg.payload?.status === "accepted") {
          this.pending.delete(msg.id);
          req.resolve(msg.payload);
          return;
        }

        // For acceptOnly, skip non-final responses silently
        if (req.acceptOnly && msg.ok && msg.payload?.status !== "ok") {
          return;
        }

        // Final response or error
        this.pending.delete(msg.id);
        if (msg.ok) {
          req.resolve(msg.payload);
        } else {
          req.reject(new Error(msg.error?.message || "gateway error"));
        }
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
      type: "req",
      id,
      method: "agent",
      params: {
        message,
        agentId,
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

  private sendConnectAuth(): void {
    const id = randomUUID();
    const frame = {
      type: "req",
      id,
      method: "connect",
      params: {
        minProtocol: 3,
        maxProtocol: 3,
        client: { id: "gateway-client", version: "1.0", mode: "backend", platform: process.platform },
        auth: { deviceToken: this.auth.deviceToken },
        role: "operator",
        scopes: this.auth.scopes,
        caps: [],
      },
    };

    this.pending.set(id, {
      resolve: (payload: any) => {
        this.connected = true;
        this.backoffMs = 1000;
        this.log("openclaw-ws", "Connected to gateway", { scopes: payload?.auth?.scopes, role: payload?.auth?.role });
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
    // Reject all pending requests
    for (const [id, req] of this.pending) {
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
